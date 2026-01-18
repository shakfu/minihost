#!/bin/bash
#
# Download JUCE framework for minihost
#

set -e

JUCE_VERSION="${JUCE_VERSION:-8.0.6}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
JUCE_DIR="${JUCE_DIR:-$PROJECT_ROOT/JUCE}"

if [ -d "$JUCE_DIR" ] && [ -f "$JUCE_DIR/CMakeLists.txt" ]; then
    echo "JUCE already exists at $JUCE_DIR"
    exit 0
fi

echo "Downloading JUCE $JUCE_VERSION..."

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

ARCHIVE_URL="https://github.com/juce-framework/JUCE/archive/refs/tags/${JUCE_VERSION}.tar.gz"

curl -sL "$ARCHIVE_URL" -o "$TMPDIR/juce.tar.gz"

echo "Extracting..."
tar -xzf "$TMPDIR/juce.tar.gz" -C "$TMPDIR"

# Move to destination
mv "$TMPDIR/JUCE-${JUCE_VERSION}" "$JUCE_DIR"

echo "JUCE $JUCE_VERSION installed to $JUCE_DIR"
