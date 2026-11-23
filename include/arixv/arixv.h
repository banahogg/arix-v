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
#define ARIXV_VERSION_MINOR 1
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

/* Explicit calling convention on Windows so Win32 (x86) DLL ABI is fixed. */
#ifndef ARIXV_CALL
#if defined(_WIN32) || defined(__CYGWIN__)
#define ARIXV_CALL __cdecl
#else
#define ARIXV_CALL
#endif
#endif

/* Encoder declarations are opt-in: link `arixv::encoder` in CMake or define
 * `ARIXV_ENCODER=1` when building with `src/arixv_encode.cpp`. In this public
 * header, undefined, empty (`-DARIXV_ENCODER=`), `0`, and non-zero definitions
 * are all handled safely.
 */
#if defined(ARIXV_ENCODER)
#if (ARIXV_ENCODER + 0) != 0
#define ARIXV_HAS_ENCODER_ 1
#else
#define ARIXV_HAS_ENCODER_ 0
#endif
#else
#define ARIXV_HAS_ENCODER_ 0
#endif

/* Shader-pack declarations (arixv_pack_*, 1.1) are on by default. Builds
 * configured with ARIXV_BUILD_PACK=OFF (which omit the pack code) propagate
 * `ARIXV_PACK=0`; as with ARIXV_ENCODER, an empty or zero definition turns
 * the declarations off.
 */
#if defined(ARIXV_PACK)
#if (ARIXV_PACK + 0) != 0
#define ARIXV_HAS_PACK_ 1
#else
#define ARIXV_HAS_PACK_ 0
#endif
#else
#define ARIXV_HAS_PACK_ 1
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
  ARIXV_STATUS_TOO_LARGE = 7,           /* valid container exceeds this platform's addressable limit */
  ARIXV_STATUS_DEPENDENCY_INCOMPLETE = 8 /* (1.1) pack stage 2 before every stage-1 block published */
};

/* Container payload modes reported in arixv_header_info::mode. */
enum {
  ARIXV_MODE_STORED = 0,
  ARIXV_MODE_CODED = 1,
  ARIXV_MODE_SPIRV = 2
};

/* Frozen v1.0 prefix sizes (in bytes) for versioned public structs. Future 1.x
 * versions only append fields after these prefixes, preserving v1 field offsets
 * and alignment.
 */
#define ARIXV_HEADER_INFO_V1_SIZE 24u
#define ARIXV_DECODE_OPTIONS_V1_SIZE 8u
#define ARIXV_ENCODE_OPTIONS_V1_SIZE 8u

/* Packed library version: (major << 16) | (minor << 8) | patch. */
ARIXV_API uint32_t ARIXV_CALL arixv_version(void) ARIXV_NOEXCEPT;

/* Human-readable name for a status code (never returns NULL). */
ARIXV_API const char* ARIXV_CALL arixv_status_string(arixv_status status) ARIXV_NOEXCEPT;

typedef struct arixv_header_info {
  uint32_t struct_size;        /* caller sets to sizeof(arixv_header_info) (v1: 24) */
  uint32_t magic;              /* little-endian 4CC (e.g. 0x32585241 for "ARX2") */
  uint32_t mode;               /* container mode byte (ARIXV_MODE_*) */
  uint32_t reserved;           /* set to 0 */
  uint64_t uncompressed_size;  /* declared output size in bytes (untrusted) */
} arixv_header_info;

/* O(1) header inspection without decompressing (stateless, thread-safe).
 * Validates the container header framing only (not payload length) and
 * populates `*out_info` on ARIXV_STATUS_OK. `in` and `out_info` must be
 * non-NULL (even when `in_size == 0`).
 *
 * Struct versioning:
 * - Backward compatible: future 1.x libraries accept v1 callers passing
 *   `out_info->struct_size == ARIXV_HEADER_INFO_V1_SIZE` (24) and write only
 *   the fields that fit within `out_info->struct_size`.
 * - Forward compatible: accepts larger `out_info->struct_size` up to 4096
 *   bytes, populates the v1 fields, and zero-fills any trailing extension
 *   bytes beyond `sizeof(arixv_header_info)`.
 * - Reports the full 64-bit declared `uncompressed_size` even if it exceeds
 *   this platform's addressable limit (unlike `arixv_decode`, which returns
 *   ARIXV_STATUS_TOO_LARGE).
 */
ARIXV_API arixv_status ARIXV_CALL arixv_inspect(const void* in, size_t in_size,
                                                arixv_header_info* out_info) ARIXV_NOEXCEPT;

typedef struct arixv_decode_options {
  uint32_t struct_size;  /* caller sets to sizeof(arixv_decode_options) (v1: 8) */
  uint32_t flags;        /* reserved; must be 0 */
} arixv_decode_options;

/* Decompresses a blob produced by `arixv_encode` (stateless, thread-safe).
 * `in` and `out_size` must be non-NULL (even when `in_size == 0`). `opts` may
 * be NULL for defaults. `in` and `out` must not overlap.
 *
 * Options struct versioning:
 * - Future 1.x libraries accept v1 `struct_size` (8 bytes) and default missing
 *   trailing fields to 0.
 * - Older 1.x libraries accept a larger `struct_size` (up to 4096 bytes) when
 *   all unknown trailing bytes after `sizeof(arixv_decode_options)` are 0, and
 *   return ARIXV_STATUS_INVALID_ARGUMENT if any unknown trailing byte is
 *   non-zero.
 *
 * Follows the Vulkan two-call size-query pattern:
 * - If `out == NULL`: inspects the container header without decompressing
 *   (and, for ARIXV_MODE_STORED, verifies the exact payload length in O(1),
 *   returning ARIXV_STATUS_TRUNCATED or ARIXV_STATUS_BAD_CONTAINER if it does
 *   not match), writes the declared uncompressed size (in bytes) to `*out_size`,
 *   and returns ARIXV_STATUS_OK (or a container/format/truncation/too-large
 *   error). Note: for compressed modes, `*out_size` comes from the untrusted
 *   container header; callers should validate it against their own budget
 *   before allocating.
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
ARIXV_API arixv_status ARIXV_CALL arixv_decode(const void* in, size_t in_size,
                                               void* out, size_t* out_size,
                                               const arixv_decode_options* opts) ARIXV_NOEXCEPT;

#if ARIXV_HAS_ENCODER_
typedef struct arixv_encode_options {
  uint32_t struct_size;  /* caller sets to sizeof(arixv_encode_options) (v1: 8) */
  uint32_t flags;        /* reserved; must be 0 */
} arixv_encode_options;

/* Returns the worst-case encoded size (in bytes) for an input of `in_size`
 * bytes with `opts` (NULL for defaults), or 0 if `in_size` exceeds the
 * platform's maximum supported uncompressed size or `opts` is invalid.
 * `opts` follows the same forward/backward zero-tail struct_size rules as
 * `arixv_decode_options`.
 *
 * The buffer passed to `out` in `arixv_encode` must be at least as large as
 * the return value of this function when called with matching `in_size` and
 * `opts` to guarantee the call will not fail with
 * ARIXV_STATUS_BUFFER_TOO_SMALL.
 */
ARIXV_API size_t ARIXV_CALL arixv_encode_bound(size_t in_size,
                                               const arixv_encode_options* opts) ARIXV_NOEXCEPT;

/* Compresses `in_size` bytes at `in` into `out` (stateless, thread-safe;
 * allocates internal scratch memory). `opts` may be NULL for defaults. `in`
 * and `out` must not overlap. Unlike `arixv_inspect` and `arixv_decode`, `in`
 * may be NULL when `in_size == 0`.
 *
 * `out` and `out_size` must be non-NULL; `*out_size` on entry is the capacity
 * of `out` in bytes (size it with `arixv_encode_bound(in_size, opts)`). On
 * ARIXV_STATUS_OK, `*out_size` is updated to the actual compressed byte count
 * written to `out`. If `*out_size` is smaller than the compressed result
 * (possible only if the caller provided less than
 * `arixv_encode_bound(in_size, opts)`), leaves `out` untouched, writes the
 * exact required size to `*out_size`, and returns
 * ARIXV_STATUS_BUFFER_TOO_SMALL.
 */
ARIXV_API arixv_status ARIXV_CALL arixv_encode(const void* in, size_t in_size,
                                               void* out, size_t* out_size,
                                               const arixv_encode_options* opts) ARIXV_NOEXCEPT;
#endif

#if ARIXV_HAS_PACK_
/* ==========================================================================
 * Shader packs (1.1): the ARXP container (docs/format-arxp.md).
 *
 * A pack holds an ordered set of modules (SPIR-V or arbitrary bytes) coded
 * together, so modules can borrow from each other. It decodes in at most two
 * stages of independent blocks. The library starts no threads and takes no
 * callbacks: the caller's job system runs the blocks.
 *
 *   inspect -> allocate outputs (and scratch if scratch_size > 0)
 *   -> reset scratch -> every stage-1 block (any order, concurrently)
 *   -> every stage-2 block (any order, concurrently)
 *
 * Every call is stateless and validates the whole pack header
 * (O(module_count)) before using it. `arixv_pack_decode` runs both stages on
 * the calling thread. `arixv_decode` reports a pack as
 * ARIXV_STATUS_UNSUPPORTED_FORMAT.
 * ========================================================================== */

#define ARIXV_PACK_MAGIC_0 'A'
#define ARIXV_PACK_MAGIC_1 'R'
#define ARIXV_PACK_MAGIC_2 'X'
#define ARIXV_PACK_MAGIC_3 'P'

/* Frozen v1 prefix sizes of the versioned pack structs. */
#define ARIXV_PACK_INFO_V1_SIZE 40u
#define ARIXV_PACK_DECODE_OPTIONS_V1_SIZE 8u
#define ARIXV_PACK_OUTPUTS_V1_SIZE \
  ((uint32_t)(offsetof(arixv_pack_outputs, caps) + sizeof(const size_t*)))

/* arixv_pack_info::flags */
enum { ARIXV_PACK_INFO_FAST_DECODE = 1u << 0 }; /* encoded with FAST_DECODE */

/* arixv_pack_module_info::stage */
enum { ARIXV_PACK_STAGE_1 = 1, ARIXV_PACK_STAGE_2 = 2 };

typedef struct arixv_pack_info {
  uint32_t struct_size;         /* caller sets (v1: 40); same rules as arixv_header_info */
  uint32_t magic;               /* little-endian 4CC: 0x50585241 ("ARXP") */
  uint32_t flags;               /* ARIXV_PACK_INFO_* */
  uint32_t module_count;        /* >= 1 */
  uint32_t stage1_block_count;  /* 0 for solid and plain-block packs */
  uint32_t stage2_block_count;  /* >= 1; stage1 + stage2 <= module_count */
  uint64_t total_output_size;   /* sum of the module sizes (untrusted) */
  uint64_t scratch_size;        /* scratch the two-stage path needs; 0 iff no stage 1 */
} arixv_pack_info;

/* Array element of arixv_pack_inspect_modules: fixed 16-byte layout. */
typedef struct arixv_pack_module_info {
  uint64_t size;         /* decoded bytes */
  uint32_t block_index;  /* index within its stage */
  uint8_t stage;         /* ARIXV_PACK_STAGE_1 or ARIXV_PACK_STAGE_2 */
  uint8_t reserved;      /* 0 */
  uint16_t flags;        /* 0 in 1.1; test bits, never compare the whole field */
} arixv_pack_module_info;

/* Output descriptor: plain caller data, shared by every job of a decode.
 * outs[i] and caps[i] are indexed by original module index. A call reads
 * only its own block's entries, so other entries may be NULL or unset. An
 * output may be NULL only when its module is 0 bytes; outputs need no
 * alignment (Vulkan callers use uint32_t-aligned storage). */
typedef struct arixv_pack_outputs {
  uint32_t struct_size;   /* caller sets (ARIXV_PACK_OUTPUTS_V1_SIZE or sizeof) */
  uint32_t module_count;  /* must equal arixv_pack_info::module_count */
  void* const* outs;      /* [module_count] */
  const size_t* caps;     /* [module_count]: capacity of each output */
} arixv_pack_outputs;

typedef struct arixv_pack_decode_options {
  uint32_t struct_size;  /* caller sets (v1: 8); same rules as arixv_decode_options */
  uint32_t flags;        /* reserved; must be 0 */
} arixv_pack_decode_options;

/* Validates the whole header and reports counts and sizes. On OK they are
 * mutually consistent and every module belongs to exactly one block, but
 * they are still the pack's own claims: check module_count,
 * total_output_size and scratch_size against your budget (and against
 * SIZE_MAX on 32-bit targets) before allocating. Reports full 64-bit sizes
 * and never returns TOO_LARGE. `*out_info` is untouched on error. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_inspect(
    const void* in, size_t in_size, arixv_pack_info* out_info) ARIXV_NOEXCEPT;

/* Per-module size, stage and block, in original module order (same
 * validation). `count` must equal module_count; `out_modules` is untouched
 * on error. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_inspect_modules(
    const void* in, size_t in_size, arixv_pack_module_info* out_modules,
    uint32_t count) ARIXV_NOEXCEPT;

/* Starts a decode attempt on `scratch`: required once per attempt before any
 * stage-1 call when scratch_size > 0, after every call of an earlier attempt
 * on the same buffer has returned. Touches only a small prefix. When the
 * pack needs no scratch it is an OK no-op, and (NULL, 0) or any buffer is
 * accepted. Scratch needs no particular alignment. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_reset_scratch(
    const void* in, size_t in_size, void* scratch,
    size_t scratch_size) ARIXV_NOEXCEPT;

/* Stage 1: block_index in [0, stage1_block_count). Decodes the block into
 * its modules' outputs and publishes shared data into its own part of
 * `scratch`. Calls with distinct indices may run concurrently. Lower indices
 * are larger blocks, so issue them first. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_decode_stage1(
    const void* in, size_t in_size, uint32_t block_index,
    const arixv_pack_outputs* outputs, void* scratch, size_t scratch_size,
    const arixv_pack_decode_options* opts) ARIXV_NOEXCEPT;

/* Stage 2: block_index in [0, stage2_block_count). Precondition: every
 * stage-1 call of this attempt returned OK, with a happens-before edge to
 * this call. Reads `scratch` only. Calls with distinct indices may run
 * concurrently. Returns ARIXV_STATUS_DEPENDENCY_INCOMPLETE if a stage-1
 * block has not published in this attempt (a diagnostic, not a
 * synchronization mechanism). */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_decode_stage2(
    const void* in, size_t in_size, uint32_t block_index,
    const arixv_pack_outputs* outputs, const void* scratch,
    size_t scratch_size, const arixv_pack_decode_options* opts) ARIXV_NOEXCEPT;

/* Decodes every block on the calling thread, allocating any scratch
 * internally. Checks every output before writing any. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_decode(
    const void* in, size_t in_size, const arixv_pack_outputs* outputs,
    const arixv_pack_decode_options* opts) ARIXV_NOEXCEPT;

/* Pack decode errors (every call checks in this order, writing nothing until
 * step 6):
 *   1. INVALID_ARGUMENT: NULL `in` or `outputs`, NULL outs/caps, a bad
 *      struct_size or options, or NULL scratch with nonzero scratch_size.
 *   2. BAD_CONTAINER / UNSUPPORTED_FORMAT / TRUNCATED: header validation.
 *   3. INVALID_ARGUMENT: module_count mismatch, block_index out of range, or
 *      NULL scratch when scratch is needed; TOO_LARGE: needed scratch or an
 *      own output beyond this platform's addressable span.
 *   4. BUFFER_TOO_SMALL: short scratch; then for the block's own modules a
 *      NULL output with nonzero size (INVALID_ARGUMENT) and, after all of
 *      those, a short capacity (BUFFER_TOO_SMALL). Required sizes are not
 *      written back; they come from inspection.
 *   5. DEPENDENCY_INCOMPLETE (stage 2 only).
 *   6. BAD_CONTAINER / TRUNCATED: corrupt payload. The block's outputs (and
 *      a stage-1 block's scratch part) are then unspecified; other blocks
 *      are unaffected. If stage 1 failed, that status is the root cause. */

#if ARIXV_HAS_ENCODER_
#define ARIXV_PACK_ENCODE_OPTIONS_V1_SIZE 24u

/* arixv_pack_encode_options::flags */
enum {
  /* Copied instructions skip probability training: faster decode, slightly
   * larger packs. */
  ARIXV_PACK_ENCODE_FAST_DECODE = 1u << 0
};

typedef struct arixv_pack_encode_options {
  uint32_t struct_size;         /* caller sets (v1: 24); same rules as arixv_encode_options */
  uint32_t flags;               /* ARIXV_PACK_ENCODE_*; other bits 0 */
  uint32_t parallelism;         /* decode-width hint: 0 = library default (8 in 1.1),
                                   1 = solid, N = aim for N workers */
  uint32_t latency_budget_pct;  /* 0 = default (150), else 100..100000: see below */
  uint64_t max_block_bytes;     /* 0 = none: close a block before it would exceed this */
} arixv_pack_encode_options;

/* Encoder layout semantics (heuristics, not decode-time guarantees):
 * - `parallelism` is a hint for the decode width, not a block count; values
 *   above the module count act as the module count.
 * - `latency_budget_pct` trades size for decode latency: the allowed
 *   critical path (stage 1 plus stage 2) as a percent of an ideal estimate
 *   for `parallelism` workers, modelling decode time as proportional to
 *   decoded bytes. 100 means no stage 1 and the most, smallest blocks;
 *   larger values buy larger blocks and a shared stage 1. Measured decode
 *   times depend on content and hardware and may exceed the budget.
 * - Exact duplicates are coded as references where reachable; zero-byte,
 *   header-only and non-SPIR-V modules are accepted; incompressible blocks
 *   are stored.
 * - Output is deterministic for the same inputs, options and library
 *   version; layout may change between library versions. */

/* Worst-case output size for these module sizes and options (every block
 * stored, worst-case header). Returns 0 for NULL `sizes`, count == 0 or
 * above 2^20, a size above UINT32_MAX, invalid options, or overflow. */
ARIXV_API size_t ARIXV_CALL arixv_pack_encode_bound(
    const size_t* sizes, uint32_t count,
    const arixv_pack_encode_options* opts) ARIXV_NOEXCEPT;

/* Encodes `count` modules into one pack (stateless, thread-safe,
 * single-threaded; allocates internally). modules[i] may be NULL only when
 * sizes[i] == 0; modules may alias each other but not `out`. `out`,
 * `out_size`, and BUFFER_TOO_SMALL follow arixv_encode. count == 0 or above
 * 2^20, a size above UINT32_MAX, or invalid `opts` -> INVALID_ARGUMENT.
 * Decode outputs are indexed by the input order. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_encode(
    const void* const* modules, const size_t* sizes, uint32_t count,
    void* out, size_t* out_size,
    const arixv_pack_encode_options* opts) ARIXV_NOEXCEPT;
#endif /* ARIXV_HAS_ENCODER_ */
#endif /* ARIXV_HAS_PACK_ */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ARIXV_ARIXV_H_ */
