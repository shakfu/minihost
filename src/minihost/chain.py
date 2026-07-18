"""Declarative chain definitions.

Load a :class:`PluginChain` from a JSON or YAML spec, so rendering
pipelines can be checked into source control rather than expressed as
ad-hoc Python.

Schema::

    sample_rate: 48000              # optional, defaults to load_chain()'s arg
    block_size: 512                 # optional
    in_channels: 2                  # optional
    out_channels: 2                 # optional
    plugins:
      - path: /path/to/delay.vst3
        params:                     # optional, by name (case-insensitive)
          Mix: 0.5
          Feedback: 0.7
        preset: 3                   # optional, factory program index
        vstpreset: /path/file.vstpreset  # optional, mutually exclusive
        state: /path/state.bin           # optional, mutually exclusive
      - path: /path/to/reverb.vst3
        params:
          Decay: 0.8

YAML support is optional -- PyYAML is imported lazily and only required
if the file extension is ``.yaml`` / ``.yml``. JSON works with the
stdlib only.

The returned chain owns the underlying plugins (they are kept alive via
an attribute on the chain) so callers do not need to manage plugin
lifetimes separately.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from minihost._core import Plugin, PluginChain


class _OwningPluginChain(PluginChain):
    """PluginChain subclass that keeps its constituent plugins alive.

    The C++ chain holds raw pointers into the plugins; the only way to
    pin those Python objects to the chain's lifetime (and survive
    nanobind's lack of __dict__ on bound classes) is to subclass here.
    """

    __slots__ = ("_owned_plugins",)

    def __init__(self, plugins: list[Plugin]) -> None:
        super().__init__(plugins)
        self._owned_plugins = plugins


def _load_spec(path: Path) -> dict[str, Any]:
    suffix = path.suffix.lower()
    text = path.read_text()
    if suffix in (".yaml", ".yml"):
        try:
            # PyYAML is an optional dependency and ships no type stubs, so
            # mypy emits import-not-found when it is absent and import-untyped
            # when it is installed; silence both.
            import yaml  # type: ignore[import-not-found, import-untyped]
        except ImportError as e:
            raise ImportError(
                "Loading YAML chain specs requires PyYAML. Install it "
                "with 'pip install pyyaml', or use a JSON spec instead."
            ) from e
        data = yaml.safe_load(text)
    elif suffix == ".json":
        data = json.loads(text)
    else:
        raise ValueError(
            f"Unsupported chain spec extension '{suffix}'. Use .json, .yaml, or .yml."
        )
    if not isinstance(data, dict):
        raise ValueError("Chain spec must be a mapping at the top level.")
    return data


def _apply_plugin_entry(plugin: Plugin, entry: dict[str, Any]) -> None:
    # Mutually exclusive state sources: validate up front.
    sources = [k for k in ("preset", "vstpreset", "state") if entry.get(k) is not None]
    if len(sources) > 1:
        raise ValueError(
            f"Plugin entry for '{entry.get('path')}' specifies multiple "
            f"state sources ({sources}); use at most one of preset / "
            f"vstpreset / state."
        )

    preset = entry.get("preset")
    if preset is not None:
        n = plugin.num_programs
        if not isinstance(preset, int) or preset < 0 or preset >= n:
            raise ValueError(
                f"preset {preset!r} out of range (plugin has {n} program(s))."
            )
        plugin.program = preset

    vstpreset_path = entry.get("vstpreset")
    if vstpreset_path is not None:
        from minihost.vstpreset import load_vstpreset

        load_vstpreset(vstpreset_path, plugin)

    state_path = entry.get("state")
    if state_path is not None:
        with open(state_path, "rb") as f:
            plugin.set_state(f.read())

    params = entry.get("params") or {}
    if not isinstance(params, dict):
        raise ValueError(
            f"params for '{entry.get('path')}' must be a mapping of name -> value."
        )
    for name, value in params.items():
        plugin.set_param_by_name(str(name), float(value))


def load_chain(
    spec_path: str | Path,
    sample_rate: int | None = None,
    block_size: int | None = None,
) -> PluginChain:
    """Load a :class:`PluginChain` from a JSON or YAML spec file.

    Args:
        spec_path: Path to a ``.json``, ``.yaml``, or ``.yml`` chain spec.
        sample_rate: Sample rate for plugin construction. Overrides
            ``sample_rate`` in the spec; if neither is provided, defaults
            to 48000.
        block_size: Max block size for plugin construction. Overrides
            ``block_size`` in the spec; if neither is provided, defaults
            to 512.

    Returns:
        A :class:`PluginChain`. The chain holds references to the
        constructed plugins via ``chain._owned_plugins`` so callers
        only need to close the chain (and optionally the plugins).
    """
    path = Path(spec_path)
    if not path.exists():
        raise FileNotFoundError(f"Chain spec not found: {path}")

    spec = _load_spec(path)

    plugins_spec = spec.get("plugins")
    if not plugins_spec or not isinstance(plugins_spec, list):
        raise ValueError("Chain spec must contain a non-empty 'plugins' list.")

    sr = int(sample_rate if sample_rate is not None else spec.get("sample_rate", 48000))
    bs = int(block_size if block_size is not None else spec.get("block_size", 512))
    spec_in = spec.get("in_channels")
    spec_out = spec.get("out_channels")

    # Validate up front, before touching any plugin constructor, so
    # spec errors surface fast and don't depend on plugin discovery.
    for entry in plugins_spec:
        if not isinstance(entry, dict):
            raise ValueError("Each entry in 'plugins' must be a mapping.")
        if not entry.get("path"):
            raise ValueError("Each plugin entry must specify a 'path'.")
        sources = [
            k for k in ("preset", "vstpreset", "state") if entry.get(k) is not None
        ]
        if len(sources) > 1:
            raise ValueError(
                f"Plugin entry for '{entry.get('path')}' specifies "
                f"multiple state sources ({sources}); use at most one of "
                f"preset / vstpreset / state."
            )

    plugins: list[Plugin] = []
    try:
        for entry in plugins_spec:
            plugin_path = entry["path"]
            kwargs: dict[str, Any] = {
                "sample_rate": sr,
                "max_block_size": bs,
            }
            in_ch = entry.get("in_channels", spec_in)
            out_ch = entry.get("out_channels", spec_out)
            if in_ch is not None:
                kwargs["in_channels"] = int(in_ch)
            if out_ch is not None:
                kwargs["out_channels"] = int(out_ch)

            plugin = Plugin(plugin_path, **kwargs)
            plugins.append(plugin)
            _apply_plugin_entry(plugin, entry)
    except Exception:
        # Clean up any plugins constructed before the failure.
        for p in plugins:
            try:
                p.close()
            except Exception:
                pass
        raise

    return _OwningPluginChain(plugins)
