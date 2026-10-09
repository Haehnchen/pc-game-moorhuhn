.DEFAULT_GOAL := build

CMAKE ?= cmake
CTEST ?= ctest
CTEST_ARGS ?=
BUILD_DIR ?= build/dev
BUILD_TYPE ?= Debug
GENERATOR ?= Ninja
CMAKE_ARGS ?=
RELEASE_BUILD_DIR ?= build/release
DIST_DIR ?= dist
PYTHON ?= python3
CLANG_FORMAT ?= clang-format
BUILD_JOBS ?= 4
TEST_JOBS ?= 2
SANITIZER_BUILD_DIR ?= build/sanitized
PACKAGE_ARGS ?=

.PHONY: configure build run test check check-sanitizers release format format-check help

configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" $(CMAKE_ARGS)

build: configure
	$(CMAKE) --build "$(BUILD_DIR)" --config "$(BUILD_TYPE)" --parallel $(BUILD_JOBS)

run: build
	$(CMAKE) --build "$(BUILD_DIR)" --config "$(BUILD_TYPE)" --target run

test check: build
	$(CTEST) --test-dir "$(BUILD_DIR)" --build-config "$(BUILD_TYPE)" --output-on-failure --parallel $(TEST_JOBS) --timeout 120 $(CTEST_ARGS)

check-sanitizers:
	$(MAKE) check BUILD_DIR="$(SANITIZER_BUILD_DIR)" BUILD_TYPE=Debug CMAKE_ARGS="$(CMAKE_ARGS) -DMOORHUHN_SANITIZERS=ON"

release:
	$(CMAKE) -S . -B "$(RELEASE_BUILD_DIR)" -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=Release -DMOORHUHN_DISTRIBUTION=ON -DBUILD_TESTING=OFF $(CMAKE_ARGS)
	$(CMAKE) --build "$(RELEASE_BUILD_DIR)" --config Release --target moorhuhn --parallel $(BUILD_JOBS)
	$(PYTHON) tools/packaging/package_release.py --build-dir "$(RELEASE_BUILD_DIR)" --output-dir "$(DIST_DIR)" $(PACKAGE_ARGS)

format:
	$(CMAKE) -DCLANG_FORMAT="$(CLANG_FORMAT)" -DSOURCE_DIR="$(CURDIR)" -DFIX=ON -P cmake/FormatCheck.cmake

format-check:
	$(CMAKE) -DCLANG_FORMAT="$(CLANG_FORMAT)" -DSOURCE_DIR="$(CURDIR)" -P cmake/FormatCheck.cmake

help:
	@printf '%s\n' 'make build    Configure and build incrementally (default target).' 'make run      Build, then launch the game.' 'make check    Build and run CTest (warnings are errors).' 'make check-sanitizers  Run CTest with ASan and UBSan.' 'make release  Build release ZIPs for this system in dist/.' 'make format  Apply clang-format to src/ and tests/.' 'make format-check  Fail on unformatted files in src/ and tests/.' 'Options: BUILD_DIR=build/dev BUILD_TYPE=Debug RELEASE_BUILD_DIR=build/release DIST_DIR=dist GENERATOR=Ninja CMAKE_ARGS= PACKAGE_ARGS= BUILD_JOBS=4 TEST_JOBS=2'
