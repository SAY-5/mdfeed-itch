.PHONY: all configure build test bench asan ubsan fuzz fmt clean

BUILD_DIR ?= build
CMAKE_FLAGS ?=

all: build

configure:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_FLAGS)

build: configure
	cmake --build $(BUILD_DIR) -j

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

bench: build
	$(BUILD_DIR)/handler_bench

asan:
	cmake -S . -B build-asan -DMDFEED_ITCH_ASAN=ON -DMDFEED_ITCH_UBSAN=ON -DCMAKE_BUILD_TYPE=Debug
	cmake --build build-asan -j
	ctest --test-dir build-asan --output-on-failure

fuzz:
	cmake -S . -B build-fuzz -DMDFEED_ITCH_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
	cmake --build build-fuzz -j parser_fuzz

fmt:
	find src tests bench -name '*.cpp' -o -name '*.h' | xargs clang-format -i

clean:
	rm -rf build build-asan build-fuzz
