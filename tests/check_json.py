#!/usr/bin/env python3
"""Check a py2tosc JSON file without py2tosc installed.

Both dialects are handled, told apart the way `py2tosc.load` tells them apart,
by the envelope's `format`: `py2tosc.ui`, the layout description that the
combinators build, and `py2tosc.layout`, the faithful node tree.

    python check_json.py synth.ui.json
    python check_json.py --quiet layout.json

It is one file, imports nothing but the standard library, and is meant to be
copied into a project that *writes* these files. That is the case it exists
for: a generator emitting a description wants its own tests to say the file is
well formed, and requiring py2tosc for that would put the compiler in the
dependency list of a program whose whole point is not to need one.

    from check_json import check

    problems = check(json.loads(text))
    assert not problems, problems

**py2tosc remains the authority.** This is a shape checker, deliberately
conservative: everything it rejects, py2tosc rejects too, so a clean run here
is not a promise that the file builds. What it cannot see:

- whether a layout fits -- `sizes` against the children they divide, a row
  narrower than the controls in it, anything `ui.resolve` computes;
- whether a property *value* coerces, beyond the obvious type mistakes;
- what a schema newer than the tables below introduced.

What it does see is the failure class this format has to close, and the one a
generator hits: a key nothing reads, which is silently ignored rather than
refused, so a `childs` drops a subtree and the result looks like a file that
read correctly. Plus, for the description dialect, a `$name` no repeat binds, a
binding a control cannot carry, and a `schema` stamped below the spellings the
file actually uses.

The tables are generated from py2tosc itself by `scripts/make_check_json.py`,
and `tests/test_check_json.py` fails when they drift. Regenerate rather than
edit them by hand; everything outside the generated block is ordinary source.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
from typing import Any

__all__ = ["check", "main", "required_schema"]

ERROR = "error"
WARNING = "warning"

# --- generated tables: do not edit, see scripts/make_check_json.py ----------
TABLES: dict[str, Any] = {
    "layout": {
        "control_types": [
            "BOX",
            "BUTTON",
            "LABEL",
            "TEXT",
            "FADER",
            "XY",
            "RADIAL",
            "ENCODER",
            "RADAR",
            "RADIO",
            "GROUP",
            "PAGER",
            "GRID",
        ],
        "envelope_keys": ["format", "lexml", "root", "schema"],
        "format": "py2tosc.layout",
        "known_types": {
            "background": "b",
            "bar": "b",
            "barDisplay": "i",
            "buttonType": "i",
            "centered": "b",
            "color": "c",
            "cornerRadius": "f",
            "cursor": "b",
            "cursorDisplay": "i",
            "exclusive": "b",
            "font": "i",
            "frame": "r",
            "grabFocus": "b",
            "grid": "b",
            "gridColor": "c",
            "gridNaming": "i",
            "gridOrder": "i",
            "gridStart": "i",
            "gridSteps": "i",
            "gridStepsX": "i",
            "gridStepsY": "i",
            "gridType": "i",
            "gridX": "i",
            "gridY": "i",
            "interactive": "b",
            "inverted": "b",
            "lines": "b",
            "linesDisplay": "i",
            "lockX": "b",
            "lockY": "b",
            "locked": "b",
            "name": "s",
            "orientation": "i",
            "outline": "b",
            "outlineStyle": "i",
            "pointerPriority": "i",
            "press": "b",
            "radioType": "i",
            "release": "b",
            "response": "i",
            "responseFactor": "i",
            "script": "s",
            "shape": "i",
            "steps": "i",
            "tabColorOff": "c",
            "tabColorOn": "c",
            "tabLabel": "s",
            "tabLabels": "b",
            "tabbar": "b",
            "tabbarDoubleTap": "b",
            "tabbarSize": "i",
            "tag": "s",
            "textAlignH": "i",
            "textAlignV": "i",
            "textClip": "b",
            "textColor": "c",
            "textColorOff": "c",
            "textColorOn": "c",
            "textLength": "i",
            "textSize": "i",
            "textSizeOff": "i",
            "textSizeOn": "i",
            "textWrap": "b",
            "valuePosition": "b",
            "visible": "b",
        },
        "messages": ["gamepad", "local", "midi", "osc"],
        "node_keys": [
            "children",
            "id",
            "includes",
            "messages",
            "properties",
            "type",
            "values",
        ],
        "schemas": [1],
    },
    "ui": {
        "ambiguous": ["grid", "inset", "text"],
        "arrange": ["column", "pager", "row", "stack", "tiles"],
        "carries": {
            "BOX": ["touch"],
            "BUTTON": ["touch", "x"],
            "ENCODER": ["touch", "x", "y"],
            "FADER": ["touch", "x"],
            "GRID": ["touch"],
            "GROUP": ["touch"],
            "LABEL": ["text", "touch"],
            "PAGER": ["page", "touch"],
            "RADAR": ["touch", "x", "y"],
            "RADIAL": ["touch", "x"],
            "RADIO": ["touch", "x"],
            "TEXT": ["text", "touch"],
            "XY": ["touch", "x", "y"],
        },
        "choice_keys": ["case", "when"],
        "common_keys": ["id", "messages", "props", "values"],
        "control_types": [
            "BOX",
            "BUTTON",
            "LABEL",
            "TEXT",
            "FADER",
            "XY",
            "RADIAL",
            "ENCODER",
            "RADAR",
            "RADIO",
            "GROUP",
            "PAGER",
            "GRID",
        ],
        "dialect": "py2tosc.ui",
        "envelope_keys": ["format", "lexml", "root", "schema"],
        "messages": {
            "connect": ["enabled", "on", "source", "to", "triggers", "var"],
            "midi_cc": [
                "channel",
                "connections",
                "enabled",
                "feedback",
                "no_duplicates",
                "on",
                "receive",
                "scale",
                "send",
                "source",
                "triggers",
                "var",
            ],
            "midi_note": [
                "channel",
                "connections",
                "enabled",
                "feedback",
                "no_duplicates",
                "on",
                "receive",
                "scale",
                "send",
                "source",
                "triggers",
                "var",
            ],
            "osc": [
                "args",
                "connections",
                "enabled",
                "feedback",
                "no_duplicates",
                "on",
                "receive",
                "send",
                "triggers",
                "var",
            ],
        },
        "options": {
            "column": ["gap", "pad", "sizes"],
            "grid": ["columns", "rows"],
            "group": [],
            "inset": ["by"],
            "labelled": ["caption", "inset", "size"],
            "pager": ["pad"],
            "row": ["gap", "pad", "sizes"],
            "stack": ["pad"],
            "tiles": ["columns", "gap", "pad", "rows"],
        },
        "partial_args": {
            "connect": ["source", "to"],
            "midi_cc": ["channel", "source"],
            "midi_note": ["channel", "source"],
            "osc": ["args"],
        },
        "partials": ["const", "index", "prop", "value"],
        "produces": {
            "box": "BOX",
            "button": "BUTTON",
            "column": "GROUP",
            "encoder": "ENCODER",
            "fader": "FADER",
            "grid": "GRID",
            "group": "GROUP",
            "label": "LABEL",
            "labelled": "GROUP",
            "pager": "PAGER",
            "radar": "RADAR",
            "radial": "RADIAL",
            "radio": "RADIO",
            "row": "GROUP",
            "stack": "GROUP",
            "text": "TEXT",
            "tiles": "GROUP",
            "xy": "XY",
        },
        "properties": {
            "BOX": [
                "background",
                "color",
                "cornerRadius",
                "frame",
                "grabFocus",
                "interactive",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "script",
                "shape",
                "tag",
                "visible",
            ],
            "BUTTON": [
                "background",
                "buttonType",
                "color",
                "cornerRadius",
                "frame",
                "grabFocus",
                "interactive",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "press",
                "release",
                "script",
                "shape",
                "tag",
                "valuePosition",
                "visible",
            ],
            "ENCODER": [
                "background",
                "color",
                "cornerRadius",
                "cursor",
                "cursorDisplay",
                "frame",
                "grabFocus",
                "grid",
                "gridColor",
                "gridSteps",
                "interactive",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "response",
                "responseFactor",
                "script",
                "shape",
                "tag",
                "visible",
            ],
            "FADER": [
                "background",
                "bar",
                "barDisplay",
                "centered",
                "color",
                "cornerRadius",
                "cursor",
                "cursorDisplay",
                "frame",
                "grabFocus",
                "grid",
                "gridColor",
                "gridSteps",
                "interactive",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "response",
                "responseFactor",
                "script",
                "shape",
                "tag",
                "visible",
            ],
            "GRID": [
                "background",
                "color",
                "cornerRadius",
                "exclusive",
                "frame",
                "grabFocus",
                "gridNaming",
                "gridOrder",
                "gridStart",
                "gridType",
                "gridX",
                "gridY",
                "interactive",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "script",
                "shape",
                "tag",
                "visible",
            ],
            "GROUP": [
                "background",
                "color",
                "cornerRadius",
                "frame",
                "grabFocus",
                "interactive",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "script",
                "shape",
                "tabColorOff",
                "tabColorOn",
                "tabLabel",
                "tag",
                "textColorOff",
                "textColorOn",
                "visible",
            ],
            "LABEL": [
                "background",
                "color",
                "cornerRadius",
                "font",
                "frame",
                "grabFocus",
                "interactive",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "script",
                "shape",
                "tag",
                "textAlignH",
                "textAlignV",
                "textClip",
                "textColor",
                "textLength",
                "textSize",
                "visible",
            ],
            "PAGER": [
                "background",
                "color",
                "cornerRadius",
                "frame",
                "grabFocus",
                "interactive",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "script",
                "shape",
                "tabLabels",
                "tabbar",
                "tabbarDoubleTap",
                "tabbarSize",
                "tag",
                "textSizeOff",
                "textSizeOn",
                "visible",
            ],
            "RADAR": [
                "background",
                "color",
                "cornerRadius",
                "cursor",
                "cursorDisplay",
                "frame",
                "grabFocus",
                "gridColor",
                "gridStepsX",
                "gridStepsY",
                "gridX",
                "gridY",
                "interactive",
                "lines",
                "linesDisplay",
                "lockX",
                "lockY",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "script",
                "shape",
                "tag",
                "visible",
            ],
            "RADIAL": [
                "background",
                "centered",
                "color",
                "cornerRadius",
                "cursor",
                "cursorDisplay",
                "frame",
                "grabFocus",
                "grid",
                "gridColor",
                "gridSteps",
                "interactive",
                "inverted",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "response",
                "responseFactor",
                "script",
                "shape",
                "tag",
                "visible",
            ],
            "RADIO": [
                "background",
                "color",
                "cornerRadius",
                "frame",
                "grabFocus",
                "interactive",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "radioType",
                "script",
                "shape",
                "steps",
                "tag",
                "visible",
            ],
            "TEXT": [
                "background",
                "color",
                "cornerRadius",
                "font",
                "frame",
                "grabFocus",
                "interactive",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "script",
                "shape",
                "tag",
                "textAlignH",
                "textAlignV",
                "textClip",
                "textColor",
                "textSize",
                "textWrap",
                "visible",
            ],
            "XY": [
                "background",
                "color",
                "cornerRadius",
                "cursor",
                "cursorDisplay",
                "frame",
                "grabFocus",
                "gridColor",
                "gridStepsX",
                "gridStepsY",
                "gridX",
                "gridY",
                "interactive",
                "lines",
                "linesDisplay",
                "lockX",
                "lockY",
                "locked",
                "name",
                "orientation",
                "outline",
                "outlineStyle",
                "pointerPriority",
                "response",
                "responseFactor",
                "script",
                "shape",
                "tag",
                "visible",
            ],
        },
        "repeat_keys": ["as", "case", "each", "from", "of", "repeat", "when"],
        "schemas": [1, 2, 3],
        "slots": {
            "column": "children",
            "group": "children",
            "messages": "bindings",
            "pager": "children",
            "row": "children",
            "stack": "children",
            "tiles": "children",
        },
        "tags": [
            "box",
            "button",
            "column",
            "encoder",
            "fader",
            "grid",
            "group",
            "inset",
            "label",
            "labelled",
            "pager",
            "radar",
            "radial",
            "radio",
            "row",
            "stack",
            "text",
            "tiles",
            "xy",
        ],
        "takes": {
            "connect": [["str"], "the name of the control it writes to"],
            "midi_cc": [
                ["int", "dict"],
                "a controller number or a partial that reads one",
            ],
            "midi_note": [["int", "dict"], "a note number or a partial that reads one"],
            "osc": [["str"], "an address"],
        },
        "triggers": ["ANY", "RISE", "FALL"],
        "value_fields": [
            "key",
            "locked",
            "locked_default_current",
            "default",
            "default_pull",
        ],
        "value_sugar": ["text"],
    },
}
# --- end generated tables ---------------------------------------------------

#: Keys the format stores under more than one type, so a mismatch means
#: nothing: `gridX`/`gridY` count elements on a GRID and switch lines on and
#: off on an XY or RADAR.
AMBIGUOUS_TYPES = frozenset({"gridX", "gridY"})

_COMMENT = "//"
_PLACEHOLDER = re.compile(r"\$(?:\$|\{(\w+)\}|(\w+))")
_SNAKE_BOUNDARY = re.compile(r"_([a-z0-9])")


def _to_camel(name: str) -> str:
    """A `snake_case` key as the format spells it, camelCase left alone."""
    return _SNAKE_BOUNDARY.sub(lambda m: m.group(1).upper(), name)


def _describe(value: Any) -> str:
    """What a reader wants to be told they wrote instead."""
    return {
        dict: "an object",
        list: "a list",
        str: "a string",
        bool: "a boolean",
        int: "a number",
        float: "a number",
        type(None): "null",
    }.get(type(value), type(value).__name__)


class _Report:
    """Findings, in the order they were found."""

    def __init__(self) -> None:
        self.found: list[str] = []

    def error(self, where: str, message: str) -> None:
        self.found.append(f"{ERROR}: {where}: {message}")

    def warn(self, where: str, message: str) -> None:
        self.found.append(f"{WARNING}: {where}: {message}")

    def object(self, data: Any, where: str) -> dict[str, Any] | None:
        """Insist on an object, and drop the comments from it."""
        if not isinstance(data, dict):
            self.error(where, f"should be an object, found {_describe(data)}")
            return None
        return {k: v for k, v in data.items() if not k.startswith(_COMMENT)}

    def list(self, data: Any, where: str) -> list[Any] | None:
        if not isinstance(data, list):
            self.error(where, f"should be a list, found {_describe(data)}")
            return None
        return data

    def keys(self, entry: dict[str, Any], allowed: set[str], where: str) -> None:
        """Refuse a key nobody will read, with the nearest spelling."""
        for key in entry:
            if key in allowed:
                continue
            near = difflib.get_close_matches(key, sorted(allowed), n=2)
            hint = (
                "did you mean " + " or ".join(repr(m) for m in near) + "?"
                if near
                else "expected one of " + ", ".join(sorted(allowed))
            )
            self.error(where, f"unknown key {key!r}; {hint}")


# -- the schema a description needs ------------------------------------------


def _needs(entry: dict[str, Any]) -> int:
    """The schema one node needs, ignoring what is nested inside it.

    A hand copy of the table in `py2tosc.ui_json`, because no generated table
    can carry code. `tests/test_schema_corpus.py` in py2tosc runs both over one
    description per schema, which is what stops the two answering differently.
    """
    ui = TABLES["ui"]
    for key in ui["slots"]:
        held = entry.get(key)
        if isinstance(held, list) and any(_is_choice(item) for item in held):
            return 3
    if _is_choice(entry):
        table = entry.get("when")
        if isinstance(table, dict) and any(
            isinstance(branch, list) for branch in table.values()
        ):
            return 3

    held = entry.get("of")
    if ("repeat" in entry or "each" in entry) and _is_choice(held):
        return 2
    return int(ui["schemas"][0])


def required_schema(data: Any) -> int:
    """The lowest schema that builds a description: the number to stamp.

    The same answer `py2tosc.ui_json.required_schema` gives, from the same
    table. It detects spellings and never meanings, so a schema that changed
    what an existing spelling does is invisible to it.
    """
    needed = int(TABLES["ui"]["schemas"][0])
    pending: list[Any] = [data]
    while pending:
        item = pending.pop()
        if isinstance(item, dict):
            needed = max(needed, _needs(item))
            pending.extend(item.values())
        elif isinstance(item, list):
            pending.extend(item)
    return needed


# -- the layout description --------------------------------------------------


def _names(text: str) -> list[str]:
    """Every `$name` one string reads, ignoring `$$`."""
    return [
        m.group(1) or m.group(2)
        for m in _PLACEHOLDER.finditer(text)
        if m.group(1) or m.group(2)
    ]


def _interpolate(text: str, bindings: dict[str, Any]) -> Any:
    """One string with its names filled in, keeping a lone name's type."""
    whole = _PLACEHOLDER.fullmatch(text)
    if whole is not None and (whole.group(1) or whole.group(2)):
        name = whole.group(1) or whole.group(2)
        if name in bindings:
            return bindings[name]
        return text

    def swap(match: re.Match[str]) -> str:
        name = match.group(1) or match.group(2)
        if name is None:
            return "$"
        if name not in bindings:
            return match.group(0)
        found = bindings[name]
        return str(found).lower() if isinstance(found, bool) else str(found)

    return _PLACEHOLDER.sub(swap, text)


def _spelled(value: Any) -> str:
    return str(value).lower() if isinstance(value, bool) else str(value)


def _is_choice(described: Any) -> bool:
    """Whether one object is a choice rather than a node."""
    if not isinstance(described, dict):
        return False
    named = {k for k in described if not k.startswith(_COMMENT)}
    return bool({"case", "when"} & named)


def _branch_list(branch: Any) -> list[Any]:
    """One branch as the list it stands for, which may be empty."""
    return branch if isinstance(branch, list) else [branch]


class _Description:
    """One `py2tosc.ui` file, checked node by node."""

    def __init__(self, report: _Report) -> None:
        self.report = report
        self.ui = TABLES["ui"]
        self.named: dict[str, int] = {}
        self.connects: list[tuple[str, str]] = []
        #: Whether any control's name came out of a repeat, which makes the
        #: roll of names partial and `connect` unanswerable from here.
        self.expanded = False

    # -- repetition ----------------------------------------------------------

    def rows(self, entry: dict[str, Any], where: str) -> list[dict[str, Any]] | None:
        """What each pass of one repeat binds beyond its counters."""
        counter = entry.get("as", "i")
        if not isinstance(counter, str) or not counter.isidentifier():
            self.report.error(where, f"as should name a counter, found {counter!r}")
            return None

        if "repeat" in entry and "each" in entry:
            self.report.error(
                where,
                "a repeat counts with `repeat` or walks a list with `each`, not both",
            )
            return None

        if "each" in entry:
            records = self.report.list(entry["each"], f"{where}.each")
            if records is None:
                return None
            rows = []
            for index, record in enumerate(records):
                spot = f"{where}.each[{index}]"
                fields = self.report.object(record, spot)
                if fields is None:
                    return None
                for key, value in fields.items():
                    if not key.isidentifier():
                        self.report.error(
                            spot,
                            f"a field is read as ${key}, and {key!r} cannot be "
                            f"written that way, so nothing could reach it",
                        )
                    elif key in (counter, f"{counter}0"):
                        self.report.error(
                            spot,
                            f"{key!r} is this repeat's own counter; rename one "
                            f"of the two with `as`",
                        )
                    elif value is None or isinstance(value, (dict, list)):
                        self.report.error(
                            spot,
                            f"{key} should be a string, a number or a boolean, "
                            f"found {_describe(value)}",
                        )
                rows.append(fields)
            return rows

        count = entry.get("repeat")
        if isinstance(count, bool) or not isinstance(count, int):
            self.report.error(
                where, f"repeat should be a number, found {_describe(count)}"
            )
            return None
        if count < 1:
            self.report.error(where, f"repeat is {count}, which is less than 1")
            return None
        return [{} for _ in range(count)]

    def repeat(self, entry: dict[str, Any], where: str, bound: dict[str, Any]) -> None:
        """Check one `repeat` or `each`, and the node it stands for."""
        if "of" in entry:
            also = [k for k in entry if k in self.ui["tags"]]
            if also:
                named = " and ".join(repr(k) for k in also)
                self.report.error(
                    where,
                    f"a repeat holds the node it repeats under `of` or is that "
                    f"node itself, not both; {named} names one and `of` holds "
                    f"another",
                )
                return
            self.report.keys(entry, set(self.ui["repeat_keys"]), where)
            described: Any = entry["of"]
        elif any(k in self.ui["tags"] for k in entry):
            described = {
                k: v for k, v in entry.items() if k not in self.ui["repeat_keys"]
            }
        else:
            if {"case", "when"} & set(entry):
                self.report.error(
                    where,
                    "a choice is what a repeat repeats, so it goes under `of`; "
                    "the short form is the node itself",
                )
                return
            self.report.keys(entry, set(self.ui["repeat_keys"]), where)
            self.report.error(
                where, "a repeat needs an `of` to repeat, or a tag of its own"
            )
            return

        start = entry.get("from", 1)
        if isinstance(start, bool) or not isinstance(start, int) or start < 0:
            self.report.error(where, f"from should be a number, found {start!r}")
            start = 1
        counter = entry.get("as", "i")
        if not isinstance(counter, str):
            counter = "i"

        rows = self.rows(entry, where)
        if rows is None:
            return

        choice = None
        if "of" in entry and _is_choice(described):
            choice = self.check_choice(described, "node", f"{where}.of")
            if choice is None:
                return  # the choice itself was refused; nothing left to check

        for step, row in enumerate(rows):
            bindings: dict[str, Any] = {counter: start + step, f"{counter}0": step}
            bindings.update(row)
            here = {**bound, **bindings}
            spot = f"{where}#{step + 1}"
            if choice is None:
                self.node(described, spot, here)
                continue
            picked = self.pick(choice, bindings, spot)
            if picked is not None:
                self.node(picked, spot, here)

    # -- nodes ---------------------------------------------------------------

    def check_choice(
        self, entry: dict[str, Any], slot: str, where: str
    ) -> dict[str, Any] | None:
        """A choice appearing among children or among bindings.

        Shape only, and every branch rather than the one taken -- which is
        exactly what the reader checks before expansion, so this refuses
        nothing it would accept.
        """
        held = self.report.object(entry, where)
        if held is None:
            return None
        self.report.keys(held, set(self.ui["choice_keys"]), where)
        missing = sorted(set(self.ui["choice_keys"]) - set(held))
        if missing:
            self.report.error(
                where,
                f"a choice reads a field with `case` and holds the branches it "
                f"chooses between under `when`; {missing[0]!r} is missing",
            )
            return None
        if not isinstance(held["case"], str):
            self.report.error(
                where,
                f"case reads the field that chooses a branch, "
                f"found {_describe(held['case'])}",
            )
            return None

        table = self.report.object(held["when"], f"{where}.when")
        if table is None:
            return None
        if not table:
            self.report.error(f"{where}.when", "a choice needs a branch to choose")
            return None

        for name, branch in table.items():
            spot = f"{where}.when[{name!r}]"
            for index, node in enumerate(_branch_list(branch)):
                at = spot if not isinstance(branch, list) else f"{spot}[{index}]"
                written = self.report.object(node, at)
                if written is None:
                    continue
                if slot == "bindings":
                    self.binding_kind(written, at)
                else:
                    self.tag(written, at)
        return held

    def pick(self, entry: dict[str, Any], bound: dict[str, Any], where: str) -> Any:
        """Which branch this pass takes, or None with the reason reported.

        Unlike the reader, this walks with every enclosing repeat's names
        already in hand, so a selector that still will not resolve is one
        nothing binds rather than one an inner pass will fill.
        """
        picked = _interpolate(entry["case"], bound)
        if isinstance(picked, str) and _PLACEHOLDER.search(picked):
            if bound:
                known = ", ".join(f"${key}" for key in sorted(bound))
                self.report.error(
                    where,
                    f"case reads {entry['case']!r}, which is not one of the "
                    f"names this repeat binds ({known})",
                )
            else:
                self.report.error(
                    where,
                    "a choice is selected by a value a repeat binds, and "
                    "nothing here is inside a repeat",
                )
            return None
        name = _spelled(picked)
        if name not in entry["when"]:
            written = ", ".join(repr(key) for key in entry["when"])
            self.report.error(
                where,
                f"case read {name!r}, and no branch is written for it; "
                f"when holds {written}",
            )
            return None
        return entry["when"][name]

    def expand(
        self, data: Any, where: str, slot: str, bound: dict[str, Any]
    ) -> list[tuple[str, Any]]:
        """One list with every choice in it opened out where it stood."""
        opened: list[tuple[str, Any]] = []
        for index, item in enumerate(self.report.list(data, where) or []):
            spot = f"{where}[{index}]"
            if not _is_choice(item):
                opened.append((spot, item))
                continue
            held = self.check_choice(item, slot, spot)
            if held is None:
                continue
            picked = self.pick(held, bound, spot)
            if picked is None:
                continue
            for step, node in enumerate(_branch_list(picked)):
                opened.append((f"{spot}#{step + 1}", node))
        return opened

    def binding_kind(self, entry: dict[str, Any], where: str) -> str | None:
        """Which of the four a binding is, checked without building it."""
        found = [k for k in entry if k in self.ui["messages"]]
        if len(found) == 1:
            return found[0]
        kinds = ", ".join(sorted(self.ui["messages"]))
        self.report.error(
            where,
            f"a binding is one of {kinds}"
            + (f", found {' and '.join(repr(f) for f in found)}" if found else ""),
        )
        return None

    def tag(self, entry: dict[str, Any], where: str) -> str | None:
        """Which key names the thing, of the ones that could."""
        found = [k for k in entry if k in self.ui["tags"]]
        if len(found) > 1:
            found = [k for k in found if k not in self.ui["ambiguous"]] or found
        if len(found) == 1:
            return found[0]
        if not found:
            self.report.error(
                where,
                "nothing here names a control or a layout; expected one of "
                + ", ".join(sorted(self.ui["tags"])),
            )
            return None
        self.report.error(
            where,
            " and ".join(repr(f) for f in found) + " both name something; "
            "a node is one thing",
        )
        return None

    def strings(self, value: Any, where: str, bound: dict[str, Any]) -> None:
        """Every `$name` in one value's strings, against what is bound."""
        if isinstance(value, str):
            for name in _names(value):
                if name not in bound:
                    known = ", ".join(f"${k}" for k in sorted(bound))
                    self.report.error(
                        where,
                        f"${name} is not one of the names this repeat binds"
                        + (f" ({known})" if bound else ", and nothing binds one")
                        + "; write $$ for a literal dollar sign",
                    )
        elif isinstance(value, list):
            for item in value:
                self.strings(item, where, bound)
        elif isinstance(value, dict):
            for key, item in value.items():
                if not key.startswith(_COMMENT):
                    self.strings(item, where, bound)

    def node(self, data: Any, where: str, bound: dict[str, Any]) -> None:
        """Read one node: the tag, its argument, and everything hanging off it."""
        entry = self.report.object(data, where)
        if entry is None:
            return
        if "repeat" in entry or "each" in entry:
            self.report.error(
                where,
                "a repeat expands where children are accepted, and this is not "
                "one of those places",
            )
            return

        tag = self.tag(entry, where)
        if tag is None:
            return

        produced = self.ui["produces"].get(tag)
        accepts = set(self.ui["properties"].get(produced, ())) if produced else set()
        carries = set(self.ui["carries"].get(produced, ())) if produced else set()
        options = set(self.ui["options"].get(tag, ()))
        sugar = set(self.ui["value_sugar"]) & carries
        # A property may be written in either convention, so the snake_case
        # spelling of every one the type accepts is allowed alongside it.
        spellings = {_to_camel(key): key for key in entry}
        allowed = options | set(self.ui["common_keys"]) | sugar | {tag}
        allowed |= {spellings[key] for key in accepts & set(spellings)}
        self.report.keys(entry, allowed | accepts, where)

        for key, value in entry.items():
            if key in options or _to_camel(key) in accepts or key in sugar:
                self.strings(value, where, bound)

        self.argument(tag, entry, where, bound)
        self.extras(entry, where, bound)

    def argument(
        self, tag: str, entry: dict[str, Any], where: str, bound: dict[str, Any]
    ) -> None:
        """What the tag's one positional value has to be."""
        value = entry[tag]
        if tag in self.ui["arrange"] or tag == "group":
            for spot, child in self.expand(value, f"{where}.{tag}", "children", bound):
                self.child(child, spot, bound)
        elif tag == "grid":
            if not isinstance(value, str):
                self.report.error(
                    where,
                    f"grid takes the control type to replicate, "
                    f"found {_describe(value)}",
                )
            elif value not in self.ui["control_types"]:
                self.report.error(
                    where,
                    f"{value!r} is not a control type; expected one of "
                    + ", ".join(self.ui["control_types"]),
                )
        elif tag == "labelled":
            if "caption" not in entry:
                self.report.error(where, "labelled needs a caption")
            self.node(value, f"{where}.labelled", bound)
        elif tag == "inset":
            if "by" not in entry:
                self.report.error(where, "inset needs a `by` to shrink it by")
            self.node(value, f"{where}.inset", bound)
        elif isinstance(value, str):
            self.strings(value, where, bound)
            if _names(value):
                # A name a repeat fills in is a different name every pass, and
                # this never expands one, so the roll of names is incomplete
                # from here on and `finish` stops trusting it.
                self.expanded = True
            else:
                self.named[value] = self.named.get(value, 0) + 1
        elif value is not None:
            self.report.error(where, f"{tag} takes its name, found {_describe(value)}")

    def child(self, data: Any, where: str, bound: dict[str, Any]) -> None:
        entry = self.report.object(data, where)
        if entry is None:
            return
        if "repeat" in entry or "each" in entry:
            self.repeat(entry, where, bound)
        else:
            self.node(entry, where, bound)

    def extras(self, entry: dict[str, Any], where: str, bound: dict[str, Any]) -> None:
        """`id`, `props`, `values` and the bindings."""
        identifier = entry.get("id")
        if identifier is not None and not isinstance(identifier, str):
            self.report.error(
                where, f"id should be a string, found {_describe(identifier)}"
            )

        if "props" in entry:
            self.report.object(entry["props"], f"{where}.props")

        if "values" in entry:
            values = self.report.list(entry["values"], f"{where}.values")
            for index, item in enumerate(values or []):
                spot = f"{where}.values[{index}]"
                fields = self.report.object(item, spot)
                if fields is not None:
                    self.report.keys(fields, set(self.ui["value_fields"]), spot)

        if "messages" not in entry:
            return
        for spot, item in self.expand(
            entry["messages"], f"{where}.messages", "bindings", bound
        ):
            self.message(item, spot, bound)

    def message(self, data: Any, where: str, bound: dict[str, Any]) -> None:
        entry = self.report.object(data, where)
        if entry is None:
            return
        kind = self.binding_kind(entry, where)
        if kind is None:
            return
        self.report.keys(entry, set(self.ui["messages"][kind]) | {kind}, where)
        self.strings({k: v for k, v in entry.items() if k != kind}, where, bound)

        target = entry[kind]
        wanted, what = self.ui["takes"][kind]
        if isinstance(target, str) and _names(target):
            pass  # a name a repeat fills in; its type is not knowable here
        elif isinstance(target, bool) or type(target).__name__ not in wanted:
            self.report.error(where, f"{kind} takes {what}, found {_describe(target)}")

        if kind == "connect" and isinstance(target, str):
            self.connects.append((target, where))

        for key in sorted(set(self.ui["partial_args"][kind]) & set(entry)):
            spot = f"{where}.{key}"
            if key == "args":
                for index, item in enumerate(self.report.list(entry[key], spot) or []):
                    self.partial(item, f"{spot}[{index}]")
            elif isinstance(entry[key], dict):
                self.partial(entry[key], spot)

        if "triggers" in entry:
            spot = f"{where}.triggers"
            for index, item in enumerate(
                self.report.list(entry["triggers"], spot) or []
            ):
                fields = self.report.object(item, f"{spot}[{index}]")
                if fields is None:
                    continue
                self.report.keys(fields, {"var", "on"}, f"{spot}[{index}]")
                condition = fields.get("on")
                if condition is not None and condition not in self.ui["triggers"]:
                    self.report.error(
                        f"{spot}[{index}]",
                        f"{condition!r} is not a trigger condition; expected "
                        + ", ".join(self.ui["triggers"]),
                    )

    def partial(self, data: Any, where: str) -> None:
        entry = self.report.object(data, where)
        if entry is None:
            return
        found = [k for k in entry if k in self.ui["partials"]]
        if len(found) != 1:
            self.report.error(
                where,
                "a partial names one of " + ", ".join(sorted(self.ui["partials"])),
            )
            return
        self.report.keys(
            entry, set(self.ui["partials"]) | {"conversion", "scale"}, where
        )

    def finish(self) -> None:
        """Every `connect` against the names the description actually holds.

        A warning rather than an error, and only where the answer is knowable:
        this never expands a repeat, so a description that names controls from
        one has more names than were counted, and a miss would say nothing.
        py2tosc resolves these properly, against the built tree.
        """
        if self.expanded:
            return
        for target, where in self.connects:
            if _names(target):
                continue
            if target not in self.named:
                near = difflib.get_close_matches(target, sorted(self.named), n=1)
                hint = f"; did you mean {near[0]!r}?" if near else ""
                self.report.warn(
                    where,
                    f"no control in this description is named {target!r}{hint}",
                )


def _check_description(data: dict[str, Any], report: _Report) -> None:
    ui = TABLES["ui"]
    report.keys(data, set(ui["envelope_keys"]), "the layout")

    schemas = ui["schemas"]
    declared = data.get("schema")
    needed = required_schema(data)
    if declared is None:
        if needed > schemas[0]:
            report.warn(
                "<envelope>",
                f"the description carries no schema key and uses schema "
                f"{needed}; a release reading only schema {needed - 1} will "
                f"refuse it with a message about a node",
            )
    elif isinstance(declared, bool) or not isinstance(declared, int):
        report.error(
            "<envelope>", f"schema should be a number, found {_describe(declared)}"
        )
    elif declared > schemas[-1]:
        report.warn(
            "<envelope>",
            f"schema {declared} is newer than these tables describe "
            f"(schema {schemas[-1]}); regenerate this checker",
        )
    elif declared < needed:
        report.warn(
            "<envelope>",
            f"the description declares schema {declared} and uses schema "
            f"{needed}; a release reading only schema {needed - 1} will refuse "
            f"it with a message about a node",
        )

    version = data.get("lexml", "6")
    if not isinstance(version, str):
        report.error(
            "<envelope>", f"lexml should be a string, found {_describe(version)}"
        )

    if "root" not in data:
        report.error("the layout", "the layout holds no root node")
        return

    described = _Description(report)
    described.node(data["root"], "root", {})
    described.finish()


# -- the faithful encoding ---------------------------------------------------


def _check_node(data: Any, where: str, report: _Report) -> None:
    layout = TABLES["layout"]
    entry = report.object(data, where)
    if entry is None:
        return
    report.keys(entry, set(layout["node_keys"]), where)

    kind = entry.get("type")
    if kind is None:
        report.error(where, "a node needs a type")
    elif not isinstance(kind, str) or kind not in layout["control_types"]:
        report.error(
            where,
            f"{kind!r} is not a control type; expected one of "
            + ", ".join(layout["control_types"]),
        )

    if "id" in entry and not isinstance(entry["id"], str):
        report.error(where, f"id should be a string, found {_describe(entry['id'])}")

    properties = entry.get("properties")
    if properties is not None:
        fields = report.object(properties, f"{where}.properties")
        for name, value in (fields or {}).items():
            spot = f"{where}.properties.{name}"
            if not isinstance(value, list) or len(value) != 2:
                report.error(
                    spot,
                    "a property is written as its type tag and its value, "
                    f"found {_describe(value)}",
                )
                continue
            tag = value[0]
            if not isinstance(tag, str):
                report.error(
                    spot, f"the type tag should be a string, found {_describe(tag)}"
                )
                continue
            known = layout["known_types"].get(name)
            if known is not None and tag != known and name not in AMBIGUOUS_TYPES:
                # Not a warning: a property read under the wrong tag comes back
                # a different value, and the file still loads.
                report.error(
                    spot, f"{name} is stored under {known!r} and this says {tag!r}"
                )

    for key in ("values", "messages", "children"):
        if key in entry:
            report.list(entry[key], f"{where}.{key}")

    for index, child in enumerate(entry.get("children") or []):
        _check_node(child, f"{where}.children[{index}]", report)

    for index, message in enumerate(entry.get("messages") or []):
        spot = f"{where}.messages[{index}]"
        held = report.object(message, spot)
        if held is None:
            continue
        kind = held.get("kind")
        if kind not in layout["messages"]:
            report.error(
                spot,
                f"{kind!r} is not a binding kind; expected one of "
                + ", ".join(layout["messages"]),
            )


def _check_layout(data: dict[str, Any], report: _Report) -> None:
    layout = TABLES["layout"]
    report.keys(data, set(layout["envelope_keys"]), "the layout")

    declared = data.get("schema")
    if declared is not None:
        if isinstance(declared, bool) or not isinstance(declared, int):
            report.error(
                "<envelope>", f"schema should be a number, found {_describe(declared)}"
            )
        elif declared > layout["schemas"][-1]:
            report.warn(
                "<envelope>",
                f"schema {declared} is newer than these tables describe "
                f"(schema {layout['schemas'][-1]}); regenerate this checker",
            )

    if not isinstance(data.get("lexml", "6"), str):
        report.error("<envelope>", "lexml should be a string")

    if "root" not in data:
        report.error("the layout", "the layout holds no root node")
        return
    _check_node(data["root"], "root", report)


# -- the entry points --------------------------------------------------------


def check(data: Any) -> list[str]:
    """Every problem one decoded file has, worst first within its order.

    Args:
        data: The decoded JSON, as `json.load` returns it.

    Returns:
        A list of `level: path: message` lines, empty when nothing was found.
        A line beginning `error:` is something py2tosc refuses; a `warning:` is
        something it accepts and probably was not meant.
    """
    report = _Report()
    document = report.object(data, "the layout")
    if document is None:
        return report.found

    declared = document.get("format")
    if declared == TABLES["ui"]["dialect"]:
        _check_description(document, report)
    elif declared in (None, TABLES["layout"]["format"]):
        _check_layout(document, report)
    else:
        report.error(
            "the layout",
            f"{declared!r} is not a py2tosc layout; expected "
            f"{TABLES['ui']['dialect']!r} or {TABLES['layout']['format']!r}",
        )
    return report.found


def _parser() -> argparse.ArgumentParser:
    """The command line, which is the same shape `py2tosc validate` has."""
    parser = argparse.ArgumentParser(
        prog="check_json.py",
        description=(
            "Check a py2tosc JSON file -- a layout description or the faithful "
            "encoding -- without py2tosc installed."
        ),
        epilog=(
            "exit codes: 0 clean, 1 the file was read and has errors, "
            "2 the file could not be read. These are py2tosc's own, so this "
            "drops into the same scripts."
        ),
    )
    parser.add_argument(
        "files", metavar="FILE", nargs="+", help="the JSON files to check"
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="report only the findings, saying nothing about a clean file",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    """Check each file named on the command line.

    Args:
        argv: The arguments, or None to read them from `sys.argv`.

    Returns:
        0 when every file is clean, 1 when one was read and has errors, and 2
        when one could not be read at all -- which is also what argparse exits
        on a command line it cannot parse, so the two agree by construction.
    """
    args = _parser().parse_args(argv)

    worst = 0
    for path in args.files:
        try:
            with open(path, encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError) as exc:
            print(f"{path}: not a readable layout ({exc})", file=sys.stderr)
            worst = max(worst, 2)
            continue

        found = check(data)
        for line in found:
            print(f"{path}: {line}")
        if any(line.startswith(ERROR) for line in found):
            worst = max(worst, 1)
        elif not found and not args.quiet:
            print(f"{path}: clean")
    return worst


if __name__ == "__main__":
    raise SystemExit(main())
