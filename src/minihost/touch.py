"""Generate a touch control surface from a plugin's parameters.

Produces two files from one parameter table, which is the whole point of doing
this here rather than piping two command-line tools together: the layout and
the host's mapping cannot disagree, because they are rendered from the same
rows.

  - ``<name>.ui.json`` -- a layout description in the ``py2tosc.ui`` dialect
  - ``<name>.map.json`` -- a ``minihost play --map-file`` mapping

The layout targets ``py2tosc.ui_json`` rather than the flat
``py2tosc.surface`` list, and rather than emitting a ``.tosc`` directly. Three
reasons:

- **Generation needs no dependency.** This module writes JSON text and imports
  nothing from py2tosc. py2tosc is needed only to compile the result into a
  ``.tosc``, so when it is absent the generator still produces a complete,
  valid description and says how to compile it -- rather than failing and
  producing nothing.
- **The output is a source file, not an artefact.** A ``.tosc`` is a zipped XML
  blob nobody hand-edits, so a generator that emits one owns every layout
  decision forever. A ``.ui.json`` is reviewable, diffable and editable: move a
  control, change a gap, add a page, recompile.
- **``each`` is a parameter table.** The dialect walks a list of rows binding
  every field, which is exactly the shape of a plugin's parameter list.

Widget choice needs a branch table, because substitution in that dialect
reaches values and never keys -- so the tag naming a node is fixed in a
template and one ``each`` would otherwise build one kind of control. py2tosc
0.5.2 added ``of: {case, when}`` for this (at minihost's request), which is
``ui_json`` schema 2.
"""

from __future__ import annotations

import json
import re
from typing import Any, Optional

__all__ = ["Parameter", "build_layout", "build_map", "collect_parameters", "slugify"]

#: MIDI has 128 controller numbers. A plugin can have far more parameters;
#: those past the end go out over OSC alone.
CC_LIMIT = 128

#: Design canvas. TouchOSC scales a layout to whatever screen opens it, so
#: this is an aspect ratio and a coordinate space rather than a pixel count.
DEFAULT_SIZE = (1024, 768)

#: Controls per page. Four across and three down reads well on a tablet.
DEFAULT_COLUMNS = 4
DEFAULT_ROWS = 3


def slugify(text: str) -> str:
    """An OSC-safe name. Same rules as :func:`minihost.slug`.

    Duplicated rather than imported from ``control`` so this module stays a
    pure JSON emitter with no import of the extension at generation time.
    ``tests/test_touch.py`` asserts the two agree.
    """
    words = re.findall(r"[A-Za-z0-9]+", text)
    if not words:
        return "parameter"
    return words[0].lower() + "".join(word.capitalize() for word in words[1:])


class Parameter:
    """One control's worth of a plugin's parameter list."""

    __slots__ = ("index", "name", "slug", "kind", "steps", "unit", "default", "cc")

    def __init__(
        self,
        index: int,
        name: str,
        slug: str,
        kind: str,
        steps: int = 0,
        unit: str = "",
        default: float = 0.0,
        cc: Optional[int] = None,
    ):
        self.index = index
        self.name = name
        self.slug = slug
        self.kind = kind
        self.steps = steps
        self.unit = unit
        self.default = default
        self.cc = cc

    def __repr__(self) -> str:
        return f"Parameter({self.index}, {self.name!r}, kind={self.kind!r})"


def collect_parameters(
    plugin: Any,
    automatable_only: bool = True,
    indices: Optional[list[int]] = None,
) -> list[Parameter]:
    """Read a plugin's parameters into the table everything else renders from.

    Widget choice comes from metadata minihost already exports and the flat
    ``py2tosc.surface`` path discards: ``is_boolean`` becomes a button,
    ``num_steps`` a radio, everything else a fader.

    Names are slugged and duplicates numbered, because real plugins repeat
    names -- a compressor with three parameters called "Bypass" is ordinary --
    and two controls sharing one OSC address makes the second unreachable.

    CC numbers are assigned in order until they run out at 128. The remainder
    keep their OSC addresses and get no CC.
    """
    chosen = indices if indices is not None else range(plugin.num_params)

    seen: dict[str, int] = {}
    params: list[Parameter] = []
    next_cc = 0

    for index in chosen:
        info = plugin.get_param_info(index)
        if automatable_only and not info.get("is_automatable", True):
            continue

        name = info.get("name", f"param{index}")
        base = slugify(name)
        seen[base] = seen.get(base, 0) + 1
        unique = base if seen[base] == 1 else f"{base}{seen[base]}"

        steps = int(info.get("num_steps") or 0)
        if info.get("is_boolean"):
            kind, steps = "toggle", 0
        elif steps > 1:
            kind = "stepped"
        else:
            kind, steps = "continuous", 0

        cc: Optional[int] = None
        if next_cc < CC_LIMIT:
            cc = next_cc
            next_cc += 1

        params.append(
            Parameter(
                index=index,
                name=name,
                slug=unique,
                kind=kind,
                steps=steps,
                unit=str(info.get("label", "") or ""),
                default=float(info.get("default_value", 0.0) or 0.0),
                cc=cc,
            )
        )

    return params


def _branch(
    tag: str, with_osc: bool, with_cc: bool, extra: Optional[dict] = None
) -> dict:
    """One arm of the case table.

    Each arm is a fully-written node, so the whole table is checked against
    the tag table before any row is read -- a branch naming two tags or none
    is refused whether or not a row reaches it.

    There are two arms per widget kind because a parameter past the 128th has
    no controller number left, and ``each`` cannot conditionally include a
    message: a row picks a branch, and the branch either has a ``midi_cc``
    binding or does not. A branch nothing selects is not an error, so a
    surface with fewer than 128 parameters simply never reaches the no-CC
    arms.
    """
    messages: list[dict] = []
    if with_osc:
        messages.append({"osc": "$address"})
    if with_cc:
        messages.append({"midi_cc": "$cc"})

    node: dict[str, Any] = {tag: "$name", "messages": messages}
    if extra:
        node.update(extra)
    return node


def _branch_table(midi: bool, osc: bool) -> dict:
    """Every widget kind, with and without a controller number."""
    table: dict[str, Any] = {}
    for kind, tag, extra in (
        ("continuous", "fader", None),
        ("toggle", "button", None),
        ("stepped", "radio", {"steps": "$steps"}),
    ):
        table[kind] = _branch(tag, with_osc=osc, with_cc=midi, extra=extra)
        table[f"{kind}NoCc"] = _branch(tag, with_osc=osc, with_cc=False, extra=extra)
    return table


def build_layout(
    params: list[Parameter],
    prefix: str = "/mh/param",
    size: tuple[int, int] = DEFAULT_SIZE,
    columns: int = DEFAULT_COLUMNS,
    rows: int = DEFAULT_ROWS,
    midi: bool = True,
    osc: bool = True,
    plugin_name: str = "",
    version: str = "",
) -> dict:
    """Render the parameter table as a ``py2tosc.ui`` layout description.

    Rows keep the plugin's own parameter order, which is information the
    plugin author chose. That is why this uses one ``each`` over a
    ``case``/``when`` branch table rather than one ``each`` per widget kind:
    grouping by widget would regroup the surface and throw that order away.
    """
    if not params:
        raise ValueError("a surface needs at least one parameter")
    if not midi and not osc:
        raise ValueError("a surface with neither MIDI nor OSC would do nothing")

    prefix = prefix.rstrip("/")
    per_page = max(1, columns * rows)

    def row_for(p: Parameter) -> dict:
        has_cc = midi and p.cc is not None
        row: dict[str, Any] = {
            "kind": p.kind if has_cc else f"{p.kind}NoCc",
            "name": p.slug,
            "caption": p.name if not p.unit else f"{p.name} ({p.unit})",
        }
        if osc:
            row["address"] = f"{prefix}/{p.slug}"
        if has_cc:
            row["cc"] = p.cc
        if p.kind == "stepped":
            row["steps"] = p.steps
        return row

    pages = []
    for start in range(0, len(params), per_page):
        page = params[start : start + per_page]
        pages.append(
            {
                "tiles": [
                    {
                        "each": [row_for(p) for p in page],
                        "of": {"case": "$kind", "when": _branch_table(midi, osc)},
                    }
                ],
                "columns": columns,
                "rows": rows,
                "gap": 6,
                "pad": 8,
                "name": f"page{start // per_page + 1}",
            }
        )

    header = f"generated by minihost{' ' + version if version else ''}"
    if plugin_name:
        header += f" from {plugin_name}"

    root: dict[str, Any] = {
        "//": header,
        "pager": pages,
        "name": "surface",
    }

    return {
        "format": "py2tosc.ui",
        # Stamped explicitly rather than left to default. ui_json is read and
        # never written, so the producer stamps -- and a description with no
        # schema key means "whatever the reader is", which is the ambiguity a
        # version number exists to remove. 2 is the schema that introduced the
        # case/when branch tables this layout uses.
        "schema": 2,
        "//": header,
        "root": {
            "stack": [root],
            "frame": [0, 0, size[0], size[1]],
            "name": "surface",
        },
    }


def build_map(params: list[Parameter], channel: int = 0) -> dict:
    """Render the same table as a ``minihost play --map-file`` mapping.

    Rendered from the identical rows as the layout, so the two cannot drift.
    Parameters past the CC limit are omitted here and reachable over OSC only.
    """
    mappings = []
    for p in params:
        if p.cc is None:
            continue
        mappings.append({"channel": channel, "cc": p.cc, "param": p.name})
    return {"mappings": mappings}


def write_files(
    params: list[Parameter],
    out_base: str,
    **layout_kwargs: Any,
) -> tuple[str, str]:
    """Write ``<out_base>.ui.json`` and ``<out_base>.map.json``.

    Returns the two paths.
    """
    layout_path = f"{out_base}.ui.json"
    map_path = f"{out_base}.map.json"

    with open(layout_path, "w") as f:
        json.dump(build_layout(params, **layout_kwargs), f, indent=2)
        f.write("\n")

    with open(map_path, "w") as f:
        json.dump(build_map(params), f, indent=2)
        f.write("\n")

    return layout_path, map_path
