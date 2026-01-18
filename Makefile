.PHONY: all build clean juce

all: build

juce:
	@./scripts/download_juce.sh

build: juce
	@mkdir -p build && cd build && cmake .. && cmake --build . --config Release

clean:
	@rm -rf build
