"""Regression tests for sample-accurate automation via ``process_auto``.

These target a specific defect in the chunk-splitting logic of
``mh_process_auto`` / ``mh_chain_process_auto``: when two or more
parameter changes fall inside a single process block, the chunk
boundary was computed from the change at ``param_idx`` *before* the
apply-loop advanced past changes already due at the chunk start. A
change due at ``current_sample`` was therefore never used as a
boundary, ``chunk_end`` jumped straight to ``nframes``, and every later
change in that block was silently swallowed (never applied).

The observable used here is plugin-agnostic: ``setValueNotifyingHost``
persists a parameter value, so reading ``get_param`` after the call
reveals whether the *last* change in the block actually took effect.
Under the bug the parameter retains the value of the first change.

Integration tests -- require a real plugin via ``MINIHOST_TEST_PLUGIN``.
"""

import os

import numpy as np
import pytest

import minihost


@pytest.fixture
def plugin_path():
    path = os.environ.get("MINIHOST_TEST_PLUGIN")
    if not path:
        pytest.skip("MINIHOST_TEST_PLUGIN not set")
    return path


@pytest.fixture
def plugin(plugin_path):
    return minihost.Plugin(plugin_path, sample_rate=48000, max_block_size=512)


def _find_varying_param(plugin):
    """Find a parameter whose normalized read-back clearly differs between
    0.0 and 1.0, so the automation effect is observable through get_param.

    Returns (index, lo_readback, hi_readback) or None if the plugin has no
    such parameter (e.g. all params are quantized to a single value).
    """
    for idx in range(plugin.num_params):
        plugin.set_param(idx, 0.0)
        lo = plugin.get_param(idx)
        plugin.set_param(idx, 1.0)
        hi = plugin.get_param(idx)
        if abs(hi - lo) > 0.25:
            return idx, lo, hi
    return None


class TestProcessAutoMultiChange:
    """Both changes in a single block must be applied, not just the first."""

    BLOCK = 512

    def _run(self, plugin, changes):
        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)
        inp = np.zeros((in_ch, self.BLOCK), dtype=np.float32)
        out = np.zeros((out_ch, self.BLOCK), dtype=np.float32)
        plugin.process_auto(inp, out, [], changes)

    def _assert_last_change_wins(self, plugin, idx, lo, hi):
        got = plugin.get_param(idx)
        assert abs(got - hi) < abs(got - lo), (
            f"param {idx} read back {got:.4f}; expected it to reflect the "
            f"last in-block change (~{hi:.4f}), not the first (~{lo:.4f}). "
            f"The mid-block parameter change was dropped."
        )

    def test_second_change_when_first_is_at_block_start(self, plugin):
        """Change at offset 0 plus a change mid-block: the offset-0 change
        must not suppress the later one (the original failing case)."""
        found = _find_varying_param(plugin)
        if found is None:
            pytest.skip("no parameter with distinguishable 0.0/1.0 read-back")
        idx, lo, hi = found
        plugin.reset()
        plugin.set_param(idx, 0.0)
        self._run(plugin, [(0, idx, 0.0), (self.BLOCK // 2, idx, 1.0)])
        self._assert_last_change_wins(plugin, idx, lo, hi)

    def test_two_changes_neither_at_block_start(self, plugin):
        """Two changes at interior offsets: the second must still land."""
        found = _find_varying_param(plugin)
        if found is None:
            pytest.skip("no parameter with distinguishable 0.0/1.0 read-back")
        idx, lo, hi = found
        plugin.reset()
        plugin.set_param(idx, 0.0)
        self._run(
            plugin,
            [(self.BLOCK // 4, idx, 0.0), (self.BLOCK // 2, idx, 1.0)],
        )
        self._assert_last_change_wins(plugin, idx, lo, hi)

    def test_three_changes_in_one_block_last_wins(self, plugin):
        """Denser case: three changes in a block, final value must be the
        last one, exercising repeated boundary recomputation."""
        found = _find_varying_param(plugin)
        if found is None:
            pytest.skip("no parameter with distinguishable 0.0/1.0 read-back")
        idx, lo, hi = found
        plugin.reset()
        plugin.set_param(idx, 0.0)
        self._run(
            plugin,
            [(0, idx, 0.0), (128, idx, 0.0), (300, idx, 1.0)],
        )
        self._assert_last_change_wins(plugin, idx, lo, hi)
