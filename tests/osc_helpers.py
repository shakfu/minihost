"""Shared helpers for the OSC tests."""

from __future__ import annotations

import minihost


def first_bound_param(mapper, plugin):
    """The lowest parameter index `bind_all` exposed by name, with its address.

    Not every parameter is bindable, so a test that assumes index 0 is
    testing one plugin's parameter list rather than the mapper: Renoise
    Redux's parameter 0 is a Preset selector that `bind_all` skips, leaving
    its lowest bound parameter at index 1.

    Returns (None, None) when the mapper bound nothing by name.
    """
    addresses = mapper.addresses
    for i in range(plugin.num_params):
        address = f"/mh/param/{minihost.slug(plugin.get_param_info(i)['name'])}"
        if address in addresses:
            return i, address
    return None, None
