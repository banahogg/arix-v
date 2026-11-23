cmake_minimum_required(VERSION 3.21)

project(arixv
  VERSION 1.1.0
  DESCRIPTION "Standalone, purely adaptive SPIR-V compressor"
  LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(ARIXV_BUILD_ENCODER "Build the ARIX-V encoder target" ${PROJECT_IS_TOP_LEVEL})
option(ARIXV_BUILD_CLI "Build the arixv CLI binary" ${PROJECT_IS_TOP_LEVEL})
option(ARIXV_BUILD_TESTS "Build the ARIX-V test suite" ${PROJECT_IS_TOP_LEVEL})
option(ARIXV_BUILD_TOOLS "Build benchmark and fuzzing tools" ${PROJECT_IS_TOP_LEVEL})
option(ARIXV_BUILD_DIAGNOSTICS "Build diagnostic microbenchmarks (trace/memstats)" OFF)
option(ARIXV_BUILD_LIBFUZZER "Build Clang libFuzzer harnesses" OFF)
option(ARIXV_INSTALL "Generate install target" ${PROJECT_IS_TOP_LEVEL})
option(ARIXV_BUILD_PACK
  "Build the ARXP shader-pack API (arixv_pack_*); OFF keeps a 1.0-sized decoder" ON)
option(ARIXV_BUILD_RESEARCH_PACK
  "Build research-only multi-module pack probe targets (not ARX2; Track B)" OFF)

if(PROJECT_IS_TOP_LEVEL AND WIN32 AND NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

set(ARIXV_DECODER_SOURCES
  src/arixv.cpp
  src/model.cpp
  src/tracker.cpp
  src/operand_cursor.cpp
  src/grammar_tables.cpp
)

set(ARIXV_ENCODER_SOURCES
  src/arixv_encode.cpp
)

# Shader packs: on by default. When off, the pack sources and codec are left
# out and ARIXV_PACK=0 is propagated publicly, which hides the arixv_pack_*
# declarations in the public header.
if(ARIXV_BUILD_PACK)
  list(APPEND ARIXV_DECODER_SOURCES src/arixv_pack.cpp)
  list(APPEND ARIXV_ENCODER_SOURCES src/arixv_pack_encode.cpp)
  set(ARIXV_PACK_DEFINITIONS)
else()
  set(ARIXV_PACK_DEFINITIONS ARIXV_PACK=0)
  if(ARIXV_BUILD_RESEARCH_PACK)
    message(FATAL_ERROR "ARIXV_BUILD_RESEARCH_PACK requires ARIXV_BUILD_PACK=ON")
  endif()
endif()

function(arixv_apply_compile_options target)
  target_compile_features(${target} PRIVATE cxx_std_17)
  set_target_properties(${target} PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W3
      $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/Gy>
    )
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -ffunction-sections
      -fdata-sections
    )
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Public library targets: arixv::decoder (always) and arixv::encoder (optional)
# ---------------------------------------------------------------------------
set(ARIXV_INSTALL_TARGETS)

if(BUILD_SHARED_LIBS)
  # Single shared library exporting only ARIXV_API C symbols so consumers
  # linking both arixv::decoder and arixv::encoder into one executable share
  # one DSO/DLL with zero duplicate exports and zero C++ ABI exposure.
  set(_shared_sources ${ARIXV_DECODER_SOURCES})
  if(ARIXV_BUILD_ENCODER)
    list(APPEND _shared_sources ${ARIXV_ENCODER_SOURCES})
  endif()

  add_library(arixv_shared SHARED ${_shared_sources})
  arixv_apply_compile_options(arixv_shared)
  target_include_directories(arixv_shared
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(arixv_shared
    PUBLIC
      ARIXV_SHARED
      ${ARIXV_PACK_DEFINITIONS}
    PRIVATE
      ARIXV_BUILDING
      ARIXV_ENCODER=$<BOOL:${ARIXV_BUILD_ENCODER}>
  )
  if(ARIXV_BUILD_ENCODER)
    set(_shared_output_name arixv)
  else()
    set(_shared_output_name arixv_decoder)
  endif()
  set_target_properties(arixv_shared PROPERTIES
    OUTPUT_NAME ${_shared_output_name}
    EXPORT_NAME shared
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
  )
  if(UNIX AND NOT APPLE)
    set(_arixv_version_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/arixv.map")
    target_link_options(arixv_shared PRIVATE
      "-Wl,--version-script=${_arixv_version_script}"
      "-Wl,--gc-sections"
    )
    set_property(TARGET arixv_shared APPEND PROPERTY
      LINK_DEPENDS "${_arixv_version_script}"
    )
  elseif(APPLE)
    target_link_options(arixv_shared PRIVATE "-Wl,-dead_strip")
  endif()

  add_library(arixv_decoder INTERFACE)
  target_link_libraries(arixv_decoder INTERFACE arixv_shared)
  set_target_properties(arixv_decoder PROPERTIES EXPORT_NAME decoder)
  add_library(arixv::decoder ALIAS arixv_decoder)

  list(APPEND ARIXV_INSTALL_TARGETS arixv_shared arixv_decoder)

  if(ARIXV_BUILD_ENCODER)
    add_library(arixv_encoder INTERFACE)
    target_link_libraries(arixv_encoder INTERFACE arixv_shared)
    target_compile_definitions(arixv_encoder INTERFACE ARIXV_ENCODER=1)
    set_target_properties(arixv_encoder PROPERTIES EXPORT_NAME encoder)
    add_library(arixv::encoder ALIAS arixv_encoder)
    list(APPEND ARIXV_INSTALL_TARGETS arixv_encoder)
  endif()
else()
  add_library(arixv_decoder STATIC ${ARIXV_DECODER_SOURCES})
  arixv_apply_compile_options(arixv_decoder)
  target_include_directories(arixv_decoder
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(arixv_decoder
    PUBLIC
      ${ARIXV_PACK_DEFINITIONS}
    PRIVATE
      ARIXV_BUILDING
      ARIXV_ENCODER=$<BOOL:${ARIXV_BUILD_ENCODER}>
  )
  # When a pure-C parent consumes static ARIX-V via FetchContent/add_subdirectory
  # without enabling CXX in its own directory scope, CMake links the parent
  # executable with the C linker driver. Propagating only the extra C++ runtime
  # libraries/directories for C-linked build-interface consumers resolves C++
  # runtime symbols cleanly without affecting C++ consumers or installed exports.
  set(_arixv_cxx_libs ${CMAKE_CXX_IMPLICIT_LINK_LIBRARIES})
  if(CMAKE_C_IMPLICIT_LINK_LIBRARIES)
    list(REMOVE_ITEM _arixv_cxx_libs ${CMAKE_C_IMPLICIT_LINK_LIBRARIES})
  endif()
  if(_arixv_cxx_libs)
    target_link_libraries(arixv_decoder
      PUBLIC
        $<BUILD_INTERFACE:$<$<LINK_LANGUAGE:C>:${_arixv_cxx_libs}>>
    )
  endif()
  set(_arixv_cxx_dirs ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES})
  if(CMAKE_C_IMPLICIT_LINK_DIRECTORIES)
    list(REMOVE_ITEM _arixv_cxx_dirs ${CMAKE_C_IMPLICIT_LINK_DIRECTORIES})
  endif()
  if(_arixv_cxx_dirs)
    target_link_directories(arixv_decoder
      PUBLIC
        $<BUILD_INTERFACE:$<$<LINK_LANGUAGE:C>:${_arixv_cxx_dirs}>>
    )
  endif()
  set_target_properties(arixv_decoder PROPERTIES
    EXPORT_NAME decoder
    POSITION_INDEPENDENT_CODE ON
  )
  add_library(arixv::decoder ALIAS arixv_decoder)
  list(APPEND ARIXV_INSTALL_TARGETS arixv_decoder)

  if(ARIXV_BUILD_ENCODER)
    add_library(arixv_encoder STATIC ${ARIXV_ENCODER_SOURCES})
    arixv_apply_compile_options(arixv_encoder)
    target_include_directories(arixv_encoder
      PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(arixv_encoder
      PUBLIC
        ARIXV_ENCODER=1
      PRIVATE
        ARIXV_BUILDING
    )
    target_link_libraries(arixv_encoder PUBLIC arixv_decoder)
    set_target_properties(arixv_encoder PROPERTIES
      EXPORT_NAME encoder
      POSITION_INDEPENDENT_CODE ON
    )
    add_library(arixv::encoder ALIAS arixv_encoder)
    list(APPEND ARIXV_INSTALL_TARGETS arixv_encoder)
  endif()
endif()

# Helper to build isolated private static libraries for white-box tests, CLI
# internal flags (--force-coded), and diagnostic tools (zero ODR risk).
function(arixv_add_internal_lib target)
  add_library(${target} STATIC ${ARIXV_DECODER_SOURCES} ${ARIXV_ENCODER_SOURCES})
  arixv_apply_compile_options(${target})
  target_include_directories(${target}
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(${target}
    PUBLIC
      ARIXV_ENCODER=1
      ${ARIXV_PACK_DEFINITIONS}
      ${ARGN}
    PRIVATE
      ARIXV_BUILDING
  )
endfunction()

if(ARIXV_BUILD_ENCODER AND (ARIXV_BUILD_CLI OR ARIXV_BUILD_TOOLS OR ARIXV_BUILD_TESTS))
  arixv_add_internal_lib(arixv_internal)
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
if(ARIXV_BUILD_CLI AND ARIXV_BUILD_ENCODER)
  add_executable(arixv_cli cli/main.cpp)
  arixv_apply_compile_options(arixv_cli)
  target_link_libraries(arixv_cli PRIVATE arixv_internal)
  set_target_properties(arixv_cli PROPERTIES OUTPUT_NAME arixv)
endif()

# ---------------------------------------------------------------------------
# Tools (benchmarks & fuzzers)
# ---------------------------------------------------------------------------
if(ARIXV_BUILD_TOOLS AND ARIXV_BUILD_ENCODER)
  arixv_add_internal_lib(arixv_internal_stats ARIXV_ENABLE_STATS)
  add_executable(arixv_bench tools/bench/bench.cpp)
  arixv_apply_compile_options(arixv_bench)
  target_link_libraries(arixv_bench PRIVATE arixv_internal_stats)

  if(NOT TARGET arixv_internal_raw_residual)
    arixv_add_internal_lib(arixv_internal_raw_residual ARIXV_ENABLE_RAW_RESIDUAL)
  endif()
  add_executable(arixv_decode_bench tools/bench/decode_bench.cpp)
  arixv_apply_compile_options(arixv_decode_bench)
  target_link_libraries(arixv_decode_bench PRIVATE arixv_internal_raw_residual)

  if(ARIXV_BUILD_PACK)
    find_package(Threads REQUIRED)
    add_executable(arixv_pack_api_bench tools/bench/pack_api_bench.cpp)
    arixv_apply_compile_options(arixv_pack_api_bench)
    target_link_libraries(arixv_pack_api_bench PRIVATE arixv_internal Threads::Threads)
  endif()

  add_executable(fuzz_roundtrip tools/fuzz/fuzz_roundtrip.cpp)
  arixv_apply_compile_options(fuzz_roundtrip)
  target_link_libraries(fuzz_roundtrip PRIVATE arixv_internal)
endif()

if(ARIXV_BUILD_DIAGNOSTICS AND ARIXV_BUILD_ENCODER)
  arixv_add_internal_lib(arixv_internal_trace ARIXV_TRACE_DECISIONS)
  add_executable(ans_microbench tools/bench/ans_microbench.cpp)
  arixv_apply_compile_options(ans_microbench)
  target_link_libraries(ans_microbench PRIVATE arixv_internal_trace)

  add_executable(ans_symbol_microbench tools/bench/ans_symbol_microbench.cpp)
  arixv_apply_compile_options(ans_symbol_microbench)
  target_link_libraries(ans_symbol_microbench PRIVATE arixv_internal_trace)

  arixv_add_internal_lib(arixv_internal_memstats ARIXV_MEMSTATS)
  add_executable(memory_breakdown tools/bench/memory_breakdown.cpp)
  arixv_apply_compile_options(memory_breakdown)
  target_link_libraries(memory_breakdown PRIVATE arixv_internal_memstats)
endif()

if(ARIXV_BUILD_LIBFUZZER AND ARIXV_BUILD_ENCODER)
  set(_lf_flags -O1 -g -fno-crash-diagnostics -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)
  add_library(arixv_internal_lf STATIC ${ARIXV_DECODER_SOURCES} ${ARIXV_ENCODER_SOURCES})
  arixv_apply_compile_options(arixv_internal_lf)
  target_include_directories(arixv_internal_lf
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  # The fuzz library computes but does not enforce the ARXP header checksum,
  # so mutated pack headers reach the structural validation behind it.
  target_compile_definitions(arixv_internal_lf
    PUBLIC
      ARIXV_ENCODER=1
      ${ARIXV_PACK_DEFINITIONS}
    PRIVATE
      ARIXV_BUILDING
      ARIXV_FUZZ_NO_PACK_CHECKSUM
  )
  target_compile_options(arixv_internal_lf PRIVATE ${_lf_flags})

  foreach(_lf_target fuzz_roundtrip_lf fuzz_decode_lf)
    if(_lf_target STREQUAL "fuzz_roundtrip_lf")
      set(_lf_src tools/fuzz/fuzz_roundtrip_cov.cpp)
    else()
      set(_lf_src tools/fuzz/fuzz_decode_cov.cpp)
    endif()
    add_executable(${_lf_target} ${_lf_src})
    arixv_apply_compile_options(${_lf_target})
    target_compile_options(${_lf_target} PRIVATE ${_lf_flags})
    target_link_options(${_lf_target} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(${_lf_target} PRIVATE arixv_internal_lf)
  endforeach()
  set(_lf_pack_modes)
  set(_lf_pack_targets)
  if(ARIXV_BUILD_PACK)
    set(_lf_pack_modes RT DEC)
    set(_lf_pack_targets fuzz_pack_rt_lf fuzz_pack_dec_lf)
  endif()
  foreach(_lf_mode ${_lf_pack_modes})
    string(TOLOWER ${_lf_mode} _lf_lower)
    set(_lf_target fuzz_pack_${_lf_lower}_lf)
    add_executable(${_lf_target} tools/fuzz/fuzz_pack_cov.cpp)
    arixv_apply_compile_options(${_lf_target})
    target_compile_definitions(${_lf_target} PRIVATE ARIXV_PACK_FUZZ_${_lf_mode})
    target_compile_options(${_lf_target} PRIVATE ${_lf_flags})
    target_link_options(${_lf_target} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(${_lf_target} PRIVATE arixv_internal_lf)
  endforeach()

  if(TARGET arixv_cli)
    add_custom_target(fuzz-smoke
      COMMAND ${CMAKE_COMMAND} -E env
        BUILD_DIR=$<TARGET_FILE_DIR:fuzz_roundtrip_lf>
        bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fuzz/smoke.sh"
      DEPENDS fuzz_roundtrip_lf fuzz_decode_lf ${_lf_pack_targets} arixv_cli
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      USES_TERMINAL
    )
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests (CTest)
# ---------------------------------------------------------------------------
if(ARIXV_BUILD_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)

  # Public-surface golden vector test (runs in both decoder-only & full builds)
  add_executable(golden_test tests/golden_test.cpp)
  arixv_apply_compile_options(golden_test)
  if(ARIXV_BUILD_ENCODER)
    target_link_libraries(golden_test PRIVATE arixv::encoder arixv::decoder)
  else()
    target_link_libraries(golden_test PRIVATE arixv::decoder)
  endif()
  add_test(NAME golden_test
    COMMAND golden_test --dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
  )

  # Public-surface C API contract test linked directly against shipped targets
  # (exercises the actual DSO/DLL directly in BUILD_SHARED_LIBS=ON builds).
  add_executable(public_api_test tests/public_api_test.cpp)
  arixv_apply_compile_options(public_api_test)
  if(ARIXV_BUILD_ENCODER)
    target_link_libraries(public_api_test PRIVATE arixv::encoder arixv::decoder Threads::Threads)
  else()
    target_link_libraries(public_api_test PRIVATE arixv::decoder Threads::Threads)
  endif()
  add_test(NAME public_api_test COMMAND public_api_test)

  # Decoder-safe unit tests (header/table/cursor)
  add_executable(range_coder_test tests/range_coder_test.cpp)
  arixv_apply_compile_options(range_coder_test)
  target_include_directories(range_coder_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME range_coder_test COMMAND range_coder_test)

  add_executable(grammar_tables_test
    tests/grammar_tables_test.cpp
    src/grammar_tables.cpp
    src/grammar_names.cpp
  )
  arixv_apply_compile_options(grammar_tables_test)
  target_include_directories(grammar_tables_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME grammar_tables_test COMMAND grammar_tables_test)

  add_executable(operand_cursor_test
    tests/operand_cursor_test.cpp
    src/operand_cursor.cpp
    src/grammar_tables.cpp
  )
  arixv_apply_compile_options(operand_cursor_test)
  target_include_directories(operand_cursor_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME operand_cursor_test COMMAND operand_cursor_test)

  if(ARIXV_BUILD_ENCODER)
    if(NOT TARGET arixv_internal)
      arixv_add_internal_lib(arixv_internal)
    endif()

    add_executable(arixv_test tests/arixv_test.cpp)
    arixv_apply_compile_options(arixv_test)
    target_link_libraries(arixv_test PRIVATE arixv_internal Threads::Threads)
    add_test(NAME arixv_test
      COMMAND arixv_test
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    if(ARIXV_BUILD_PACK)
      add_executable(pack_test tests/pack_test.cpp)
      arixv_apply_compile_options(pack_test)
      target_link_libraries(pack_test PRIVATE arixv_internal Threads::Threads)
      add_test(NAME pack_test
        COMMAND pack_test
          --golden-dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
          --fuzz-iters 300
      )
    endif()

    if(NOT TARGET arixv_internal_raw_residual)
      arixv_add_internal_lib(arixv_internal_raw_residual ARIXV_ENABLE_RAW_RESIDUAL)
    endif()
    add_executable(model_test tests/model_test.cpp)
    arixv_apply_compile_options(model_test)
    target_link_libraries(model_test PRIVATE arixv_internal_raw_residual)
    add_test(NAME model_test
      COMMAND model_test
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    if(TARGET fuzz_roundtrip)
      file(GLOB _corpus_spv CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/corpus/spv/*.spv"
      )
      list(SORT _corpus_spv)
      list(LENGTH _corpus_spv _corpus_len)
      if(_corpus_len GREATER 20)
        list(SUBLIST _corpus_spv 0 20 _corpus_spv)
      endif()
      add_test(NAME fuzz_roundtrip_smoke
        COMMAND fuzz_roundtrip --iters=500 ${_corpus_spv}
      )
    endif()
  endif()

  # Verify generated grammar tables stay in sync when grammar-deps are present.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND AND TARGET arixv_cli)
    add_test(NAME cli_test
      COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.py"
        --cli "$<TARGET_FILE:arixv_cli>"
        --golden "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
    )
  endif()
  set(_grammar_json_dir
    "${CMAKE_CURRENT_SOURCE_DIR}/grammar-deps/spirv-headers/include/spirv/unified1"
  )
  if(Python3_FOUND AND EXISTS "${_grammar_json_dir}")
    add_test(NAME check_generated
      COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_grammar_tables.py"
        --grammar-dir "${_grammar_json_dir}"
        --check
    )
  endif()
endif()

# ---------------------------------------------------------------------------
# Research-only multi-module sequence probes (Track B gate 1b). Opt-in, OFF by
# default; nothing here changes the standard libraries, CLI, or tests. All
# targets compile the model with ARIXV_RESEARCH_PACK and frozen default
# tuning (see docs/development/multi-module-gate1b.md).
# ---------------------------------------------------------------------------
if(ARIXV_BUILD_RESEARCH_PACK AND ARIXV_BUILD_ENCODER)
  arixv_add_internal_lib(arixv_internal_research ARIXV_RESEARCH_PACK)

  add_executable(arixv_pack_probe tools/bench/pack_probe.cpp)
  arixv_apply_compile_options(arixv_pack_probe)
  target_link_libraries(arixv_pack_probe PRIVATE arixv_internal_research)

  # Gate 2: per-instruction model-cost attribution (stats + gated hook).
  arixv_add_internal_lib(arixv_internal_research_cost ARIXV_RESEARCH_PACK
    ARIXV_ENABLE_STATS ARIXV_RESEARCH_INST_COST)
  add_executable(arixv_structure_dump tools/bench/structure_dump.cpp)
  arixv_apply_compile_options(arixv_structure_dump)
  target_link_libraries(arixv_structure_dump PRIVATE arixv_internal_research_cost)

  find_package(Threads REQUIRED)
  add_executable(research_pack_test tests/research_pack_test.cpp)
  arixv_apply_compile_options(research_pack_test)
  target_link_libraries(research_pack_test PRIVATE arixv_internal_research
                        Threads::Threads)
  if(ARIXV_BUILD_TESTS)
    add_test(NAME research_pack_test
      COMMAND research_pack_test
        --golden-dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
        --fuzz-iters 300
    )
  endif()

  if(ARIXV_BUILD_LIBFUZZER)
    set(_rlf_flags -O1 -g -fno-crash-diagnostics -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)
    add_library(arixv_internal_research_lf STATIC
      ${ARIXV_DECODER_SOURCES} ${ARIXV_ENCODER_SOURCES})
    arixv_apply_compile_options(arixv_internal_research_lf)
    target_include_directories(arixv_internal_research_lf PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(arixv_internal_research_lf
      PUBLIC ARIXV_ENCODER=1 ARIXV_RESEARCH_PACK
      PRIVATE ARIXV_BUILDING)
    target_compile_options(arixv_internal_research_lf PRIVATE ${_rlf_flags})
    foreach(_rlf_mode RT DEC)
      string(TOLOWER ${_rlf_mode} _rlf_lower)
      set(_rlf_target fuzz_research_${_rlf_lower}_lf)
      add_executable(${_rlf_target} tests/research_pack_test.cpp)
      arixv_apply_compile_options(${_rlf_target})
      target_compile_definitions(${_rlf_target} PRIVATE
        ARIXV_RESEARCH_LIBFUZZER_${_rlf_mode})
      target_compile_options(${_rlf_target} PRIVATE ${_rlf_flags})
      target_link_options(${_rlf_target} PRIVATE -fsanitize=fuzzer,address,undefined)
      target_link_libraries(${_rlf_target} PRIVATE arixv_internal_research_lf)
    endforeach()
  endif()
endif()

# ---------------------------------------------------------------------------
# Install & find_package(arixv) support
# ---------------------------------------------------------------------------
if(ARIXV_INSTALL)
  if(BUILD_SHARED_LIBS)
    set(ARIXV_INSTALLED_SHARED ON)
  else()
    set(ARIXV_INSTALLED_SHARED OFF)
  endif()
  if(ARIXV_BUILD_ENCODER)
    set(ARIXV_INSTALLED_ENCODER ON)
  else()
    set(ARIXV_INSTALLED_ENCODER OFF)
  endif()

  install(TARGETS ${ARIXV_INSTALL_TARGETS}
    EXPORT arixvTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
  install(DIRECTORY include/arixv
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )
  install(EXPORT arixvTargets
    FILE arixvTargets.cmake
    NAMESPACE arixv::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/arixv"
  )
  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/arixvConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/arixvConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/arixv"
  )
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/arixvConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/arixvConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/arixvConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/arixv"
  )
endif()
