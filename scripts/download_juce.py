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

    print(f"Downloading JUCE {JUCE_VERSION}...")

    # Create temp directory for download
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = Path(tmpdir)

        # Download archive (use tar.gz which works on all platforms with Python)
        archive_url = f"https://github.com/juce-framework/JUCE/archive/refs/tags/{JUCE_VERSION}.tar.gz"
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
        extracted_dir = tmpdir_path / f"JUCE-{JUCE_VERSION}"
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
