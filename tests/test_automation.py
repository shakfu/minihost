"""Tests for minihost.automation module."""

import json
import os
from unittest.mock import MagicMock

import pytest

from minihost.automation import (
    _interpolate_keyframes,
    _parse_time_key,
    find_param_by_name,
    parse_automation_file,
    parse_param_arg,
)


# -- Helpers --


def _make_mock_plugin(params=None):
    """Create a mock plugin with given parameter names and behaviors."""
    if params is None:
        params = [
            {"name": "Mix", "label": "%"},
            {"name": "Feedback", "label": ""},
            {"name": "Cutoff", "label": "Hz"},
        ]

    plugin = MagicMock()
    plugin.num_params = len(params)

    def get_param_info(idx):
        return params[idx]

    def param_from_text(idx, text):
        # Simple mock: try float parsing, else raise
        try:
            return float(text)
        except ValueError:
            # Simulate text lookup for known values
            known = {"Off": 0.0, "On": 1.0, "Half": 0.5, "Moderate": 0.6}
            if text in known:
                return known[text]
            raise ValueError(f"Unknown text: {text}")

    plugin.get_param_info = MagicMock(side_effect=get_param_info)
    plugin.param_from_text = MagicMock(side_effect=param_from_text)
    return plugin


# -- Tests for _parse_time_key --


class TestParseTimeKey:
    def test_sample_offset(self):
        assert _parse_time_key("1000", 48000, 96000) == 1000

    def test_seconds(self):
        assert _parse_time_key("1.5s", 48000, 96000) == 72000

    def test_percent(self):
        assert _parse_time_key("50%", 48000, 100000) == 50000

    def test_zero_percent(self):
        assert _parse_time_key("0%", 48000, 100000) == 0

    def test_100_percent(self):
        assert _parse_time_key("100%", 48000, 100000) == 100000

    def test_fractional_seconds(self):
        assert _parse_time_key("0.5s", 44100, 44100) == 22050

    def test_whitespace_handling(self):
        assert _parse_time_key(" 1000 ", 48000, 96000) == 1000
        assert _parse_time_key(" 1.0s ", 48000, 96000) == 48000


# -- Tests for _interpolate_keyframes --


class TestInterpolateKeyframes:
    def test_single_keyframe(self):
        result = _interpolate_keyframes([(0, 0.5)], 10000, 512)
        assert result == [(0, 0.5)]

    def test_two_keyframes_linear(self):
        result = _interpolate_keyframes([(0, 0.0), (1024, 1.0)], 2048, 512)
        # Should have start, interpolated at 512, and end
        assert result[0] == (0, 0.0)
        assert result[-1] == (1024, 1.0)

        # Check interpolated value at sample 512
        mid_values = [v for s, v in result if s == 512]
        assert len(mid_values) == 1
        assert abs(mid_values[0] - 0.5) < 0.01

    def test_empty_keyframes(self):
        assert _interpolate_keyframes([], 10000, 512) == []

    def test_three_keyframes(self):
        result = _interpolate_keyframes(
            [(0, 0.0), (1024, 0.5), (2048, 1.0)], 3000, 1024
        )
        # Should contain at least start of each segment and end
        sample_offsets = [s for s, v in result]
        assert 0 in sample_offsets
        assert 1024 in sample_offsets
        assert 2048 in sample_offsets

    def test_values_increase_monotonically_for_ramp(self):
        result = _interpolate_keyframes([(0, 0.0), (4096, 1.0)], 4096, 512)
        values = [v for _, v in result]
        for i in range(1, len(values)):
            assert values[i] >= values[i - 1]


# -- Tests for find_param_by_name --


class TestFindParamByName:
    def test_exact_match(self):
        plugin = _make_mock_plugin()
        assert find_param_by_name(plugin, "Mix") == 0
        assert find_param_by_name(plugin, "Feedback") == 1
        assert find_param_by_name(plugin, "Cutoff") == 2

    def test_case_insensitive(self):
        plugin = _make_mock_plugin()
        assert find_param_by_name(plugin, "mix") == 0
        assert find_param_by_name(plugin, "MIX") == 0
        assert find_param_by_name(plugin, "feedback") == 1

    def test_not_found_raises(self):
        plugin = _make_mock_plugin()
        with pytest.raises(ValueError, match="Parameter not found"):
            find_param_by_name(plugin, "NonExistent")


# -- Tests for parse_param_arg --


class TestParseParamArg:
    def test_name_value(self):
        plugin = _make_mock_plugin()
        idx, val = parse_param_arg("Mix:0.5", plugin)
        assert idx == 0
        assert abs(val - 0.5) < 1e-6

    def test_name_value_normalized(self):
        plugin = _make_mock_plugin()
        idx, val = parse_param_arg("Feedback:0.7:n", plugin)
        assert idx == 1
        assert abs(val - 0.7) < 1e-6

    def test_name_text_value(self):
        plugin = _make_mock_plugin()
        idx, val = parse_param_arg("Mix:Moderate", plugin)
        assert idx == 0
        assert abs(val - 0.6) < 1e-6

    def test_invalid_format_no_colon(self):
        plugin = _make_mock_plugin()
        with pytest.raises(ValueError, match="Invalid parameter format"):
            parse_param_arg("Mix", plugin)

    def test_unknown_param_name(self):
        plugin = _make_mock_plugin()
        with pytest.raises(ValueError, match="Parameter not found"):
            parse_param_arg("UnknownParam:0.5", plugin)


# -- Tests for parse_automation_file --


class TestParseAutomationFile:
    def test_static_numeric_value(self, tmp_path):
        plugin = _make_mock_plugin()
        auto_file = tmp_path / "auto.json"
        auto_file.write_text(json.dumps({"Mix": 0.5}))

        changes = parse_automation_file(auto_file, plugin, 48000, 96000, block_size=512)
        assert len(changes) == 1
        assert changes[0] == (0, 0, 0.5)

    def test_static_text_value(self, tmp_path):
        plugin = _make_mock_plugin()
        auto_file = tmp_path / "auto.json"
        auto_file.write_text(json.dumps({"Mix": "Half"}))

        changes = parse_automation_file(auto_file, plugin, 48000, 96000, block_size=512)
        assert len(changes) == 1
        assert changes[0][0] == 0
        assert changes[0][1] == 0
        assert abs(changes[0][2] - 0.5) < 1e-6

    def test_keyframes(self, tmp_path):
        plugin = _make_mock_plugin()
        auto_file = tmp_path / "auto.json"
        auto_file.write_text(json.dumps({"Mix": {"0": 0.0, "1.0s": 1.0}}))

        changes = parse_automation_file(auto_file, plugin, 48000, 96000, block_size=512)
        # Should have at least start and end
        assert len(changes) >= 2
        # First change at sample 0
        assert changes[0][0] == 0
        assert changes[0][1] == 0
        assert abs(changes[0][2]) < 1e-6
        # Last change at sample 48000
        assert changes[-1][0] == 48000
        assert abs(changes[-1][2] - 1.0) < 1e-6

    def test_keyframes_with_percent(self, tmp_path):
        plugin = _make_mock_plugin()
        auto_file = tmp_path / "auto.json"
        auto_file.write_text(
            json.dumps({"Feedback": {"0%": 0.0, "50%": 0.5, "100%": 1.0}})
        )

        total = 96000
        changes = parse_automation_file(auto_file, plugin, 48000, total, block_size=512)
        # Check start
        assert changes[0] == (0, 1, 0.0)
        # Check end
        assert changes[-1] == (total, 1, 1.0)
        # Check midpoint exists
        mid_changes = [(s, i, v) for s, i, v in changes if s == total // 2]
        assert len(mid_changes) == 1
        assert abs(mid_changes[0][2] - 0.5) < 1e-6

    def test_multiple_params(self, tmp_path):
        plugin = _make_mock_plugin()
        auto_file = tmp_path / "auto.json"
        auto_file.write_text(
            json.dumps(
                {
                    "Mix": 0.5,
                    "Feedback": 0.8,
                }
            )
        )

        changes = parse_automation_file(auto_file, plugin, 48000, 96000, block_size=512)
        assert len(changes) == 2
        indices = {c[1] for c in changes}
        assert indices == {0, 1}

    def test_file_not_found(self):
        plugin = _make_mock_plugin()
        with pytest.raises(FileNotFoundError):
            parse_automation_file("/nonexistent/auto.json", plugin, 48000, 96000)

    def test_invalid_json_structure(self, tmp_path):
        plugin = _make_mock_plugin()
        auto_file = tmp_path / "auto.json"
        auto_file.write_text("[1, 2, 3]")

        with pytest.raises(ValueError, match="JSON object"):
            parse_automation_file(auto_file, plugin, 48000, 96000)

    def test_sorted_output(self, tmp_path):
        plugin = _make_mock_plugin()
        auto_file = tmp_path / "auto.json"
        auto_file.write_text(
            json.dumps(
                {
                    "Mix": {"48000": 1.0, "0": 0.0},
                    "Feedback": 0.5,
                }
            )
        )

        changes = parse_automation_file(auto_file, plugin, 48000, 96000, block_size=512)
        # Verify sorted by sample offset
        offsets = [c[0] for c in changes]
        assert offsets == sorted(offsets)


# -- Integration tests (require real plugin) --


@pytest.fixture
def test_plugin_path():
    path = os.environ.get("MINIHOST_TEST_PLUGIN")
    if not path:
        pytest.skip("MINIHOST_TEST_PLUGIN not set")
    return path


class TestFindParamByNameIntegration:
    def test_with_real_plugin(self, test_plugin_path):
        import minihost

        plugin = minihost.Plugin(test_plugin_path)
        if plugin.num_params == 0:
            pytest.skip("Plugin has no parameters")

        # Get first param name
        info = plugin.get_param_info(0)
        name = info["name"]

        # Should find it
        idx = find_param_by_name(plugin, name)
        assert idx == 0

        # Case-insensitive
        idx = find_param_by_name(plugin, name.lower())
        assert idx == 0
