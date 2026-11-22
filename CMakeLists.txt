cmake_minimum_required(VERSION 3.21)

project(arixv
  VERSION 1.0.0
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
    PRIVATE
      ARIXV_BUILDING
      ARIXV_ENCODER=$<BOOL:${ARIXV_BUILD_ENCODER}>
  )
  if(NOT ARIXV_BUILD_ENCODER)
    target_compile_definitions(arixv_shared PUBLIC ARIXV_ENCODER=0)
  endif()
  set_target_properties(arixv_shared PROPERTIES
    OUTPUT_NAME arixv
    EXPORT_NAME shared
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
  )
  if(UNIX AND NOT APPLE)
    set(_arixv_version_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/arixv.map")
    target_link_options(arixv_shared PRIVATE
      "-Wl,--version-script=${_arixv_version_script}"
    )
    set_property(TARGET arixv_shared APPEND PROPERTY
      LINK_DEPENDS "${_arixv_version_script}"
    )
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
    PRIVATE
      ARIXV_BUILDING
      ARIXV_ENCODER=$<BOOL:${ARIXV_BUILD_ENCODER}>
  )
  if(NOT ARIXV_BUILD_ENCODER)
    target_compile_definitions(arixv_decoder PUBLIC ARIXV_ENCODER=0)
  endif()
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
  set_target_properties(arixv_decoder PROPERTIES EXPORT_NAME decoder)
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
    set_target_properties(arixv_encoder PROPERTIES EXPORT_NAME encoder)
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
  target_compile_definitions(arixv_internal_lf
    PUBLIC
      ARIXV_ENCODER=1
    PRIVATE
      ARIXV_BUILDING
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

  if(TARGET arixv_cli)
    add_custom_target(fuzz-smoke
      COMMAND ${CMAKE_COMMAND} -E env
        BUILD_DIR=$<TARGET_FILE_DIR:fuzz_roundtrip_lf>
        bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fuzz/smoke.sh"
      DEPENDS fuzz_roundtrip_lf fuzz_decode_lf arixv_cli
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
  target_compile_definitions(golden_test PRIVATE
    ARIXV_ENCODER=$<BOOL:${ARIXV_BUILD_ENCODER}>
  )
  if(ARIXV_BUILD_ENCODER)
    target_link_libraries(golden_test PRIVATE arixv::encoder arixv::decoder)
  else()
    target_link_libraries(golden_test PRIVATE arixv::decoder)
  endif()
  add_test(NAME golden_test
    COMMAND golden_test --dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
  )

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
# Install & find_package(arixv) support
# ---------------------------------------------------------------------------
if(ARIXV_INSTALL)
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
