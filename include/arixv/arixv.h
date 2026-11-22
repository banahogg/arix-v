/* Copyright (c) 2026, Eric Werness
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ARIXV_ARIXV_H_
#define ARIXV_ARIXV_H_

/* ARIX-V (Arithmetic Reduction Intermediate Transform - V): a standalone,
 * purely adaptive SPIR-V compressor.
 *
 * Public C API (C99/C11 and C++ compatible). The implementation is C++17
 * internally; CMake propagates C++ runtime linkage automatically, and
 * non-CMake static consumers should link the platform C++ runtime
 * (-lstdc++ / -lc++).
 *
 * Exception / OOM contract:
 * - When built with C++ exceptions enabled, allocation failures are caught
 *   at the C boundary and reported as ARIXV_STATUS_OUT_OF_MEMORY.
 * - When built with exceptions disabled (-fno-exceptions / /EHs-c-), internal
 *   out-of-memory conditions abort the process in v1.0.
 */

#include <stddef.h>
#include <stdint.h>

#define ARIXV_VERSION_MAJOR 1
#define ARIXV_VERSION_MINOR 0
#define ARIXV_VERSION_PATCH 0

#define ARIXV_MAGIC_0 'A'
#define ARIXV_MAGIC_1 'R'
#define ARIXV_MAGIC_2 'X'
#define ARIXV_MAGIC_3 '2'

/* Symbol visibility macro. Defaults to empty (safe for static libraries and
 * direct source-copy builds). Shared builds define ARIXV_SHARED publicly and
 * ARIXV_BUILDING while compiling the library.
 */
#ifndef ARIXV_API
#if defined(ARIXV_SHARED)
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(ARIXV_BUILDING)
#define ARIXV_API __declspec(dllexport)
#else
#define ARIXV_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ARIXV_API __attribute__((visibility("default")))
#else
#define ARIXV_API
#endif
#else
#define ARIXV_API
#endif
#endif

#ifdef __cplusplus
#define ARIXV_NOEXCEPT noexcept
extern "C" {
#else
#define ARIXV_NOEXCEPT
#endif

/* Fixed-width 32-bit status code for stable C/C++ ABI across compilers.
 * Callers should handle unknown future status values gracefully.
 */
typedef int32_t arixv_status;
enum {
  ARIXV_STATUS_OK = 0,
  ARIXV_STATUS_BAD_CONTAINER = 1,       /* malformed header, bad mode, or trailing bytes */
  ARIXV_STATUS_UNSUPPORTED_FORMAT = 2,  /* recognized ARX? magic for newer/unsupported version */
  ARIXV_STATUS_TRUNCATED = 3,           /* input ends before declared output is complete */
  ARIXV_STATUS_BUFFER_TOO_SMALL = 4,    /* out != NULL and *out_size < required (*out_size updated) */
  ARIXV_STATUS_INVALID_ARGUMENT = 5,    /* null required pointer, invalid opts, or size > limit */
  ARIXV_STATUS_OUT_OF_MEMORY = 6,       /* internal allocation failed (exceptions-enabled builds) */
  ARIXV_STATUS_TOO_LARGE = 7            /* valid container exceeds this platform's addressable limit */
};

/* Container payload modes reported in arixv_header_info::mode. */
enum {
  ARIXV_MODE_STORED = 0,
  ARIXV_MODE_CODED = 1,
  ARIXV_MODE_SPIRV = 2
};

/* Packed library version: (major << 16) | (minor << 8) | patch. */
ARIXV_API uint32_t arixv_version(void) ARIXV_NOEXCEPT;

/* Human-readable name for a status code (never returns NULL). */
ARIXV_API const char* arixv_status_string(arixv_status status) ARIXV_NOEXCEPT;

typedef struct arixv_header_info {
  uint32_t struct_size;        /* caller must set to sizeof(arixv_header_info) */
  uint32_t magic;              /* little-endian 4CC (e.g. 0x32585241 for "ARX2") */
  uint32_t mode;               /* container mode byte (ARIXV_MODE_*) */
  uint32_t reserved;           /* set to 0 */
  uint64_t uncompressed_size;  /* declared output size in bytes (untrusted) */
} arixv_header_info;

/* O(1) header inspection without decompressing (stateless, thread-safe).
 * Validates the container header and populates `*out_info` on ARIXV_STATUS_OK.
 * Accepts any `out_info->struct_size` in [v1_size, sizeof(arixv_header_info)].
 * Reports the full 64-bit declared `uncompressed_size` even if it exceeds this
 * platform's addressable limit (unlike `arixv_decode`, which returns
 * ARIXV_STATUS_TOO_LARGE).
 */
ARIXV_API arixv_status arixv_inspect(const void* in, size_t in_size,
                                     arixv_header_info* out_info) ARIXV_NOEXCEPT;

typedef struct arixv_decode_options {
  uint32_t struct_size;  /* must be in [v1_size, sizeof(arixv_decode_options)] */
  uint32_t flags;        /* reserved; must be 0 */
} arixv_decode_options;

/* Decompresses a blob produced by `arixv_encode` (stateless, thread-safe).
 * `opts` may be NULL for defaults. `in` and `out` must not overlap.
 *
 * Follows the Vulkan two-call size-query pattern:
 * - If `out == NULL`: inspects the container header without decompressing,
 *   writes the declared uncompressed size (in bytes) to `*out_size`, and
 *   returns ARIXV_STATUS_OK (or a container/format/truncation/too-large error).
 *   Note: `*out_size` comes from the untrusted container header; callers should
 *   validate it against their own size budget before allocating.
 * - If `out != NULL`: `*out_size` on entry is the capacity of `out` in bytes.
 *   If `*out_size` is smaller than the declared output size, leaves `out`
 *   untouched, writes the required size to `*out_size`, and returns
 *   ARIXV_STATUS_BUFFER_TOO_SMALL (allowing single-call optimistic decoding
 *   into a reusable scratch buffer). Otherwise decodes directly into `out`,
 *   writes the exact byte count to `*out_size`, and returns ARIXV_STATUS_OK.
 *   On any error other than ARIXV_STATUS_BUFFER_TOO_SMALL, `out` contents are
 *   unspecified and `*out_size` is unchanged.
 * - 0-byte outputs are unambiguous: `out == NULL` always queries size;
 *   `out != NULL` with `*out_size == 0` decodes a 0-byte payload.
 * - `out` need not be aligned for this byte API; Vulkan callers passing the
 *   result to `VkShaderModuleCreateInfo::pCode` should allocate `uint32_t`-
 *   aligned storage.
 */
ARIXV_API arixv_status arixv_decode(const void* in, size_t in_size,
                                    void* out, size_t* out_size,
                                    const arixv_decode_options* opts) ARIXV_NOEXCEPT;

#if !defined(ARIXV_ENCODER) || (ARIXV_ENCODER != 0)
typedef struct arixv_encode_options {
  uint32_t struct_size;  /* must be in [v1_size, sizeof(arixv_encode_options)] */
  uint32_t flags;        /* reserved; must be 0 */
} arixv_encode_options;

/* Returns the worst-case encoded size (in bytes) for an input of `in_size`
 * bytes with `opts` (NULL for defaults), or 0 if `in_size` exceeds the
 * platform's maximum supported uncompressed size or `opts` is invalid.
 *
 * The buffer passed to `out` in `arixv_encode` must be at least as large as
 * the return value of this function when called with matching `in_size` and
 * `opts` to guarantee the call will not fail with
 * ARIXV_STATUS_BUFFER_TOO_SMALL.
 */
ARIXV_API size_t arixv_encode_bound(size_t in_size,
                                    const arixv_encode_options* opts) ARIXV_NOEXCEPT;

/* Compresses `in_size` bytes at `in` into `out` (stateless, thread-safe;
 * allocates internal scratch memory). `opts` may be NULL for defaults. `in`
 * and `out` must not overlap. `in` may be NULL only when `in_size == 0`.
 *
 * `out` must be non-NULL; `*out_size` on entry is the capacity of `out` in
 * bytes (size it with `arixv_encode_bound(in_size, opts)`). On ARIXV_STATUS_OK,
 * `*out_size` is updated to the actual compressed byte count written to `out`.
 * If `*out_size` is smaller than the compressed result (possible only if the
 * caller provided less than `arixv_encode_bound(in_size, opts)`), leaves `out`
 * untouched, writes the exact required size to `*out_size`, and returns
 * ARIXV_STATUS_BUFFER_TOO_SMALL.
 */
ARIXV_API arixv_status arixv_encode(const void* in, size_t in_size,
                                    void* out, size_t* out_size,
                                    const arixv_encode_options* opts) ARIXV_NOEXCEPT;
#endif

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ARIXV_ARIXV_H_ */
