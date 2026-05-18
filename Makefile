.PHONY: all configure build test bench bench-regress bench-multicast cache-study pcaps asan ubsan fuzz fmt clean

BUILD_DIR ?= build
CMAKE_FLAGS ?=
BENCH_COUNT ?= 1000000
BENCH_BASELINE ?= bench/results/bench_ci_baseline.json
BENCH_DRIFT ?= 0.30

all: build

configure:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_FLAGS)

build: configure
	cmake --build $(BUILD_DIR) -j

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

bench: build
	$(BUILD_DIR)/handler_bench --count $(BENCH_COUNT)

# Regression gate: fails if throughput drifts more than BENCH_DRIFT below the
# committed baseline. Hermetic; no network, runs in CI.
bench-regress: build
	$(BUILD_DIR)/handler_bench --count $(BENCH_COUNT) \
		--regress $(BENCH_BASELINE) --drift $(BENCH_DRIFT)

bench-multicast: build
	$(BUILD_DIR)/multicast_bench --count $(BENCH_COUNT)

cache-study: build
	$(BUILD_DIR)/cache_study

# Regenerate the committed pcap test inputs. The output is byte-deterministic,
# so this should leave tests/pcap/ unchanged unless the generator changed.
pcaps: build
	$(BUILD_DIR)/pcap_gen tests/pcap

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
