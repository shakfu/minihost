#!/usr/bin/env python3
"""
Download JUCE framework for minihost.

Cross-platform script that works on Windows, macOS, and Linux.
"""

import os
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

JUCE_VERSION = os.environ.get("JUCE_VERSION", "8.0.12")
# Pinned commit SHA for the default JUCE_VERSION above. GitHub tags are
# mutable on the server side (a tag can be force-pushed), so we resolve to
# a content-addressed commit SHA for reproducibility. Update both this and
# JUCE_VERSION together when bumping JUCE.
#
# To find the SHA for a new tag:
#   curl -s https://api.github.com/repos/juce-framework/JUCE/git/refs/tags/X.Y.Z \
#     | python3 -c "import json,sys; print(json.load(sys.stdin)['object']['sha'])"
JUCE_PINNED_SHA = os.environ.get(
    "JUCE_SHA",
    "29396c22c93392d6738e021b83196283d6e4d850",  # corresponds to 8.0.12
)
# Set JUCE_ALLOW_TAG=1 to bypass SHA pinning (downloads by tag name). Use only
# for ad-hoc bumps when JUCE_VERSION has been overridden but the corresponding
# SHA is not yet known. Default off so CI gets reproducible builds.
JUCE_ALLOW_TAG = os.environ.get("JUCE_ALLOW_TAG", "").strip() in ("1", "true", "yes")
SCRIPT_DIR = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent
JUCE_DIR = Path(os.environ.get("JUCE_DIR", PROJECT_ROOT / "thirdparty" / "JUCE"))

# minihost patches applied to the downloaded tree.
#
# JUCE's macOS message queue registers its run-loop source for
# kCFRunLoopCommonModes only, so the only way to deliver a message is to run a
# common mode -- which also runs every other main-loop client in the process,
# including the main dispatch queue. A headless host runs that loop from
# Plugin.poll_callbacks(), so it would be running arbitrary foreign work inside
# a library call. The patch adds the source to a private mode as well, holding
# nothing else, and projects/libminihost/minihost_pump_mac.cpp runs only that.
#
# MINIHOST_PUMP_MODE marks a patched file, which makes this idempotent. An
# anchor that no longer matches is a hard error: that is how a JUCE bump which
# moves the code gets noticed rather than silently dropping the patch.
PUMP_MODE = "net.minihost.pump"
PATCH_MARKER = "MINIHOST_PUMP_MODE"

JUCE_PATCHES = {
    "modules/juce_events/native/juce_MessageQueue_mac.h": [
        (
            "        CFRunLoopAddSource (runLoop, runLoopSource.get(), kCFRunLoopCommonModes);\n",
            "        CFRunLoopAddSource (runLoop, runLoopSource.get(), kCFRunLoopCommonModes);\n"
            "        // MINIHOST_PUMP_MODE: patched in by scripts/download_juce.py. A private\n"
            "        // mode this source is the only member of, so a headless host can deliver\n"
            "        // these without running the rest of the main loop. See\n"
            "        // projects/libminihost/minihost_pump_mac.cpp.\n"
            f'        CFRunLoopAddSource (runLoop, runLoopSource.get(), CFSTR ("{PUMP_MODE}"));\n',
        ),
        (
            "        CFRunLoopRemoveSource (runLoop, runLoopSource.get(), kCFRunLoopCommonModes);\n",
            "        CFRunLoopRemoveSource (runLoop, runLoopSource.get(), kCFRunLoopCommonModes);\n"
            f'        CFRunLoopRemoveSource (runLoop, runLoopSource.get(), CFSTR ("{PUMP_MODE}"));  // MINIHOST_PUMP_MODE\n',
        ),
    ],
}


def apply_patches(juce_dir: Path) -> int:
    """Apply the minihost patches above. Idempotent; returns 0 on success."""
    for rel, edits in JUCE_PATCHES.items():
        path = juce_dir / rel
        if not path.exists():
            print(f"Error: cannot patch {path}: no such file", file=sys.stderr)
            return 1

        src = path.read_text(encoding="utf-8")
        if PATCH_MARKER in src:
            continue

        for anchor, replacement in edits:
            found = src.count(anchor)
            if found != 1:
                print(
                    f"Error: patch anchor in {rel} matched {found} times, expected 1. "
                    "JUCE changed -- update JUCE_PATCHES in this script.",
                    file=sys.stderr,
                )
                return 1
            src = src.replace(anchor, replacement)

        path.write_text(src, encoding="utf-8")
        print(f"Patched {rel}")
    return 0


def download_file(url: str, dest: Path) -> None:
    """Download a file from URL to destination."""
    print(f"Downloading from {url}...")
    urllib.request.urlretrieve(url, dest)


def extract_archive(archive_path: Path, dest_dir: Path) -> None:
    """Extract a tar.gz or zip archive."""
    print("Extracting...")
    if archive_path.suffix == ".zip" or archive_path.name.endswith(".zip"):
        with zipfile.ZipFile(archive_path, "r") as zf:
            zf.extractall(dest_dir)
    else:
        # Assume tar.gz
        with tarfile.open(archive_path, "r:gz") as tf:
            # Use filter="data" for Python 3.12+ to avoid deprecation warning
            # and ensure safe extraction (no absolute paths, no parent traversal)
            if hasattr(tarfile, "data_filter"):
                tf.extractall(dest_dir, filter="data")
            else:
                tf.extractall(dest_dir)


def main() -> int:
    # Check if JUCE already exists
    if JUCE_DIR.exists() and (JUCE_DIR / "CMakeLists.txt").exists():
        print(f"JUCE already exists at {JUCE_DIR}")
        return apply_patches(JUCE_DIR)

    if JUCE_ALLOW_TAG:
        print(f"Downloading JUCE {JUCE_VERSION} (by tag, NOT SHA-pinned)...")
        archive_ref = JUCE_VERSION
        extracted_name = f"JUCE-{JUCE_VERSION}"
        archive_url = (
            f"https://github.com/juce-framework/JUCE/archive/refs/tags/{archive_ref}.tar.gz"
        )
    else:
        print(f"Downloading JUCE {JUCE_VERSION} (SHA {JUCE_PINNED_SHA[:12]})...")
        archive_ref = JUCE_PINNED_SHA
        # GitHub names commit-archive directories <repo>-<full sha>.
        extracted_name = f"JUCE-{JUCE_PINNED_SHA}"
        archive_url = (
            f"https://github.com/juce-framework/JUCE/archive/{archive_ref}.tar.gz"
        )

    # Create temp directory for download
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = Path(tmpdir)

        # Download archive (use tar.gz which works on all platforms with Python)
        archive_path = tmpdir_path / "juce.tar.gz"

        try:
            download_file(archive_url, archive_path)
        except Exception as e:
            print(f"Error downloading JUCE: {e}", file=sys.stderr)
            return 1

        try:
            extract_archive(archive_path, tmpdir_path)
        except Exception as e:
            print(f"Error extracting JUCE: {e}", file=sys.stderr)
            return 1

        # Move to destination
        extracted_dir = tmpdir_path / extracted_name
        if not extracted_dir.exists():
            print(f"Error: Expected directory {extracted_dir} not found", file=sys.stderr)
            return 1

        # Ensure parent directory exists
        JUCE_DIR.parent.mkdir(parents=True, exist_ok=True)

        # Move to final location
        shutil.move(str(extracted_dir), str(JUCE_DIR))

    print(f"JUCE {JUCE_VERSION} installed to {JUCE_DIR}")
    return apply_patches(JUCE_DIR)


if __name__ == "__main__":
    sys.exit(main())
