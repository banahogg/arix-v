// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_PLATFORM_H_
#define ARIXV_PLATFORM_H_

// Cross-compiler attributes used in performance-sensitive code. Keep the
// public spelling portable; compiler-specific details belong here.
#if defined(_MSC_VER)
#define ARIXV_FORCE_INLINE __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define ARIXV_FORCE_INLINE inline __attribute__((always_inline))
#else
#define ARIXV_FORCE_INLINE inline
#endif

#if defined(_MSC_VER)
#define ARIXV_NOINLINE __declspec(noinline)
#elif defined(__clang__) || defined(__GNUC__)
#define ARIXV_NOINLINE __attribute__((noinline))
#else
#define ARIXV_NOINLINE
#endif

#endif  // ARIXV_PLATFORM_H_
