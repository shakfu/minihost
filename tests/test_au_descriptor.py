"""AudioUnit (open-by-descriptor) support.

AudioUnits have no file path -- they are identified by a juce::PluginDescription
(an AU id like "AudioUnit:Effects/aufx,bpas,appl"). minihost was historically a
path-only host, so AUs could not load at all. `mh_open_desc` (C ABI) /
`Plugin.from_descriptor` (Python) open a plugin from a serialized
PluginDescription instead, and project files carry an optional base64
`descriptor` field for AU nodes.

These tests use stock Apple AU effects, whose identifiers are stable across
macOS versions, so no plugin scan (slow) or third-party plugin is needed. A
minimal descriptor (name + format + identifier) is enough -- JUCE's AU format
resolves the identifier to the installed component. Skipped off macOS.
"""

from __future__ import annotations

import base64
import json
import platform

import numpy as np
import pytest

import minihost

# Stock Apple AU effects present on any macOS install. First that opens wins.
_STOCK_AU = [
    ("AUBandpass", "AudioUnit:Effects/aufx,bpas,appl"),
    ("AUDelay",    "AudioUnit:Effects/aufx,dely,appl"),
    ("AULowpass",  "AudioUnit:Effects/aufx,lpas,appl"),
]

skip_if_not_macos = pytest.mark.skipif(
    platform.system() != "Darwin", reason="AudioUnits are macOS-only"
)


def _descriptor_xml(name: str, ident: str) -> str:
    return f'<PLUGIN name="{name}" format="AudioUnit" file="{ident}"/>'


@pytest.fixture(scope="module")
def au() -> tuple[str, str]:
    """(name, descriptor_xml) of the first stock Apple AU that opens."""
    if platform.system() != "Darwin":
        pytest.skip("AudioUnits are macOS-only")
    for name, ident in _STOCK_AU:
        xml = _descriptor_xml(name, ident)
        try:
            minihost.Plugin.from_descriptor(xml)
            return name, xml
        except Exception:
            continue
    pytest.skip("no stock Apple AU could be opened on this machine")


@skip_if_not_macos
def test_from_descriptor_opens_and_processes(au):
    name, xml = au
    p = minihost.Plugin.from_descriptor(
        xml, sample_rate=48000, max_block_size=512
    )
    assert p.num_input_channels >= 1
    assert p.num_output_channels >= 1
    # An impulse should pass through an effect and stay finite.
    x = np.zeros((2, 512), dtype=np.float32)
    x[:, 0] = 0.5
    y = np.asarray(minihost.process_audio(p, x))
    assert y.shape == (2, 512)
    assert np.isfinite(y).all()


@skip_if_not_macos
def test_from_descriptor_rejects_bad_input():
    # Malformed XML.
    with pytest.raises(Exception):
        minihost.Plugin.from_descriptor("<not valid xml")
    # Well-formed XML naming a nonexistent AU.
    with pytest.raises(Exception):
        minihost.Plugin.from_descriptor(
            '<PLUGIN name="Nope" format="AudioUnit" '
            'file="AudioUnit:Effects/aufx,zzzz,zzzz"/>'
        )


@skip_if_not_macos
def test_render_project_with_au_node(au, tmp_path):
    """A project whose plugin node is an AU (descriptor) renders to audio."""
    name, xml = au
    desc_b64 = base64.b64encode(xml.encode("utf-8")).decode("ascii")

    # write_audio expects (channels, frames).
    sig = (np.random.default_rng(0).standard_normal((2, 4800))
           .astype(np.float32) * 0.2)
    in_wav = tmp_path / "in.wav"
    out_wav = tmp_path / "out.wav"
    minihost.write_audio(str(in_wav), sig, 48000)

    proj = {
        "minihost_project_version": 1,
        "sample_rate": 48000,
        "block_size": 512,
        "nodes": [
            {"id": "in", "kind": "input", "channels": 2, "source": str(in_wav)},
            # receives_midi=False: effect AUs don't accept MIDI, and the
            # legacy fan-out migration would otherwise try to wire it.
            {"id": "fx", "kind": "plugin", "descriptor": desc_b64,
             "name": name, "receives_midi": False},
            {"id": "out", "kind": "output", "channels": 2,
             "sink": str(out_wav), "bit_depth": 24},
        ],
        "edges": [{"src": "in", "dst": "fx"}, {"src": "fx", "dst": "out"}],
    }
    proj_path = tmp_path / "proj.json"
    proj_path.write_text(json.dumps(proj))

    minihost.render_project(str(proj_path))

    assert out_wav.exists(), "render produced no output"
    y, sr = minihost.read_audio(str(out_wav))
    y = np.asarray(y)
    assert sr == 48000
    assert y.shape[1] == 4800
    assert np.isfinite(y).all()


@skip_if_not_macos
def test_descriptor_persists_through_project_json(au, tmp_path):
    """The descriptor field round-trips through save_project / the schema."""
    name, xml = au
    desc_b64 = base64.b64encode(xml.encode("utf-8")).decode("ascii")
    proj_path = tmp_path / "p.json"

    minihost.save_project(
        str(proj_path),
        sample_rate=48000,
        block_size=512,
        input_nodes=[{"id": "in", "channels": 2, "source": "in.wav"}],
        output_nodes=[{"id": "out", "channels": 2, "sink": "out.wav",
                       "bit_depth": 24}],
        plugin_nodes=[{"id": "fx", "descriptor": desc_b64, "name": name}],
        edges=[{"src": "in", "dst": "fx"}, {"src": "fx", "dst": "out"}],
    )

    doc = json.loads(proj_path.read_text())
    fx = next(n for n in doc["nodes"] if n.get("id") == "fx")
    assert fx.get("descriptor") == desc_b64, "descriptor not persisted"
