"""Saved chains: reusable subgraphs of processing nodes.

The format and its rationale are in ``docs/dev/chains.md``. The desktop app
reads the same files (``projects/minihost_desktop/src/chain_library.cpp``);
a change to validation or expansion here needs the same change there.

Typical use::

    from minihost import chain_library

    vocal = chain_library.read("Vocal")
    nodes.append(chain_library.node(vocal, "vox"))
    edges += [{"src": "in", "dst": "vox"}, {"src": "vox", "dst": "out"}]
"""

from __future__ import annotations

import json
import os
import platform
from pathlib import Path
from typing import Any

VERSION = 1
EXTENSION = ".mhchain"
SEP = "/"

# Project I/O stays in the project; a chain holds processing only.
_FORBIDDEN_KINDS = frozenset(
    {
        "input",
        "output",
        "device_input",
        "device_output",
        "midi_input",
        "midi_output",
        "chain",
    }
)
_FIELDS = ("nodes", "edges")
_PORT_KINDS = frozenset({"audio_inlet", "audio_outlet", "midi_inlet", "midi_outlet"})


def _ports(chain: dict, midi: bool, inlet: bool) -> list[str]:
    """Ids of a chain's port nodes of one kind, in file order."""
    kind = ("midi_" if midi else "audio_") + ("inlet" if inlet else "outlet")
    return [n["id"] for n in chain["nodes"] if n.get("kind") == kind]


def library_dir() -> Path:
    """The chain library directory, beside the desktop settings file."""
    env = os.environ.get("MINIHOST_DESKTOP_SETTINGS_DIR")
    if env:
        return Path(env) / "chains"
    system = platform.system()
    if system == "Darwin":
        base = Path.home() / "Library"
    elif system == "Windows":
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    return base / "minihost" / "chains"


def names(directory: str | Path | None = None) -> list[str]:
    """Names of the chains in the library, sorted."""
    d = Path(directory) if directory is not None else library_dir()
    return sorted(p.stem for p in d.glob("*" + EXTENSION))


def read(name_or_path: str | Path, directory: str | Path | None = None) -> dict:
    """Read and validate a chain by library name or file path.

    Relative plugin paths are made absolute against the file's directory.
    Raises FileNotFoundError, or ValueError for an invalid chain.
    """
    p = Path(name_or_path)
    if p.suffix != EXTENSION:
        d = Path(directory) if directory is not None else library_dir()
        p = d / (str(name_or_path) + EXTENSION)
    try:
        doc = json.loads(p.read_text())
    except json.JSONDecodeError as exc:
        raise ValueError(f"{p}: invalid JSON: {exc}") from exc
    if not isinstance(doc, dict):
        raise ValueError(f"{p}: chain root must be an object")
    if doc.get("minihost_chain_version") != VERSION:
        raise ValueError(
            f"{p}: unsupported chain version {doc.get('minihost_chain_version')!r}"
        )
    validate(doc, str(p))
    for n in doc["nodes"]:
        if isinstance(n.get("path"), str) and not Path(n["path"]).is_absolute():
            n["path"] = str((p.parent / n["path"]).resolve())
    return doc


def write(name: str, chain: dict, directory: str | Path | None = None) -> Path:
    """Validate `chain` and write it as `<directory>/<name>.mhchain`."""
    if not name or any(c in name for c in "/\\:") or name.startswith("."):
        raise ValueError(f"invalid chain name {name!r}")
    validate(chain, name)
    d = Path(directory) if directory is not None else library_dir()
    d.mkdir(parents=True, exist_ok=True)
    doc = {"minihost_chain_version": VERSION, "name": name}
    doc.update({k: chain[k] for k in (*_FIELDS, "layout") if k in chain})
    path = d / (name + EXTENSION)
    tmp = path.with_suffix(EXTENSION + ".tmp")
    tmp.write_text(json.dumps(doc, indent=2, allow_nan=False) + "\n")
    tmp.replace(path)
    return path


def node(chain: dict, node_id: str) -> dict:
    """A project ``chain`` node holding a copy of `chain`."""
    out = {"id": node_id, "kind": "chain"}
    if "name" in chain:
        out["source"] = chain["name"]
    out.update(json.loads(json.dumps({k: chain[k] for k in _FIELDS if k in chain})))
    return out


def validate(chain: Any, where: str) -> None:
    """Check a chain's structure. Inner node fields are checked on load."""
    if not isinstance(chain, dict):
        raise ValueError(f"{where}: chain must be an object")
    nodes = chain.get("nodes")
    if not isinstance(nodes, list) or not nodes:
        raise ValueError(f"{where}: nodes must be a non-empty list")
    ids: set[str] = set()
    for n in nodes:
        if not isinstance(n, dict):
            raise ValueError(f"{where}: node entry must be an object")
        nid, kind = n.get("id"), n.get("kind")
        if not isinstance(nid, str) or not nid or SEP in nid:
            raise ValueError(f"{where}: node id {nid!r} must be non-empty, no '/'")
        if nid in ids:
            raise ValueError(f"{where}: duplicate node id {nid!r}")
        if kind in _FORBIDDEN_KINDS:
            raise ValueError(
                f"{where}: node {nid!r}: kind {kind!r} cannot be in a chain"
            )
        ids.add(nid)
    kind_of = {n["id"]: str(n.get("kind")) for n in nodes}
    edges = chain.get("edges", [])
    if not isinstance(edges, list):
        raise ValueError(f"{where}: edges must be a list")
    for e in edges:
        if (
            not isinstance(e, dict)
            or e.get("src") not in ids
            or e.get("dst") not in ids
        ):
            raise ValueError(f"{where}: edge {e!r} must join two chain nodes")
        if kind_of[e["dst"]].endswith("_inlet"):
            raise ValueError(
                f"{where}: edge into inlet {e['dst']!r}; it is fed from outside"
            )
        if kind_of[e["src"]].endswith("_outlet"):
            raise ValueError(
                f"{where}: edge from outlet {e['src']!r}; it feeds the outside"
            )


def expand(nodes: list, edges: list) -> tuple[list, list]:
    """Replace each ``chain`` node with its inner nodes. Raises ValueError.

    Inner ids become ``<chain id>/<inner id>``. An edge to a chain id goes to
    its ``dst_port``-th inlet; one from a chain id leaves from its
    ``src_port``-th outlet. Edges may not name a node inside a chain.
    """
    chains: dict[str, dict] = {}
    flat: list = []
    for n in nodes:
        if isinstance(n, dict) and n.get("kind") in _PORT_KINDS:
            raise ValueError(f"{n['kind']} {n.get('id')} is outside a chain")
        if not isinstance(n, dict) or n.get("kind") != "chain":
            flat.append(n)
            continue
        cid = n.get("id")
        if not isinstance(cid, str) or not cid or SEP in cid:
            raise ValueError(f"chain id {cid!r} must be non-empty, no '/'")
        validate(n, f"chain {cid}")
        chains[cid] = n
        for inner in n["nodes"]:
            flat.append({**inner, "id": cid + SEP + inner["id"]})
    if not chains:
        return nodes, edges

    def port(cid: str, midi: bool, inlet: bool, k: Any) -> str:
        ports = _ports(chains[cid], midi, inlet)
        if isinstance(k, bool) or not isinstance(k, int) or not 0 <= k < len(ports):
            what = "MIDI" if midi else "audio"
            kind = "inlet" if inlet else "outlet"
            raise ValueError(f"chain {cid} has no {what} {kind} {k}")
        return cid + SEP + ports[k]

    out_edges: list = []
    for cid, c in chains.items():
        for e in c.get("edges", []):
            out_edges.append(
                {**e, "src": cid + SEP + e["src"], "dst": cid + SEP + e["dst"]}
            )
    for e in edges:
        if not isinstance(e, dict):
            out_edges.append(e)
            continue
        for end in ("src", "dst"):
            head, sep, _ = str(e.get(end)).partition(SEP)
            if sep and head in chains:
                raise ValueError(
                    f"edge {e.get('src')} -> {e.get('dst')}: connect to chain "
                    f"{head}, not to a node inside it"
                )
        midi = e.get("kind") == "midi"
        ne = dict(e)
        if e.get("src") in chains:
            ne["src"] = port(e["src"], midi, False, ne.pop("src_port", 0))
        if e.get("dst") in chains:
            ne["dst"] = port(e["dst"], midi, True, e.get("dst_port", 0))
            if midi:
                ne.pop("dst_port", None)
            else:
                ne["dst_port"] = 0
        out_edges.append(ne)
    return flat, out_edges
