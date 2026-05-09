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
JUCE_DIR = Path(os.environ.get("JUCE_DIR", PROJECT_ROOT / "JUCE"))


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
        return 0

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
    return 0


if __name__ == "__main__":
    sys.exit(main())
