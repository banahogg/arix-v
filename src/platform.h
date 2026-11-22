// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause

#ifndef ARIXV_PLATFORM_H_
#define ARIXV_PLATFORM_H_

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// Cross-compiler attributes and intrinsics used in performance-sensitive code.
// Keep the internal spelling portable; compiler-specific details belong here.
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

#if defined(__clang__) || defined(__GNUC__)
#define ARIXV_LIKELY(x) __builtin_expect(!!(x), 1)
#define ARIXV_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define ARIXV_LIKELY(x) (x)
#define ARIXV_UNLIKELY(x) (x)
#endif

// MSVC on x64 guarantees SSE2 without defining __SSE2__.
#if (defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || \
     (defined(_M_IX86_FP) && _M_IX86_FP >= 2)) &&                 \
    !defined(ARIXV_NO_SIMD)
#define ARIXV_HAS_SSE2 1
#else
#define ARIXV_HAS_SSE2 0
#endif

#if ((defined(__aarch64__) && defined(__ARM_NEON)) || defined(_M_ARM64)) && \
    !defined(ARIXV_NO_SIMD)
#define ARIXV_HAS_NEON 1
#else
#define ARIXV_HAS_NEON 0
#endif

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define ARIXV_HAS_EXCEPTIONS 1
#else
#define ARIXV_HAS_EXCEPTIONS 0
#endif

// Encoder support is opt-in: decoder-only builds omit ARIXV_ENCODER (default 0),
// while encoder targets and source-copy encoder builds define ARIXV_ENCODER=1.
#ifndef ARIXV_ENCODER
#define ARIXV_ENCODER 0
#endif

namespace arixv {
inline namespace v1 {

// Count leading zeros in a non-zero 32-bit unsigned integer.
ARIXV_FORCE_INLINE int Clz32(uint32_t x) {
#if defined(_MSC_VER)
  unsigned long index = 0;
  _BitScanReverse(&index, static_cast<unsigned long>(x));
  return 31 - static_cast<int>(index);
#elif defined(__clang__) || defined(__GNUC__)
  return __builtin_clz(x);
#else
  int n = 0;
  if ((x & 0xFFFF0000u) == 0) { n += 16; x <<= 16; }
  if ((x & 0xFF000000u) == 0) { n += 8; x <<= 8; }
  if ((x & 0xF0000000u) == 0) { n += 4; x <<= 4; }
  if ((x & 0xC0000000u) == 0) { n += 2; x <<= 2; }
  if ((x & 0x80000000u) == 0) { n += 1; }
  return n;
#endif
}

// Count trailing zeros in a non-zero 32-bit unsigned integer.
ARIXV_FORCE_INLINE int Ctz32(uint32_t x) {
#if defined(_MSC_VER)
  unsigned long index = 0;
  _BitScanForward(&index, static_cast<unsigned long>(x));
  return static_cast<int>(index);
#elif defined(__clang__) || defined(__GNUC__)
  return __builtin_ctz(x);
#else
  int n = 0;
  if ((x & 0x0000FFFFu) == 0) { n += 16; x >>= 16; }
  if ((x & 0x000000FFu) == 0) { n += 8; x >>= 8; }
  if ((x & 0x0000000Fu) == 0) { n += 4; x >>= 4; }
  if ((x & 0x00000003u) == 0) { n += 2; x >>= 2; }
  if ((x & 0x00000001u) == 0) { n += 1; }
  return n;
#endif
}

}  // namespace v1
}  // namespace arixv

#endif  // ARIXV_PLATFORM_H_
