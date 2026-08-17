"""Conformance tests: the C and C++ front-ends must agree byte-for-byte.

`minihost_c` (pure C) and `minihost_cpp` (C++) are two independent CLI
implementations over the same libminihost C API. They are meant to be
interchangeable, but nothing stopped them drifting apart -- historically they
lagged the library by several releases. This test runs the same data commands
through both binaries and asserts their stdout is identical, so any divergence
fails CI instead of accumulating silently.

Scope: deterministic, plugin-data commands (metadata, parameters, presets,
morph). Human-facing help/usage text is intentionally *not* compared -- the two
CLIs use different argument parsers (hand-rolled vs CLI11) and their usage
strings differ by design. stderr is ignored (plugins log there on load).

The test is skipped unless both binaries and a test plugin are available. Point
it at the binaries with ``MINIHOST_C_BIN`` / ``MINIHOST_CPP_BIN``, or build them
into ``build/`` (as CI's build-cli job does) or ``build-cli/``. The plugin comes
from ``MINIHOST_TEST_PLUGIN`` (default: Dexed).
"""

from __future__ import annotations

import functools
import os
import subprocess
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parent.parent

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)


def _find_binary(name: str, env_var: str) -> str | None:
    """Locate a CLI binary via env var or common build locations.

    Among the build-directory candidates the most recently built one wins,
    so a fresh standalone build (e.g. ``build-cli/``) is preferred over a
    stale artifact left in ``build/`` from an earlier configuration.
    """
    env = os.environ.get(env_var)
    if env and os.path.exists(env):
        return env
    candidates = [
        _REPO_ROOT / "build" / "projects" / name / name,
        _REPO_ROOT / "build" / "projects" / name / "Release" / f"{name}.exe",
        _REPO_ROOT / "build-cli" / "projects" / name / name,
        _REPO_ROOT / "build-cli" / "projects" / name / "Release" / f"{name}.exe",
        _REPO_ROOT / "build-desktop" / "projects" / name / name,
    ]
    existing = [c for c in candidates if c.exists()]
    if not existing:
        return None
    return str(max(existing, key=lambda p: p.stat().st_mtime))


C_BIN = _find_binary("minihost_c", "MINIHOST_C_BIN")
CPP_BIN = _find_binary("minihost_cpp", "MINIHOST_CPP_BIN")

# The binaries are required by every test here. The plugin is not: the
# resample and error-path tests below run without one, which is what lets
# this file do useful work in CI, where no runner has a plugin installed.
pytestmark = pytest.mark.skipif(
    C_BIN is None or CPP_BIN is None,
    reason="minihost_c and/or minihost_cpp binary not found (build them first)",
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN), reason=f"test plugin not found at {PLUGIN}"
)

PIANO = _REPO_ROOT / "tests" / "_wav" / "piano.wav"
skip_if_no_audio = pytest.mark.skipif(
    not PIANO.exists(), reason=f"test audio not found at {PIANO}"
)


# Deterministic data commands. Each entry is the argument list following the
# binary; {PLUGIN} is substituted at runtime.
CONFORMANCE_COMMANDS = [
    ["probe", "{PLUGIN}", "-j"],
    ["info", "{PLUGIN}", "--probe", "-j"],
    ["info", "{PLUGIN}", "-j"],
    ["params", "{PLUGIN}", "-j"],
    ["params", "{PLUGIN}", "-V", "-j"],
    ["presets", "{PLUGIN}", "-j"],
    ["morph", "{PLUGIN}", "-t", "0.3", "-j"],
    ["morph", "{PLUGIN}", "-t", "0.0"],
    ["morph", "{PLUGIN}", "-t", "0.75"],
]


def _run(binary: str, args: list[str]) -> subprocess.CompletedProcess:
    resolved = [a.replace("{PLUGIN}", PLUGIN) for a in args]
    return subprocess.run(
        [binary, *resolved],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        timeout=120,
    )


def _cmd_id(args: list[str]) -> str:
    return " ".join(a for a in args if a != "{PLUGIN}")


@functools.lru_cache(maxsize=1)
def _morph_is_exercisable() -> bool:
    """Whether `morph` can actually blend against the configured test plugin.

    With no --a-state/--b-state or --a-program/--b-program, `morph` falls back
    to the plugin's first two factory programs and exits non-zero when there
    are fewer than two. A parameter-only synth exposing no program list is
    therefore a legitimate "cannot run", not a CLI defect -- so the morph
    cases assert agreement between the two binaries but do not demand success.

    Unknown (minihost not importable, plugin fails to open) is reported as
    exercisable so a genuine morph regression is never silently skipped.
    """
    try:
        import minihost
    except ImportError:
        return True
    try:
        plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    except Exception:
        return True
    try:
        return plugin.num_programs >= 2
    finally:
        plugin.close()


def _assert_stdout_identical(c, cpp, args) -> None:
    if c.stdout == cpp.stdout:
        return
    # Produce a readable diff on failure rather than dumping raw bytes.
    import difflib

    c_lines = c.stdout.decode("utf-8", "replace").splitlines()
    cpp_lines = cpp.stdout.decode("utf-8", "replace").splitlines()
    diff = "\n".join(
        difflib.unified_diff(
            c_lines, cpp_lines, "minihost_c", "minihost_cpp", lineterm="", n=2
        )
    )
    # Cap the diff so a large mismatch stays readable.
    diff_head = "\n".join(diff.splitlines()[:40])
    pytest.fail(
        f"stdout differs for '{_cmd_id(args)}' "
        f"(C={len(c.stdout)}b, CPP={len(cpp.stdout)}b):\n{diff_head}"
    )


@skip_if_no_plugin
@pytest.mark.parametrize("args", CONFORMANCE_COMMANDS, ids=_cmd_id)
def test_c_and_cpp_stdout_identical(args):
    c = _run(C_BIN, args)
    cpp = _run(CPP_BIN, args)

    # Interchangeability is the property under test, and it applies whether or
    # not the command can run against this plugin: the two front-ends must
    # succeed together or decline together, with the same output.
    assert c.returncode == cpp.returncode, (
        f"exit codes diverge for '{_cmd_id(args)}': "
        f"minihost_c={c.returncode}, minihost_cpp={cpp.returncode}"
    )
    _assert_stdout_identical(c, cpp, args)

    if args[0] == "morph" and not _morph_is_exercisable():
        pytest.skip(
            f"{PLUGIN} exposes < 2 factory programs; both CLIs decline morph "
            f"identically (exit {c.returncode}), but the blend itself is "
            f"unexercised. Point MINIHOST_TEST_PLUGIN at a plugin with "
            f"factory presets to cover it."
        )

    assert c.returncode == 0, f"minihost_c failed ({c.returncode}) for {_cmd_id(args)}"
    assert cpp.returncode == 0, (
        f"minihost_cpp failed ({cpp.returncode}) for {_cmd_id(args)}"
    )


@skip_if_no_plugin
def test_morph_blend_endpoints_match_across_clis():
    """A spot check that both CLIs interpolate identically at several t."""
    # Checked in-body rather than via skipif so collection never loads a
    # plugin (JUCE initialisation during collection is best avoided).
    if not _morph_is_exercisable():
        pytest.skip(f"{PLUGIN} exposes < 2 factory programs; morph cannot blend")
    for t in ("0.1", "0.5", "0.9"):
        args = ["morph", "{PLUGIN}", "-t", t, "-j"]
        c = _run(C_BIN, args)
        cpp = _run(CPP_BIN, args)
        assert c.returncode == 0 and cpp.returncode == 0
        assert c.stdout == cpp.stdout, f"morph -t {t} diverges between CLIs"


# ---------------------------------------------------------------------------
# Rendering commands
#
# The stdout comparison above covers the data commands. The commands that
# actually produce audio -- process, chain, bus -- were checked only by
# hand until now, which is the half of the CLI that matters most.
#
# Comparing renders has to allow for plugins that are not reproducible
# against themselves: several commercial effects carry free-running
# modulation that reset() does not reseed, so two runs of the *same*
# binary differ. Each test therefore measures that floor first, by
# rendering twice with one binary, and only then asks whether the two
# binaries differ by more than the plugin differs from itself. With a
# deterministic plugin the floor is zero and the comparison is exact.
# ---------------------------------------------------------------------------


def _render(binary: str, args: list[str], out: Path) -> None:
    resolved = [
        a.replace("{PLUGIN}", PLUGIN)
        .replace("{OUT}", str(out))
        .replace("{PIANO}", str(PIANO))
        for a in args
    ]
    proc = subprocess.run(
        [binary, *resolved],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        timeout=300,
    )
    assert proc.returncode == 0, (
        f"{Path(binary).name} {' '.join(resolved)} failed: "
        f"{proc.stderr.decode(errors='replace')[-400:]}"
    )
    assert out.exists(), f"{Path(binary).name} wrote no output"


def _residual_db(path_a: Path, path_b: Path) -> float:
    """RMS difference between two renders, in dBFS. -inf when identical."""
    if path_a.read_bytes() == path_b.read_bytes():
        return float("-inf")
    np = pytest.importorskip("numpy")
    minihost = pytest.importorskip("minihost")
    a, _ = minihost.read_audio(str(path_a), as_=np.ndarray)
    b, _ = minihost.read_audio(str(path_b), as_=np.ndarray)
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    assert a.shape[0] == b.shape[0], "renders differ in channel count"
    n = min(a.shape[-1], b.shape[-1])
    assert abs(a.shape[-1] - b.shape[-1]) <= 1, "renders differ in length"
    residual = a[:, :n] - b[:, :n]
    return 20.0 * float(np.log10(max(float(np.sqrt(np.mean(residual**2))), 1e-12)))


def _assert_binaries_agree(args: list[str], tmp_path: Path, label: str) -> None:
    """Both binaries must agree to within the plugin's own repeatability."""
    c_first = tmp_path / f"{label}_c1.wav"
    c_second = tmp_path / f"{label}_c2.wav"
    cpp_out = tmp_path / f"{label}_cpp.wav"

    # A throwaway render first: the first render in a process leaves
    # plugin state behind that changes later ones, so measuring the floor
    # against a first render would overstate it.
    _render(C_BIN, args, tmp_path / f"{label}_warm.wav")
    _render(C_BIN, args, c_first)
    _render(C_BIN, args, c_second)
    _render(CPP_BIN, args, cpp_out)

    floor = _residual_db(c_first, c_second)  # same binary, twice
    across = _residual_db(c_first, cpp_out)  # C vs C++

    if floor == float("-inf"):
        assert across == float("-inf"), (
            f"{label}: the plugin renders identically twice through minihost_c, "
            f"but minihost_cpp differs ({across:.2f} dBFS residual)"
        )
    else:
        assert across <= floor + 3.0, (
            f"{label}: the two binaries differ by {across:.2f} dBFS, more than "
            f"the plugin differs from itself ({floor:.2f} dBFS)"
        )


@skip_if_no_plugin
@skip_if_no_audio
def test_process_renders_identically_across_clis(tmp_path):
    _assert_binaries_agree(
        ["process", "{PLUGIN}", "-i", "{PIANO}", "-o", "{OUT}", "--tail", "0"],
        tmp_path,
        "process",
    )


@skip_if_no_plugin
def test_process_midi_renders_identically_across_clis(tmp_path):
    """The C binary's -m used to be unimplemented; this pins the parity."""
    midi = _REPO_ROOT / "tests" / "_midi" / "bach.mid"
    if not midi.exists():
        pytest.skip(f"test MIDI not found at {midi}")
    _assert_binaries_agree(
        ["process", "{PLUGIN}", "-m", str(midi), "-o", "{OUT}", "--tail", "1"],
        tmp_path,
        "process_midi",
    )


@skip_if_no_plugin
@skip_if_no_audio
def test_chain_renders_identically_across_clis(tmp_path):
    _assert_binaries_agree(
        [
            "chain",
            "{PLUGIN}",
            "{PLUGIN}",
            "-i",
            "{PIANO}",
            "-o",
            "{OUT}",
            "--tail",
            "0",
        ],
        tmp_path,
        "chain",
    )


@skip_if_no_plugin
@skip_if_no_audio
def test_bus_renders_identically_across_clis(tmp_path):
    _assert_binaries_agree(
        ["bus", "{PLUGIN}", "{PLUGIN}", "-i", "{PIANO}", "-o", "{OUT}", "--tail", "0"],
        tmp_path,
        "bus",
    )


@skip_if_no_plugin
def test_bus_sums_its_branches(tmp_path):
    """A two-branch bus of one plugin must be that plugin, doubled.

    Not a conformance check but a correctness one, and cheap here: it
    catches a bus that drops or double-counts a branch, which comparing
    the two binaries against each other cannot.
    """
    np = pytest.importorskip("numpy")
    minihost = pytest.importorskip("minihost")

    midi = _REPO_ROOT / "tests" / "_midi" / "bach.mid"
    if not midi.exists():
        pytest.skip(f"test MIDI not found at {midi}")

    one = tmp_path / "one.wav"
    two = tmp_path / "two.wav"
    single = ["bus", "{PLUGIN}", "-m", str(midi), "-o", "{OUT}", "--tail", "0"]
    doubled = [
        "bus",
        "{PLUGIN}",
        "{PLUGIN}",
        "-m",
        str(midi),
        "-o",
        "{OUT}",
        "--tail",
        "0",
    ]
    _render(C_BIN, single, tmp_path / "warm.wav")  # discard the first render
    _render(C_BIN, single, one)
    _render(C_BIN, doubled, two)

    a, _ = minihost.read_audio(str(one), as_=np.ndarray)
    b, _ = minihost.read_audio(str(two), as_=np.ndarray)
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    n = min(a.shape[-1], b.shape[-1])
    assert float(np.max(np.abs(a[:, :n]))) > 1e-6, (
        "the instrument produced silence; the sum check would prove nothing"
    )
    # Doubling is exact for a deterministic plugin; allow the 24-bit
    # quantization of the written files either way.
    assert np.allclose(b[:, :n], 2.0 * a[:, :n], atol=1e-4)


# ---------------------------------------------------------------------------
# Commands that need no plugin
#
# These are what make this file worth running in CI, where no runner has a
# plugin installed and every test above skips.
# ---------------------------------------------------------------------------


@skip_if_no_audio
def test_resample_matches_across_clis(tmp_path):
    """Same input, same rate, same bytes out of both binaries."""
    c_out = tmp_path / "c.wav"
    cpp_out = tmp_path / "cpp.wav"
    args = ["resample", str(PIANO), "{OUT}", "--rate", "44100"]
    _render(C_BIN, args, c_out)
    _render(CPP_BIN, args, cpp_out)
    assert c_out.read_bytes() == cpp_out.read_bytes()


@pytest.mark.parametrize(
    "args",
    [
        ["probe", "/nonexistent/plugin.vst3"],
        ["info", "/nonexistent/plugin.vst3"],
        ["params", "/nonexistent/plugin.vst3"],
        ["process", "/nonexistent/plugin.vst3", "-o", "/tmp/never-written.wav"],
        ["resample", "/nonexistent/input.wav", "/tmp/never-written.wav"],
    ],
    ids=lambda a: a[0] + "-missing-file",
)
def test_both_clis_fail_on_missing_files(args):
    """A missing path is an error in both binaries, not a silent success."""
    for binary in (C_BIN, CPP_BIN):
        proc = subprocess.run(
            [binary, *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=120,
        )
        assert proc.returncode != 0, (
            f"{Path(binary).name} {' '.join(args)} exited 0 for a missing file"
        )
        assert proc.stderr, f"{Path(binary).name} reported nothing on stderr"


def test_both_clis_reject_an_unknown_command():
    for binary in (C_BIN, CPP_BIN):
        proc = subprocess.run(
            [binary, "definitely-not-a-command"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=120,
        )
        assert proc.returncode != 0
