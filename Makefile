# Makefile for minihost
# Supports both C/C++ CLI tools and Python bindings

.PHONY: all juce cli sync build rebuild test wheel sdist clean distclean help \
		check publish-test publish lint lint-fix format format-check \
		typecheck qa docs docs-serve docs-deploy desktop run-desktop tsan \
		cli-debug cli-asan

# Default target - build Python bindings
all: build

# Download JUCE if needed (prefer Python for cross-platform compatibility)
juce:
	@python3 scripts/download_juce.py 2>/dev/null || python scripts/download_juce.py 2>/dev/null || ./scripts/download_juce.sh

# Build C/C++ CLI tools only. CMAKE_BUILD_TYPE has to be set at configure
# time: --config Release is read only by multi-config generators (Xcode,
# Visual Studio), so with the Unix Makefiles generator it is silently ignored
# and an unset build type means no optimization flags at all. Both are passed
# so the one that applies to the generator in use takes effect.
cli: juce
	@cmake -B build -DCMAKE_BUILD_TYPE=Release
	@cmake --build build --config Release

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

# ThreadSanitizer stress test for the lock-free SPSC ring buffers.
# Compiles the ring buffers + harness with -fsanitize=thread (no JUCE) and
# runs them. Override the workload with N=... (default 200000). macOS/Linux;
# needs a clang/gcc with TSan. See tests/tsan/README.md.
TSAN_CXX ?= $(CXX)
tsan:
	@mkdir -p build
	@$(TSAN_CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread \
		-Iprojects/libminihost_audio -Iprojects/libminihost \
		tests/tsan/ringbuffer_stress.cpp \
		projects/libminihost_audio/midi_ringbuffer.cpp \
		projects/libminihost_audio/param_ringbuffer.cpp \
		projects/libminihost_audio/transport_ringbuffer.cpp \
		projects/libminihost_audio/audio_ringbuffer.cpp \
		-o build/tsan_ringbuffer_stress
	@TSAN_OPTIONS="halt_on_error=1 $(TSAN_OPTIONS)" \
		TSAN_STRESS_N=$(or $(N),200000) ./build/tsan_ringbuffer_stress

# The two native CLI binaries built in configurations the Release build in CI
# does not exercise. Both use their own build dir so the Release build/ tree
# the wheel and the shipped binaries come from stays untouched, and both point
# the test suite at what they just built via MINIHOST_C_BIN / MINIHOST_CPP_BIN
# rather than relying on the newest-mtime search.
#
# CLI_TESTS is the subset that runs without a plugin installed -- which is the
# subset CI can actually run. Override it to widen the sweep locally:
#   make cli-asan MINIHOST_TEST_PLUGIN=/path/to/some.vst3
CLI_TESTS ?= tests/test_cli_conformance.py tests/test_release_version.py

# `uv run` would sync the project first, i.e. build the Python wheel -- pure
# waste in a job whose subject is the native binaries. CI overrides this with
# a plain interpreter that has only pytest installed:
#   make cli-asan PYTEST="python -m pytest"
# test_release_version.py skips its two package-dependent cases in that
# environment and still checks the binaries.
PYTEST ?= uv run pytest

# Debug build: assertions live, no optimizer. Catches the JUCE and libminihost
# assert()s that a Release build compiles out entirely.
cli-debug: juce
	@cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
	@cmake --build build-debug --config Debug --target minihost_c minihost_cpp
	@MINIHOST_C_BIN="$(CURDIR)/build-debug/projects/minihost_c/minihost_c" \
	 MINIHOST_CPP_BIN="$(CURDIR)/build-debug/projects/minihost_cpp/minihost_cpp" \
		$(PYTEST) -v $(CLI_TESTS)

# AddressSanitizer + UndefinedBehaviorSanitizer. RelWithDebInfo (not Debug)
# because ASan wants -O1 and frame pointers for usable stacks and tolerable
# runtimes. -fno-sanitize-recover makes a UB finding fail the run rather than
# print and continue, so CI cannot go green over one.
#
# Leak detection is off by default. JUCE keeps deliberately-immortal singletons
# (the format manager, the message-manager instance) alive to process exit, so
# LeakSanitizer -- which is Linux-only anyway, and a no-op on macOS -- reports
# them every run. Those are by-design, not bugs, and drowning the real ASan
# findings in them would make the job unreadable. Turn it on deliberately once
# there is a suppression file:  make cli-asan ASAN_DETECT_LEAKS=1
ASAN_DETECT_LEAKS ?= 0
ASAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer \
			 -fno-sanitize-recover=undefined -g

cli-asan: juce
	@cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DCMAKE_C_FLAGS="$(ASAN_FLAGS)" \
		-DCMAKE_CXX_FLAGS="$(ASAN_FLAGS)" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
	@cmake --build build-asan --config RelWithDebInfo \
		--target minihost_c minihost_cpp
	@ASAN_OPTIONS="detect_leaks=$(ASAN_DETECT_LEAKS) abort_on_error=1" \
	 UBSAN_OPTIONS="print_stacktrace=1 halt_on_error=1" \
	 MINIHOST_C_BIN="$(CURDIR)/build-asan/projects/minihost_c/minihost_c" \
	 MINIHOST_CPP_BIN="$(CURDIR)/build-asan/projects/minihost_cpp/minihost_cpp" \
		$(PYTEST) -v $(CLI_TESTS)

# Build the desktop GUI app (non-headless) into its own build dir so the
# headless library / CLI / Python wheel build in build/ stays untouched.
desktop: juce
	@cmake -B build-desktop -DCMAKE_BUILD_TYPE=Release \
		-DMINIHOST_BUILD_DESKTOP=ON
	@cmake --build build-desktop --config Release --target minihost_desktop

# Build (if needed) and launch the desktop app. The binary sits in a
# different place on each platform -- inside an .app bundle on macOS, bare
# on Linux, under Release/ on Windows -- so pick whichever exists rather
# than hardcoding one and leaving the target broken everywhere else.
DESKTOP_DIR := build-desktop/projects/minihost_desktop
run-desktop: desktop
	@bin="$(DESKTOP_DIR)/minihost_desktop.app/Contents/MacOS/minihost_desktop"; \
	[ -x "$$bin" ] || bin="$(DESKTOP_DIR)/minihost_desktop"; \
	[ -x "$$bin" ] || bin="$(DESKTOP_DIR)/Release/minihost_desktop.exe"; \
	if [ ! -x "$$bin" ]; then \
		echo "minihost_desktop not found under $(DESKTOP_DIR)" >&2; \
		exit 1; \
	fi; \
	exec "$$bin" $(ARGS)

# Quality gates. Checkers and fixers are deliberately separate targets: a
# mutating check can rewrite the tree and still exit 0, which hides drift and
# makes a local run non-reproducible against CI. `lint`, `format-check` and
# `typecheck` never write; `lint-fix` and `format` do.
#
# Scope: ruff *lint* covers the whole tree (src, tests, examples) and matches
# what CI runs. ruff *format* covers src/ and tests/ only -- examples/ is not
# format-clean and reformatting it is a separate, deliberate change.

# Lint (check only)
lint:
	@uv run ruff check .

# Lint with auto-fix (mutates)
lint-fix:
	@uv run ruff check --fix .

# Formatting check (no writes)
format-check:
	@uv run ruff format --check src/ tests/

# Apply formatting (mutates)
format:
	@uv run ruff format src/ tests/

# Run typecheck
typecheck:
	@uv run mypy src/

# Full non-mutating quality gate; mirrors the CI `qa` job.
qa: test lint format-check typecheck

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

# Build documentation (mkdocs)
# docs/changelog.md is a symlink to the root CHANGELOG.md (the single
# source of truth), so no copy step is needed.
docs:
	@uv run --group docs mkdocs build

# Serve documentation locally (with live reload)
docs-serve:
	@uv run --group docs mkdocs serve

# Deploy documentation to GitHub Pages
docs-deploy:
	@uv run --group docs mkdocs gh-deploy --force

# Clean build artifacts. build-*/ goes too: the desktop app configures its own
# tree (non-headless), and hand-made ones such as build-cli/ accumulate. Left
# behind they are not merely wasted disk -- tests/test_cli_conformance.py picks
# the most recently built binary it can find across all of them, so a stale
# tree can decide what gets tested.
clean:
	@rm -rf build/
	@rm -rf build-*/
	@rm -rf dist/
	@rm -rf site/
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
	@echo "  lint         - Run ruff linter (check only)"
	@echo "  lint-fix     - Run ruff linter with auto-fix (mutates)"
	@echo "  format-check - Check formatting (no writes)"
	@echo "  format       - Apply code formatting (mutates)"
	@echo "  typecheck    - Run mypy type checker"
	@echo "  qa           - Non-mutating gate: test, lint, format-check, typecheck"
	@echo "  tsan         - ThreadSanitizer ring-buffer stress test"
	@echo "  cli-debug    - Build native CLIs with assertions on, run CLI tests"
	@echo "  cli-asan     - Build native CLIs with ASan+UBSan, run CLI tests"
	@echo "  wheel        - Build wheel distribution"
	@echo "  sdist        - Build source distribution"
	@echo "  check        - Check distribution with twine"
	@echo "  publish-test - Publish to TestPyPI"
	@echo "  publish      - Publish to PyPI"
	@echo "  docs         - Build documentation (mkdocs)"
	@echo "  docs-serve   - Serve docs locally with live reload"
	@echo "  docs-deploy  - Deploy docs to GitHub Pages"
	@echo "  desktop      - Build the desktop GUI app (build-desktop/)"
	@echo "  run-desktop  - Build and launch the desktop app (macOS)"
	@echo "  clean        - Remove build artifacts"
	@echo "  distclean    - Remove all generated files"
	@echo "  help         - Show this help message"
