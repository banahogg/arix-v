// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_TESTS_TEST_UTIL_H_
#define ARIXV_TESTS_TEST_UTIL_H_

#include <cstdio>
#include <cstdlib>

// Always-active test assertion macro. Unlike assert(), CHECK() is never
// compiled out in Release (-DNDEBUG) builds, so every test verification and
// side-effecting call executes in every build configuration.
#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #expr);                                                     \
      std::fflush(stderr);                                                     \
      std::abort();                                                            \
    }                                                                          \
  } while (false)

#endif  // ARIXV_TESTS_TEST_UTIL_H_
