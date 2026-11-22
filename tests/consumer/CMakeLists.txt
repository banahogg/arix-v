cmake_minimum_required(VERSION 3.21)

project(arixv_consumer_smoke LANGUAGES CXX)

set(ARIXV_CONSUMER_MODE "find_package" CACHE STRING "find_package or fetchcontent")
set(ARIXV_CONSUMER_SOURCE_DIR "" CACHE PATH "Path to arix-v source dir for fetchcontent mode")
option(ARIXV_CONSUMER_LINK_ENCODER "Link both arixv::decoder and arixv::encoder" ON)

if(ARIXV_CONSUMER_MODE STREQUAL "fetchcontent")
  include(FetchContent)
  if(ARIXV_CONSUMER_LINK_ENCODER)
    set(ARIXV_BUILD_ENCODER ON CACHE BOOL "" FORCE)
  else()
    set(ARIXV_BUILD_ENCODER OFF CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(arixv
    SOURCE_DIR "${ARIXV_CONSUMER_SOURCE_DIR}"
  )
  FetchContent_MakeAvailable(arixv)
elseif(ARIXV_CONSUMER_MODE STREQUAL "find_package")
  find_package(arixv REQUIRED)
else()
  message(FATAL_ERROR "Unknown ARIXV_CONSUMER_MODE: ${ARIXV_CONSUMER_MODE}")
endif()

add_executable(consumer_smoke main.cpp)
target_compile_features(consumer_smoke PRIVATE cxx_std_17)
if(ARIXV_CONSUMER_LINK_ENCODER)
  # Intentionally link BOTH targets into one executable to verify diamond static
  # builds and single-DSO shared builds on ELF, Mach-O, and Windows DLLs.
  target_link_libraries(consumer_smoke PRIVATE arixv::decoder arixv::encoder)
  target_compile_definitions(consumer_smoke PRIVATE CONSUMER_HAS_ENCODER=1)
else()
  target_link_libraries(consumer_smoke PRIVATE arixv::decoder)
  target_compile_definitions(consumer_smoke PRIVATE CONSUMER_HAS_ENCODER=0)
endif()
