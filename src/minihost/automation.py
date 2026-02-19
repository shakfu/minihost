"""Automation parsing and interpolation for minihost.

Supports Plugalyzer-compatible JSON automation files and CLI parameter parsing.

JSON automation format:
    {
        "Param Name": 0.5,                          // static normalized value
        "Param Name": "TextValue",                   // static text value
        "Param Name": {"0": 0.5, "1.5s": 0.7, "50%": 1.0}  // keyframes
    }

Keyframe time formats:
    "1000"   - sample offset
    "1.5s"   - seconds
    "50%"    - percentage of total length
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from minihost._core import Plugin


def find_param_by_name(plugin: Plugin, name: str) -> int:
    """Find a parameter index by name (case-insensitive).

    Args:
        plugin: Plugin instance.
        name: Parameter name to search for.

    Returns:
        Parameter index.

    Raises:
        ValueError: If no parameter with that name exists.
    """
    name_lower = name.lower()
    for i in range(plugin.num_params):
        info = plugin.get_param_info(i)
        if info["name"].lower() == name_lower:
            return i
    raise ValueError(
        f"Parameter not found: '{name}'. "
        f"Use 'minihost params <plugin>' to list available parameters."
    )


def parse_param_arg(arg_str: str, plugin: Plugin) -> tuple[int, float]:
    """Parse a CLI --param argument string.

    Formats:
        "Name:value"     - set by name, normalized value (0-1)
        "Name:value:n"   - explicit normalized flag
        "Name:TextValue" - set by name, text value (parsed via plugin)

    Args:
        arg_str: Parameter argument string.
        plugin: Plugin instance for name lookup and text parsing.

    Returns:
        Tuple of (param_index, normalized_value).

    Raises:
        ValueError: If format is invalid or parameter not found.
    """
    parts = arg_str.split(":")
    if len(parts) < 2:
        raise ValueError(
            f"Invalid parameter format: '{arg_str}'. "
            f"Expected 'Name:value' or 'Name:value:n'."
        )

    name = parts[0].strip()
    value_str = parts[1].strip()
    is_normalized = len(parts) >= 3 and parts[2].strip().lower() == "n"

    param_idx = find_param_by_name(plugin, name)

    # Try parsing as a number first
    try:
        value = float(value_str)
        if is_normalized:
            return param_idx, value
        # If no :n suffix and value is in 0-1 range, treat as normalized
        # Otherwise treat as raw value (but we still return normalized for now)
        return param_idx, value
    except ValueError:
        pass

    # Try parsing as text value via plugin
    try:
        value = plugin.param_from_text(param_idx, value_str)
        return param_idx, value
    except (RuntimeError, ValueError) as e:
        raise ValueError(
            f"Could not parse value '{value_str}' for parameter '{name}': {e}"
        ) from e


def _parse_time_key(key: str, sample_rate: int, total_length_samples: int) -> int:
    """Parse a keyframe time key to a sample offset.

    Formats:
        "1000"   - sample offset
        "1.5s"   - seconds
        "50%"    - percentage of total length

    Returns:
        Sample offset (int).
    """
    key = key.strip()

    if key.endswith("%"):
        pct = float(key[:-1])
        return int(total_length_samples * pct / 100.0)

    if key.endswith("s"):
        seconds = float(key[:-1])
        return int(seconds * sample_rate)

    # Plain number = sample offset
    return int(float(key))


def _interpolate_keyframes(
    keyframes: list[tuple[int, float]],
    total_length_samples: int,
    block_size: int,
) -> list[tuple[int, float]]:
    """Expand keyframes into per-block-boundary interpolated values.

    Uses linear interpolation between keyframes. Values before the first
    keyframe use the first keyframe's value; values after the last use
    the last keyframe's value.

    Args:
        keyframes: Sorted list of (sample_offset, value) tuples.
        total_length_samples: Total length of the audio in samples.
        block_size: Processing block size for granularity.

    Returns:
        List of (sample_offset, value) tuples at block boundaries.
    """
    if not keyframes:
        return []

    if len(keyframes) == 1:
        # Single keyframe = static value at that point
        return [keyframes[0]]

    result = []

    for i in range(len(keyframes) - 1):
        s0, v0 = keyframes[i]
        s1, v1 = keyframes[i + 1]

        # Add the start keyframe
        result.append((s0, v0))

        # Interpolate at block boundaries between keyframes
        if s1 > s0:
            # Find block boundaries in this segment
            first_block = ((s0 // block_size) + 1) * block_size
            for sample in range(first_block, s1, block_size):
                t = (sample - s0) / (s1 - s0)
                v = v0 + t * (v1 - v0)
                result.append((sample, v))

    # Add the last keyframe
    result.append(keyframes[-1])

    return result


def parse_automation_file(
    path: str | Path,
    plugin: Plugin,
    sample_rate: int,
    total_length_samples: int,
    block_size: int = 512,
) -> list[tuple[int, int, float]]:
    """Parse a JSON automation file into parameter change events.

    Args:
        path: Path to JSON automation file.
        plugin: Plugin instance for parameter name lookup and text parsing.
        sample_rate: Sample rate in Hz.
        total_length_samples: Total length of the audio in samples.
        block_size: Processing block size for interpolation granularity.

    Returns:
        Sorted list of (sample_offset, param_index, value) tuples,
        compatible with Plugin.process_auto().

    Raises:
        FileNotFoundError: If the file does not exist.
        ValueError: If the JSON format is invalid.
    """
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(f"Automation file not found: {path}")

    with open(path) as f:
        data = json.load(f)

    if not isinstance(data, dict):
        raise ValueError("Automation file must contain a JSON object at the top level.")

    changes: list[tuple[int, int, float]] = []

    for param_name, spec in data.items():
        param_idx = find_param_by_name(plugin, param_name)

        if isinstance(spec, (int, float)):
            # Static normalized value
            changes.append((0, param_idx, float(spec)))

        elif isinstance(spec, str):
            # Static text value - parse via plugin
            try:
                value = plugin.param_from_text(param_idx, spec)
            except (RuntimeError, ValueError) as e:
                raise ValueError(
                    f"Could not parse text value '{spec}' "
                    f"for parameter '{param_name}': {e}"
                ) from e
            changes.append((0, param_idx, value))

        elif isinstance(spec, dict):
            # Keyframes: {"time": value, ...}
            keyframes: list[tuple[int, float]] = []
            for time_key, value in spec.items():
                sample_offset = _parse_time_key(
                    time_key, sample_rate, total_length_samples
                )
                if isinstance(value, str):
                    try:
                        value = plugin.param_from_text(param_idx, value)
                    except (RuntimeError, ValueError) as e:
                        raise ValueError(
                            f"Could not parse text value '{value}' at time '{time_key}' "
                            f"for parameter '{param_name}': {e}"
                        ) from e
                keyframes.append((sample_offset, float(value)))

            # Sort by sample offset
            keyframes.sort(key=lambda x: x[0])

            # Interpolate
            expanded = _interpolate_keyframes(
                keyframes, total_length_samples, block_size
            )
            for sample_offset, value in expanded:
                changes.append((sample_offset, param_idx, value))

        else:
            raise ValueError(
                f"Invalid automation spec for '{param_name}': "
                f"expected number, string, or object, got {type(spec).__name__}"
            )

    # Sort by sample offset for process_auto()
    changes.sort(key=lambda x: x[0])
    return changes
