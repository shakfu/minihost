# Makefile for minihost
# Supports both C/C++ CLI tools and Python bindings

.PHONY: all juce cli sync build rebuild test wheel sdist clean distclean help \
		check publish-test publish lint format typecheck qa

# Default target - build Python bindings
all: build

# Download JUCE if needed (prefer Python for cross-platform compatibility)
juce:
	@python3 scripts/download_juce.py 2>/dev/null || python scripts/download_juce.py 2>/dev/null || ./scripts/download_juce.sh

# Build C/C++ CLI tools only
cli: juce
	@mkdir -p build && cd build && cmake .. && cmake --build . --config Release

# Sync Python environment (initial setup)
sync:
	@uv sync --all-groups

# Build/rebuild Python extension
build: juce
	@uv sync --all-groups --reinstall-package minihost

# Alias for build
rebuild: build

# Run Python tests
test: build
	@uv run pytest tests/ -v

# Run lint
lint:
	@uv run ruff check --fix src/

# Run format
format:
	@uv run ruff format src/ tests/

# Run typecheck
typecheck:
	@uv run mypy src/

# Run qa
qa: test lint typecheck format

# Build wheel
wheel: juce
	uv build --wheel

# Build source distribution
sdist:
	uv build --sdist

# Check wheel
check:
	@echo "checking distribution with twine"
	@uv run twine check dist/*

# Publish test to testpypi
publish-test: check
	@echo "uploading to TestPyPI"
	@uv run twine upload --repository testpypi dist/*

# Publish to pypi
publish: check
	@echo "uploading to PyPI"
	@uv run twine upload dist/*

# Clean build artifacts
clean:
	@rm -rf build/
	@rm -rf dist/
	@rm -rf *.egg-info/
	@rm -rf src/*.egg-info/
	@rm -rf .pytest_cache/
	@find . -name "*.so" -delete 2>/dev/null || true
	@find . -name "*.pyd" -delete 2>/dev/null || true
	@find . -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null || true

# Clean everything including CMake cache
distclean: clean
	@rm -rf CMakeCache.txt CMakeFiles/

# Show help
help:
	@echo "Available targets:"
	@echo "  all          - Build Python extension (default)"
	@echo "  juce         - Download JUCE if needed"
	@echo "  cli          - Build C/C++ CLI tools only"
	@echo "  sync         - Sync Python environment (initial setup)"
	@echo "  build        - Build/rebuild Python extension"
	@echo "  rebuild      - Alias for build"
	@echo "  test         - Run Python tests"
	@echo "  lint         - Run ruff linter with auto-fix"
	@echo "  format       - Run code formatter"
	@echo "  typecheck    - Run mypy type checker"
	@echo "  qa           - Run test, lint, typecheck, and format"
	@echo "  wheel        - Build wheel distribution"
	@echo "  sdist        - Build source distribution"
	@echo "  check        - Check distribution with twine"
	@echo "  publish-test - Publish to TestPyPI"
	@echo "  publish      - Publish to PyPI"
	@echo "  clean        - Remove build artifacts"
	@echo "  distclean    - Remove all generated files"
	@echo "  help         - Show this help message"
