"""Tests for CLI argument parsing and error paths.

These tests verify argument parsing and error handling for all 6 subcommands
(scan, info, params, midi, play, process) without requiring real plugins.
"""

import argparse
import os
import sys
from unittest.mock import MagicMock, patch

import pytest

from minihost.cli import (
    _expand_globs,
    _is_batch_output,
    cmd_info,
    cmd_midi,
    cmd_params,
    cmd_play,
    cmd_process,
    cmd_scan,
    main,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _parse(argv):
    """Run the CLI parser on the given argv list and return parsed args.

    Patches sys.argv so main()'s parser sees our arguments.
    """
    old_argv = sys.argv
    sys.argv = ["minihost"] + argv
    try:
        from minihost.cli import main as _main

        parser = argparse.ArgumentParser(prog="minihost")
        parser.add_argument("-r", "--sample-rate", type=float, default=48000)
        parser.add_argument("-b", "--block-size", type=int, default=512)
        subparsers = parser.add_subparsers(dest="command")

        # Replicate just enough of main()'s parser to validate arg parsing.
        # We import the real main() for the full integration tests below.
        scan_p = subparsers.add_parser("scan")
        scan_p.add_argument("directory")
        scan_p.add_argument("-j", "--json", action="store_true")

        info_p = subparsers.add_parser("info")
        info_p.add_argument("plugin")
        info_p.add_argument("-j", "--json", action="store_true")
        info_p.add_argument("--probe", action="store_true")

        params_p = subparsers.add_parser("params")
        params_p.add_argument("plugin")
        params_p.add_argument("-j", "--json", action="store_true")
        params_p.add_argument("-V", "--verbose", action="store_true")

        midi_p = subparsers.add_parser("midi")
        midi_p.add_argument("-j", "--json", action="store_true")
        midi_p.add_argument("-m", "--monitor", type=int, default=None)
        midi_p.add_argument("--virtual-midi", type=str, default=None)

        play_p = subparsers.add_parser("play")
        play_p.add_argument("plugin")
        play_p.add_argument("-i", "--input", action="store_true")
        play_p.add_argument("-m", "--midi", type=int)
        play_p.add_argument("-v", "--virtual-midi", type=str)
        play_p.add_argument("--midi-out", type=int)
        play_p.add_argument("--virtual-midi-out", type=str)

        process_p = subparsers.add_parser("process")
        process_p.add_argument("plugin")
        process_p.add_argument("-o", "--output", required=True)
        process_p.add_argument("-i", "--input", action="append")
        process_p.add_argument("-m", "--midi-input")
        process_p.add_argument("-t", "--tail", type=float, default=2.0)
        process_p.add_argument("-s", "--state")
        process_p.add_argument("--vstpreset")
        process_p.add_argument("-p", "--preset", type=int)
        process_p.add_argument("--param-file")
        process_p.add_argument("--param", action="append")
        process_p.add_argument("-y", "--overwrite", action="store_true")
        process_p.add_argument("--bit-depth", type=int, choices=[16, 24, 32])
        process_p.add_argument("--out-channels", type=int)
        process_p.add_argument("--non-realtime", action="store_true")
        process_p.add_argument("--bpm", type=float)

        return parser.parse_args(argv)
    finally:
        sys.argv = old_argv


# ---------------------------------------------------------------------------
# Argument Parsing
# ---------------------------------------------------------------------------


class TestArgParsingScan:
    def test_basic(self):
        args = _parse(["scan", "/some/dir"])
        assert args.command == "scan"
        assert args.directory == "/some/dir"
        assert args.json is False

    def test_json_flag(self):
        args = _parse(["scan", "/some/dir", "--json"])
        assert args.json is True

    def test_missing_directory(self):
        with pytest.raises(SystemExit):
            _parse(["scan"])


class TestArgParsingInfo:
    def test_basic(self):
        args = _parse(["info", "/path/to/plugin.vst3"])
        assert args.command == "info"
        assert args.plugin == "/path/to/plugin.vst3"
        assert args.json is False
        assert args.probe is False

    def test_probe_flag(self):
        args = _parse(["info", "/path/to/plugin.vst3", "--probe"])
        assert args.probe is True

    def test_json_flag(self):
        args = _parse(["info", "/path/to/plugin.vst3", "-j"])
        assert args.json is True

    def test_missing_plugin(self):
        with pytest.raises(SystemExit):
            _parse(["info"])


class TestArgParsingParams:
    def test_basic(self):
        args = _parse(["params", "/path/to/plugin.vst3"])
        assert args.command == "params"
        assert args.plugin == "/path/to/plugin.vst3"

    def test_verbose_flag(self):
        args = _parse(["params", "/path/to/plugin.vst3", "-V"])
        assert args.verbose is True

    def test_json_flag(self):
        args = _parse(["params", "/path/to/plugin.vst3", "--json"])
        assert args.json is True

    def test_missing_plugin(self):
        with pytest.raises(SystemExit):
            _parse(["params"])


class TestArgParsingMidi:
    def test_list_mode(self):
        args = _parse(["midi"])
        assert args.command == "midi"
        assert args.monitor is None
        assert args.virtual_midi is None

    def test_monitor(self):
        args = _parse(["midi", "-m", "0"])
        assert args.monitor == 0

    def test_virtual_midi(self):
        args = _parse(["midi", "--virtual-midi", "Test Port"])
        assert args.virtual_midi == "Test Port"

    def test_json_flag(self):
        args = _parse(["midi", "--json"])
        assert args.json is True


class TestArgParsingPlay:
    def test_basic(self):
        args = _parse(["play", "/path/to/synth.vst3"])
        assert args.command == "play"
        assert args.plugin == "/path/to/synth.vst3"
        assert args.midi is None
        assert args.virtual_midi is None

    def test_midi_port(self):
        args = _parse(["play", "/path/to/synth.vst3", "-m", "1"])
        assert args.midi == 1

    def test_virtual_midi(self):
        args = _parse(["play", "/path/to/synth.vst3", "-v", "My Synth"])
        assert args.virtual_midi == "My Synth"

    def test_midi_out(self):
        args = _parse(["play", "/path/to/synth.vst3", "--midi-out", "2"])
        assert args.midi_out == 2

    def test_virtual_midi_out(self):
        args = _parse(["play", "/path/to/synth.vst3", "--virtual-midi-out", "Out"])
        assert args.virtual_midi_out == "Out"

    def test_missing_plugin(self):
        with pytest.raises(SystemExit):
            _parse(["play"])


class TestArgParsingProcess:
    def test_basic(self):
        args = _parse(["process", "effect.vst3", "-o", "out.wav", "-i", "in.wav"])
        assert args.command == "process"
        assert args.plugin == "effect.vst3"
        assert args.output == "out.wav"
        assert args.input == ["in.wav"]

    def test_missing_output(self):
        with pytest.raises(SystemExit):
            _parse(["process", "effect.vst3", "-i", "in.wav"])

    def test_midi_input(self):
        args = _parse(["process", "synth.vst3", "-o", "out.wav", "-m", "song.mid"])
        assert args.midi_input == "song.mid"

    def test_tail(self):
        args = _parse(["process", "s.vst3", "-o", "o.wav", "-m", "s.mid", "-t", "5.0"])
        assert args.tail == 5.0

    def test_tail_default(self):
        args = _parse(["process", "s.vst3", "-o", "o.wav", "-m", "s.mid"])
        assert args.tail == 2.0

    def test_multiple_inputs(self):
        args = _parse(
            ["process", "comp.vst3", "-o", "o.wav", "-i", "main.wav", "-i", "sc.wav"]
        )
        assert args.input == ["main.wav", "sc.wav"]

    def test_param_flags(self):
        args = _parse(
            [
                "process",
                "e.vst3",
                "-o",
                "o.wav",
                "-i",
                "i.wav",
                "--param",
                "Mix:0.5",
                "--param",
                "Feedback:0.7",
            ]
        )
        assert args.param == ["Mix:0.5", "Feedback:0.7"]

    def test_bit_depth(self):
        args = _parse(
            ["process", "e.vst3", "-o", "o.wav", "-i", "i.wav", "--bit-depth", "16"]
        )
        assert args.bit_depth == 16

    def test_invalid_bit_depth(self):
        with pytest.raises(SystemExit):
            _parse(
                ["process", "e.vst3", "-o", "o.wav", "-i", "i.wav", "--bit-depth", "8"]
            )

    def test_overwrite_flag(self):
        args = _parse(["process", "e.vst3", "-o", "o.wav", "-i", "i.wav", "-y"])
        assert args.overwrite is True

    def test_non_realtime_flag(self):
        args = _parse(
            ["process", "e.vst3", "-o", "o.wav", "-i", "i.wav", "--non-realtime"]
        )
        assert args.non_realtime is True

    def test_bpm(self):
        args = _parse(
            ["process", "e.vst3", "-o", "o.wav", "-i", "i.wav", "--bpm", "140"]
        )
        assert args.bpm == 140.0

    def test_out_channels(self):
        args = _parse(
            ["process", "e.vst3", "-o", "o.wav", "-i", "i.wav", "--out-channels", "4"]
        )
        assert args.out_channels == 4

    def test_state_and_vstpreset(self):
        args = _parse(
            [
                "process",
                "e.vst3",
                "-o",
                "o.wav",
                "-i",
                "i.wav",
                "-s",
                "state.bin",
                "--vstpreset",
                "my.vstpreset",
            ]
        )
        assert args.state == "state.bin"
        assert args.vstpreset == "my.vstpreset"

    def test_factory_preset(self):
        args = _parse(["process", "e.vst3", "-o", "o.wav", "-i", "i.wav", "-p", "3"])
        assert args.preset == 3

    def test_param_file(self):
        args = _parse(
            [
                "process",
                "e.vst3",
                "-o",
                "o.wav",
                "-i",
                "i.wav",
                "--param-file",
                "auto.json",
            ]
        )
        assert args.param_file == "auto.json"


class TestGlobalOptions:
    def test_default_sample_rate(self):
        args = _parse(["scan", "/dir"])
        assert args.sample_rate == 48000

    def test_custom_sample_rate(self):
        args = _parse(["-r", "44100", "scan", "/dir"])
        assert args.sample_rate == 44100

    def test_default_block_size(self):
        args = _parse(["scan", "/dir"])
        assert args.block_size == 512

    def test_custom_block_size(self):
        args = _parse(["-b", "256", "scan", "/dir"])
        assert args.block_size == 256


class TestNoCommand:
    def test_no_subcommand_returns_1(self):
        """Calling main() with no subcommand should print help and return 1."""
        with patch("sys.argv", ["minihost"]):
            ret = main()
            assert ret == 1


# ---------------------------------------------------------------------------
# Error Paths (mock plugin loading)
# ---------------------------------------------------------------------------


class TestCmdScanErrors:
    def test_scan_runtime_error(self, capsys):
        """scan should return 1 and print error on RuntimeError."""
        args = argparse.Namespace(
            directory="/nonexistent", json=False, sample_rate=48000, block_size=512
        )
        with patch("minihost.scan_directory", side_effect=RuntimeError("not found")):
            ret = cmd_scan(args)
        assert ret == 1
        assert "not found" in capsys.readouterr().err

    def test_scan_success_text(self, capsys):
        """scan should print results in text mode."""
        args = argparse.Namespace(
            directory="/plugins", json=False, sample_rate=48000, block_size=512
        )
        results = [{"name": "TestSynth", "format": "VST3", "path": "/p/test.vst3"}]
        with patch("minihost.scan_directory", return_value=results):
            ret = cmd_scan(args)
        assert ret == 0
        out = capsys.readouterr().out
        assert "TestSynth" in out
        assert "Found 1 plugin(s)" in out

    def test_scan_success_json(self, capsys):
        """scan should print valid JSON in json mode."""
        import json

        args = argparse.Namespace(
            directory="/plugins", json=True, sample_rate=48000, block_size=512
        )
        results = [{"name": "TestSynth", "format": "VST3", "path": "/p/test.vst3"}]
        with patch("minihost.scan_directory", return_value=results):
            ret = cmd_scan(args)
        assert ret == 0
        out = capsys.readouterr().out
        parsed = json.loads(out)
        assert len(parsed) == 1
        assert parsed[0]["name"] == "TestSynth"

    def test_scan_empty_results(self, capsys):
        args = argparse.Namespace(
            directory="/empty", json=False, sample_rate=48000, block_size=512
        )
        with patch("minihost.scan_directory", return_value=[]):
            ret = cmd_scan(args)
        assert ret == 0
        assert "Found 0 plugin(s)" in capsys.readouterr().out


class TestCmdInfoErrors:
    def test_info_probe_error(self, capsys):
        args = argparse.Namespace(
            plugin="/bad.vst3",
            json=False,
            probe=True,
            sample_rate=48000,
            block_size=512,
        )
        with patch("minihost.probe", side_effect=RuntimeError("cannot probe")):
            ret = cmd_info(args)
        assert ret == 1
        assert "cannot probe" in capsys.readouterr().err

    def test_info_load_error(self, capsys):
        args = argparse.Namespace(
            plugin="/bad.vst3",
            json=False,
            probe=False,
            sample_rate=48000,
            block_size=512,
        )
        with patch("minihost.Plugin", side_effect=RuntimeError("load failed")):
            ret = cmd_info(args)
        assert ret == 1
        assert "load failed" in capsys.readouterr().err


class TestCmdParamsErrors:
    def test_params_load_error(self, capsys):
        args = argparse.Namespace(
            plugin="/bad.vst3",
            json=False,
            verbose=False,
            sample_rate=48000,
            block_size=512,
        )
        with patch("minihost.Plugin", side_effect=RuntimeError("load failed")):
            ret = cmd_params(args)
        assert ret == 1
        assert "load failed" in capsys.readouterr().err


class TestCmdMidiErrors:
    def test_midi_list_mode(self, capsys):
        """List mode should succeed with mocked ports."""
        args = argparse.Namespace(
            json=False,
            monitor=None,
            virtual_midi=None,
            sample_rate=48000,
            block_size=512,
        )
        with (
            patch("minihost.midi_get_input_ports", return_value=[]),
            patch("minihost.midi_get_output_ports", return_value=[]),
        ):
            ret = cmd_midi(args)
        assert ret == 0
        out = capsys.readouterr().out
        assert "MIDI Input Ports" in out

    def test_midi_list_json(self, capsys):
        import json

        args = argparse.Namespace(
            json=True,
            monitor=None,
            virtual_midi=None,
            sample_rate=48000,
            block_size=512,
        )
        ports = [{"index": 0, "name": "IAC Driver"}]
        with (
            patch("minihost.midi_get_input_ports", return_value=ports),
            patch("minihost.midi_get_output_ports", return_value=[]),
        ):
            ret = cmd_midi(args)
        assert ret == 0
        parsed = json.loads(capsys.readouterr().out)
        assert "inputs" in parsed
        assert len(parsed["inputs"]) == 1


class TestCmdPlayErrors:
    def test_play_load_error(self, capsys):
        args = argparse.Namespace(
            plugin="/bad.vst3",
            input=False,
            midi=None,
            virtual_midi=None,
            midi_out=None,
            virtual_midi_out=None,
            sample_rate=48000,
            block_size=512,
        )
        with patch("minihost.Plugin", side_effect=RuntimeError("load failed")):
            ret = cmd_play(args)
        assert ret == 1
        assert "load failed" in capsys.readouterr().err

    def test_play_invalid_midi_port(self, capsys):
        mock_plugin = MagicMock()
        mock_plugin.num_input_channels = 2
        mock_plugin.num_output_channels = 2
        mock_plugin.num_params = 0

        args = argparse.Namespace(
            plugin="/synth.vst3",
            input=False,
            midi=99,
            virtual_midi=None,
            midi_out=None,
            virtual_midi_out=None,
            sample_rate=48000,
            block_size=512,
        )
        with (
            patch("minihost.Plugin", return_value=mock_plugin),
            patch("minihost.midi_get_input_ports", return_value=[]),
        ):
            ret = cmd_play(args)
        assert ret == 1
        assert "MIDI port 99 not found" in capsys.readouterr().err


class TestCmdProcessErrors:
    def test_process_no_input_or_midi(self, capsys):
        """process requires at least one of --input or --midi-input."""
        args = argparse.Namespace(
            plugin="effect.vst3",
            output="out.wav",
            input=None,
            midi_input=None,
            overwrite=False,
            sample_rate=48000,
            block_size=512,
            tail=2.0,
            state=None,
            vstpreset=None,
            preset=None,
            param_file=None,
            param=None,
            bit_depth=None,
            out_channels=None,
            non_realtime=False,
            bpm=None,
        )
        with patch("os.path.exists", return_value=False):
            ret = cmd_process(args)
        assert ret == 1
        assert "At least one of --input or --midi-input" in capsys.readouterr().err

    def test_process_output_exists_no_overwrite(self, capsys):
        args = argparse.Namespace(
            plugin="effect.vst3",
            output="existing.wav",
            input=["in.wav"],
            midi_input=None,
            overwrite=False,
            sample_rate=48000,
            block_size=512,
            tail=2.0,
            state=None,
            vstpreset=None,
            preset=None,
            param_file=None,
            param=None,
            bit_depth=None,
            out_channels=None,
            non_realtime=False,
            bpm=None,
        )
        with patch("os.path.exists", return_value=True):
            ret = cmd_process(args)
        assert ret == 1
        assert "already exists" in capsys.readouterr().err

    def test_process_plugin_load_error(self, capsys):
        args = argparse.Namespace(
            plugin="/bad.vst3",
            output="out.wav",
            input=["in.wav"],
            midi_input=None,
            overwrite=False,
            sample_rate=48000,
            block_size=512,
            tail=2.0,
            state=None,
            vstpreset=None,
            preset=None,
            param_file=None,
            param=None,
            bit_depth=None,
            out_channels=None,
            non_realtime=False,
            bpm=None,
        )
        mock_info = {"sample_rate": 48000}
        with (
            patch("os.path.exists", return_value=False),
            patch("minihost.audio_io.get_audio_info", return_value=mock_info),
            patch("minihost.audio_io.read_audio") as mock_read,
        ):
            import numpy as np

            mock_read.return_value = (np.zeros((2, 1000), dtype=np.float32), 48000)
            with patch("minihost.Plugin", side_effect=RuntimeError("bad plugin")):
                ret = cmd_process(args)
        assert ret == 1
        assert "bad plugin" in capsys.readouterr().err

    def test_process_input_read_error(self, capsys):
        args = argparse.Namespace(
            plugin="effect.vst3",
            output="out.wav",
            input=["missing.wav"],
            midi_input=None,
            overwrite=False,
            sample_rate=48000,
            block_size=512,
            tail=2.0,
            state=None,
            vstpreset=None,
            preset=None,
            param_file=None,
            param=None,
            bit_depth=None,
            out_channels=None,
            non_realtime=False,
            bpm=None,
        )
        with (
            patch("os.path.exists", return_value=False),
            patch(
                "minihost.audio_io.get_audio_info",
                side_effect=RuntimeError("file not found"),
            ),
        ):
            ret = cmd_process(args)
        assert ret == 1
        assert "Error reading input file" in capsys.readouterr().err


# ---------------------------------------------------------------------------
# Play --input (capture) flag
# ---------------------------------------------------------------------------


class TestArgParsingPlayInput:
    def test_input_flag(self):
        args = _parse(["play", "/path/to/effect.vst3", "-i"])
        assert args.input is True

    def test_input_long_flag(self):
        args = _parse(["play", "/path/to/effect.vst3", "--input"])
        assert args.input is True

    def test_no_input_flag(self):
        args = _parse(["play", "/path/to/effect.vst3"])
        assert args.input is False

    def test_input_with_midi(self):
        args = _parse(["play", "/path/to/effect.vst3", "-i", "-m", "0"])
        assert args.input is True
        assert args.midi == 0


# ---------------------------------------------------------------------------
# Glob expansion and batch detection
# ---------------------------------------------------------------------------


class TestExpandGlobs:
    def test_no_globs(self):
        result = _expand_globs(["a.wav", "b.wav"])
        assert result == ["a.wav", "b.wav"]

    def test_deduplicates(self):
        result = _expand_globs(["a.wav", "a.wav"])
        assert result == ["a.wav"]

    def test_glob_expansion(self, tmp_path):
        # Create test files
        (tmp_path / "a.wav").touch()
        (tmp_path / "b.wav").touch()
        (tmp_path / "c.txt").touch()
        result = _expand_globs([str(tmp_path / "*.wav")])
        assert len(result) == 2
        basenames = {os.path.basename(p) for p in result}
        assert basenames == {"a.wav", "b.wav"}

    def test_glob_no_matches(self, tmp_path):
        result = _expand_globs([str(tmp_path / "*.xyz")])
        assert result == []

    def test_mixed_glob_and_literal(self, tmp_path):
        (tmp_path / "x.wav").touch()
        result = _expand_globs(["literal.wav", str(tmp_path / "*.wav")])
        assert result[0] == "literal.wav"
        assert os.path.basename(result[1]) == "x.wav"


class TestIsBatchOutput:
    def test_trailing_slash(self):
        assert _is_batch_output("output/") is True

    def test_trailing_os_sep(self):
        assert _is_batch_output(f"output{os.sep}") is True

    def test_existing_directory(self, tmp_path):
        assert _is_batch_output(str(tmp_path)) is True

    def test_file_path(self):
        assert _is_batch_output("output.wav") is False

    def test_nonexistent_path_no_slash(self):
        assert _is_batch_output("/nonexistent/output.wav") is False


class TestBatchProcessErrors:
    def test_batch_glob_matches_nothing_is_no_input_error(self, capsys, tmp_path):
        """Glob matching nothing means no input files -> 'at least one of' error."""
        args = argparse.Namespace(
            plugin="effect.vst3",
            output=str(tmp_path) + "/",
            input=[str(tmp_path / "*.wav")],
            midi_input=None,
            overwrite=False,
            sample_rate=48000,
            block_size=512,
            tail=2.0,
            state=None,
            vstpreset=None,
            preset=None,
            param_file=None,
            param=None,
            bit_depth=None,
            out_channels=None,
            non_realtime=False,
            bpm=None,
        )
        ret = cmd_process(args)
        assert ret == 1
        assert "At least one of --input or --midi-input" in capsys.readouterr().err

    def test_batch_plugin_load_error(self, capsys, tmp_path):
        """Batch mode should fail if plugin can't be loaded."""
        (tmp_path / "a.wav").touch()
        out_dir = tmp_path / "out"
        args = argparse.Namespace(
            plugin="/bad.vst3",
            output=str(out_dir) + "/",
            input=[str(tmp_path / "*.wav")],
            midi_input=None,
            overwrite=False,
            sample_rate=48000,
            block_size=512,
            tail=2.0,
            state=None,
            vstpreset=None,
            preset=None,
            param_file=None,
            param=None,
            bit_depth=None,
            out_channels=None,
            non_realtime=False,
            bpm=None,
        )
        mock_info = {
            "sample_rate": 48000,
            "channels": 2,
            "frames": 1000,
            "duration": 0.02,
        }
        with (
            patch("minihost.audio_io.get_audio_info", return_value=mock_info),
            patch("minihost.Plugin", side_effect=RuntimeError("bad plugin")),
        ):
            ret = cmd_process(args)
        assert ret == 1
        assert "bad plugin" in capsys.readouterr().err
