CXX ?= g++
CXXFLAGS ?= -O3 -flto=auto -std=c++17 -Wall -Wextra
CPPFLAGS ?= -Isrc
LDFLAGS ?=
TEST_ENV ?=
ASAN_CXXFLAGS := -O1 -g -std=c++17 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_LDFLAGS := -fsanitize=address,undefined
ASAN_ENV := ASAN_OPTIONS=detect_leaks=0 LSAN_OPTIONS=detect_leaks=0
# Optional process wrapper for hosts that need one (e.g.
# `make ASAN_RUN='setarch x86_64 -R' asan`); ASAN_ENV is applied either way.
ASAN_RUN ?=

BUILD_DIR := build

# The ARIX-V library: compile these (with -Isrc) into a target application.
LIB_SRCS := src/arixv.cpp src/model.cpp src/tracker.cpp src/operand_cursor.cpp src/grammar_tables.cpp
LIB_HDRS := $(wildcard src/*.h)

CLI_SRCS := cli/main.cpp $(LIB_SRCS)
BENCH_SRCS := tools/bench/bench.cpp $(LIB_SRCS)
FUZZ_SRCS := tools/fuzz/fuzz_roundtrip.cpp $(LIB_SRCS)
DECODE_BENCH_SRCS := tools/bench/decode_bench.cpp $(LIB_SRCS)
RANGE_TEST_SRCS := tests/range_coder_test.cpp
ARIXV_TEST_SRCS := tests/arixv_test.cpp $(LIB_SRCS)
MODEL_TEST_SRCS := tests/model_test.cpp $(LIB_SRCS)
OPERAND_CURSOR_TEST_SRCS := tests/operand_cursor_test.cpp src/operand_cursor.cpp src/grammar_tables.cpp
GRAMMAR_TABLES_TEST_SRCS := tests/grammar_tables_test.cpp src/grammar_tables.cpp
FUZZ_CORPUS := $(wordlist 1,20,$(sort $(wildcard corpus/spv/*.spv)))
GRAMMAR_JSON_DIR := grammar-deps/spirv-headers/include/spirv/unified1

.PHONY: all test asan fuzz-smoke check-generated clean

all: $(BUILD_DIR)/arixv $(BUILD_DIR)/arixv_bench $(BUILD_DIR)/arixv_decode_bench $(BUILD_DIR)/fuzz_roundtrip

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/arixv: $(CLI_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CLI_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/arixv_bench: $(BENCH_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(BENCH_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/arixv_decode_bench: $(DECODE_BENCH_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DECODE_BENCH_SRCS) $(LDFLAGS) -o $@

# E-A0/E-A2 entropy-coder microbenchmarks (docs/development/performance.md).
# Not part of `all`: they need the -DARIXV_TRACE_DECISIONS diagnostics build.
TRACE_CXXFLAGS := $(CXXFLAGS) -DARIXV_TRACE_DECISIONS

$(BUILD_DIR)/ans_microbench: tools/bench/ans_microbench.cpp $(LIB_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(TRACE_CXXFLAGS) tools/bench/ans_microbench.cpp $(LIB_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/ans_symbol_microbench: tools/bench/ans_symbol_microbench.cpp $(LIB_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(TRACE_CXXFLAGS) tools/bench/ans_symbol_microbench.cpp $(LIB_SRCS) $(LDFLAGS) -o $@

# Per-decode memory breakdown; needs the -DARIXV_MEMSTATS diagnostics build.
$(BUILD_DIR)/memory_breakdown: tools/bench/memory_breakdown.cpp $(LIB_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DARIXV_MEMSTATS tools/bench/memory_breakdown.cpp $(LIB_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/fuzz_roundtrip: $(FUZZ_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(FUZZ_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/range_coder_test: $(RANGE_TEST_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(RANGE_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/arixv_test: $(ARIXV_TEST_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(ARIXV_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/model_test: $(MODEL_TEST_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODEL_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/operand_cursor_test: $(OPERAND_CURSOR_TEST_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(OPERAND_CURSOR_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/grammar_tables_test: $(GRAMMAR_TABLES_TEST_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GRAMMAR_TABLES_TEST_SRCS) $(LDFLAGS) -o $@

# Verifies src/grammar_tables.cpp is exactly what the generator produces.
# Needs python3 and the optional grammar source; skipped when either is missing
# so library-only checkouts can still run the tests.
check-generated:
	@if command -v python3 >/dev/null 2>&1 && [ -d $(GRAMMAR_JSON_DIR) ]; then \
		python3 scripts/gen_grammar_tables.py --grammar-dir $(GRAMMAR_JSON_DIR) --check; \
	else \
		echo "check-generated: SKIPPED (run scripts/fetch_pinned_repos.py --lock deps/grammar-sources.lock.json --dest grammar-deps)"; \
	fi

test: check-generated $(BUILD_DIR)/range_coder_test $(BUILD_DIR)/arixv_test $(BUILD_DIR)/model_test $(BUILD_DIR)/operand_cursor_test $(BUILD_DIR)/grammar_tables_test $(BUILD_DIR)/fuzz_roundtrip
	$(TEST_ENV) $(BUILD_DIR)/range_coder_test
	$(TEST_ENV) $(BUILD_DIR)/arixv_test
	$(TEST_ENV) $(BUILD_DIR)/model_test
	$(TEST_ENV) $(BUILD_DIR)/operand_cursor_test
	$(TEST_ENV) $(BUILD_DIR)/grammar_tables_test
	$(TEST_ENV) $(BUILD_DIR)/fuzz_roundtrip --iters=500 $(FUZZ_CORPUS)

$(BUILD_DIR)/range_coder_test_asan: $(RANGE_TEST_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ASAN_CXXFLAGS) $(RANGE_TEST_SRCS) $(ASAN_LDFLAGS) -o $@

$(BUILD_DIR)/arixv_test_asan: $(ARIXV_TEST_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ASAN_CXXFLAGS) $(ARIXV_TEST_SRCS) $(ASAN_LDFLAGS) -o $@

$(BUILD_DIR)/model_test_asan: $(MODEL_TEST_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ASAN_CXXFLAGS) $(MODEL_TEST_SRCS) $(ASAN_LDFLAGS) -o $@

$(BUILD_DIR)/operand_cursor_test_asan: $(OPERAND_CURSOR_TEST_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ASAN_CXXFLAGS) $(OPERAND_CURSOR_TEST_SRCS) $(ASAN_LDFLAGS) -o $@

$(BUILD_DIR)/grammar_tables_test_asan: $(GRAMMAR_TABLES_TEST_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ASAN_CXXFLAGS) $(GRAMMAR_TABLES_TEST_SRCS) $(ASAN_LDFLAGS) -o $@

asan: $(BUILD_DIR)/range_coder_test_asan $(BUILD_DIR)/arixv_test_asan $(BUILD_DIR)/model_test_asan $(BUILD_DIR)/operand_cursor_test_asan $(BUILD_DIR)/grammar_tables_test_asan
	$(ASAN_ENV) $(ASAN_RUN) $(BUILD_DIR)/range_coder_test_asan
	$(ASAN_ENV) $(ASAN_RUN) $(BUILD_DIR)/arixv_test_asan
	$(ASAN_ENV) $(ASAN_RUN) $(BUILD_DIR)/model_test_asan
	$(ASAN_ENV) $(ASAN_RUN) $(BUILD_DIR)/operand_cursor_test_asan
	$(ASAN_ENV) $(ASAN_RUN) $(BUILD_DIR)/grammar_tables_test_asan

# Coverage-guided libFuzzer harnesses (clang only), ASan+UBSan with UB fatal.
LF_CXX ?= clang++
LF_CXXFLAGS := -O1 -g -std=c++17 -fno-crash-diagnostics \
	-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined

$(BUILD_DIR)/fuzz_roundtrip_lf: tools/fuzz/fuzz_roundtrip_cov.cpp $(LIB_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(LF_CXX) $(CPPFLAGS) $(LF_CXXFLAGS) tools/fuzz/fuzz_roundtrip_cov.cpp $(LIB_SRCS) -o $@

$(BUILD_DIR)/fuzz_decode_lf: tools/fuzz/fuzz_decode_cov.cpp $(LIB_SRCS) $(LIB_HDRS) | $(BUILD_DIR)
	$(LF_CXX) $(CPPFLAGS) $(LF_CXXFLAGS) tools/fuzz/fuzz_decode_cov.cpp $(LIB_SRCS) -o $@

# A few minutes of coverage-guided fuzzing (docs/development/fuzzing.md).
# FUZZ_SMOKE_SECONDS (per harness, default 120) and FUZZ_SMOKE_JOBS (default
# nproc) tune it; ASAN_RUN is passed through as the process wrapper.
fuzz-smoke: $(BUILD_DIR)/fuzz_roundtrip_lf $(BUILD_DIR)/fuzz_decode_lf $(BUILD_DIR)/arixv
	BUILD_DIR=$(BUILD_DIR) RUN_WRAP='$(ASAN_RUN)' scripts/fuzz/smoke.sh

clean:
	rm -rf $(BUILD_DIR)
