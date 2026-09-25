"""OSC address pattern matching: JUCE semantics without JUCE's backtracking.

JUCE's matcher retried every split point for each '*', so k stars against an
n-character part cost about C(n+k, k): a 32-byte pattern with 12 stars held
an OscMapper for over 4 s per binding. minihost now matches with a set of
reachable positions, linear per pattern token.

The fixture holds 4381 (pattern, address) pairs with JUCE 8.0.12's answers,
recorded before the switch: edge cases, random strings, and patterns derived
from their target. It pins JUCE's quirks too -- empty parts dropped, '*'
reaching the end of a part only as the last token, "{}" as the empty string,
an empty "[]" consuming nothing.
"""

from __future__ import annotations

import gzip
import time
from pathlib import Path

import pytest

from minihost import osc_address_matches

FIXTURE = Path(__file__).parent / "_osc" / "juce_pattern_matches.tsv.gz"


def _cases():
    for line in gzip.decompress(FIXTURE.read_bytes()).decode().splitlines():
        if line and not line.startswith("#"):
            pattern, address, matched = line.split("\t")
            yield pattern, address, matched == "1"


def test_matches_juce_on_recorded_corpus():
    cases = list(_cases())
    assert len(cases) > 4000
    wrong = [(p, a, want) for p, a, want in cases if osc_address_matches(p, a) != want]
    assert not wrong, wrong[:10]


@pytest.mark.parametrize(
    "pattern, address, want",
    [
        ("/mh/param/*", "/mh/param/cutoff", True),
        ("/mh/*/cutoff", "/mh/param/cutoff", True),
        ("/mh/*", "/mh/param/cutoff", False),  # '*' does not cross '/'
        ("/a*{b,}", "/a", False),  # '*' reaches the end only as last token
        ("/*{,x}", "/x", True),
        ("/{}", "/", False),  # "/" has no parts
        ("/[a-c]x", "/bx", True),
        ("/[!a]x", "/ax", False),
        ("/[]x", "/x", True),  # an empty set consumes nothing
        ("/[-a]", "/a", False),  # a range needs a start
        ("/[a", "/a", False),  # unterminated
        ("//*", "//b//", True),  # empty parts dropped
        ("/b", "//b", False),  # no wildcard: JUCE compares the strings
    ],
)
def test_juce_edge_cases(pattern, address, want):
    assert osc_address_matches(pattern, address) == want


@pytest.mark.parametrize(
    "pattern, address",
    [
        ("/mh/param/" + "*" * 12 + "Q", "/mh/param/a_really_long_slug_name"),
        ("/" + "*a" * 30 + "Q", "/" + "a" * 200),
    ],
)
def test_many_wildcards_match_in_linear_time(pattern, address):
    t0 = time.perf_counter()
    for _ in range(100):
        assert not osc_address_matches(pattern, address)
    assert time.perf_counter() - t0 < 0.5
