# Pack API: two-stage blocks (v6)

v6, 2026-11-22. Track B of the [multi-module plan](multi-module.md),
following the [gate-4 report](multi-module-gate4.md).

This is the design record of the public shader-pack C API, **shipped in
library 1.1** as `arixv_pack_*` in `include/arixv/arixv.h`, over the ARXP v1
format ([spec](../format-arxp.md)). The header is normative where the two
differ. The implementation is `src/arixv_pack.cpp` (container and decode),
`src/pack_block.inc` (block codec) and `src/arixv_pack_encode.cpp`
(encoder). [§9](#9-prototype-measurements) measured the research prototype
it was ported from, which produced the same bytes (the ARXP golden vectors
reproduce unchanged). The document has had four rounds of independent
review (of v1, v2, v4 and the v5 integration sketches), and
[§10](#10-decisions-record) records the decisions.

v6 applies the review of the integration sketches and ships the API:

- `arixv_pack_module_info` reports the stage as an explicit field
  (`ARIXV_PACK_STAGE_1` / `ARIXV_PACK_STAGE_2`) instead of a flag bit;
- the integration pattern (§3.6) tracks each stage's errors separately,
  budgets the module count and scratch, and publishes after success by
  default;
- a "pack view" argument struct and merging `inspect_modules` into
  `inspect` were considered and rejected, and the scratch reset stays
  explicit (§10);
- `arixv_pack_decode` needs no scratch at all: it keeps stage-1 modules in
  memory and reads them directly.

v5 applied the v4 reviews:

- a stored block mode and a real encode bound;
- canonical varints and 1.0-consistent status order for short input;
- explicit format limits;
- the private-memory bound gains a module-count term;
- a narrower, accurate scope for `DEPENDENCY_INCOMPLETE`;
- check-order and ABI fixes.

The API shape is unchanged since v3.

The conventions follow the frozen 1.0 C header (`include/arixv/arixv.h` on
`main`):

- `ARIXV_API` / `ARIXV_CALL` / `ARIXV_NOEXCEPT`;
- the `ARIXV_HAS_ENCODER_` gate;
- `struct_size`-versioned structs with frozen `*_V1_SIZE` prefixes, accepting
  NULL options and using the zero-tail rules up to 4096 bytes;
- caller-owned buffers, and fixed-width status codes with new values
  appended;
- the same OOM contract.

## 1. Context and goals

A pack is an ordered set of separate SPIR-V modules (one shader set). The
pack codec copies instruction runs between modules with ID restoration. On
real application sets it is 47–79% smaller than independent ARX2.

A solid stream decodes on one thread only, so packs are split into blocks
decoded in two stages:

- **Stage 1:** zero or more independent blocks. Each publishes its modules
  as copy sources into caller-owned scratch.
- **Stage 2:** one or more independent blocks. Each may copy from all of
  stage 1, reading scratch only.

| Pack | Stage 1 count | Stage 2 count | Scratch |
|---|---|---|---|
| Solid | 0 | 1 | none |
| Plain independent blocks | 0 | N | none |
| Shared-base pack | M > 0 | N | needed |

Goals:

1. **The engine's job system owns all threading.** The library creates no
   threads, takes no callbacks, holds no library-managed locks, and has no
   global state. Calls never wait for other calls.
2. **Minimal surface and no extra objects.** Calls are stateless. A pack
   without stage 1 needs no scratch. One call decodes everything on the
   calling thread.
3. **Caller-owned outputs and shared data.** Sizes come from a validated
   header before decoding.
4. **Explicit dependencies.** The stages are separate entry points.
5. **Untrusted input stays safe.** No input can cause out-of-bounds access,
   unbounded allocation, a skipped output, or two jobs writing the same
   output or scratch range.
6. **Additive.** ARX2 and `arixv_encode` / `arixv_decode` are unchanged.
   Packs use the distinct magic `"ARXP"` ([format spec](../format-arxp.md)).
   `arixv_decode` reports them as `UNSUPPORTED_FORMAT`, since `"ARX?"` is
   reserved for successor formats. The pack calls reject ARX2 blobs with
   `BAD_CONTAINER`.

Non-goals (v1):

- streaming input (the whole pack is resident);
- incremental re-encode;
- per-block dependency subsets;
- library-managed parallelism.

## 2. Format requirements the API relies on

The format is not specified here. These properties are required, and the
prototype (`src/research_axsb.inc`, ARXP v1) demonstrates each of them.

- **F1. Ownership is a partition, checked on every call.** Each module has
  exactly one `(block, position)` entry. The position is the module's coding
  order within its block, so the encoder may order modules inside a block
  freely. Validation places every module in a block-major slot table, and
  any repeated or out-of-range slot is rejected. Every block owns at least
  one module, so the block count is at most the module count.
- **F2. Every call validates the whole header.** That covers magic, version,
  counts, the module table, block lengths, the checksum, and the exact
  payload end; trailing bytes are an error. Payload offsets are prefix sums
  of block lengths, so block payloads are disjoint by construction.
  - Cost: O(module_count), measured at 0.3–7 µs per call for 9–398 modules
    (§9). That is under 1.5% of decode work in every measured pack, and
    under 0.1% for large ones.
  - Whole-header validation replaces v2's "own entries only" rule. Per-entry
    checks alone missed out-of-range blocks, which skipped outputs, and
    overlapping scratch slices.
- **F3. Header checksum.** It detects corruption of the header, and lets
  completion records name the header they were published for (§3.4). It
  covers the header only, so two packs with identical headers but different
  payloads share a checksum. It is not an authentication or identity
  mechanism.
- **F4. Largest-first block order.** Within each stage, blocks are in
  descending estimated decode work; the prototype uses decoded bytes as the
  proxy. Issuing jobs in index order is the recommended schedule. The parser
  does not check the order.
- **F5. Compact header** (ARXP v1, specified in
  [`docs/format-arxp.md`](../format-arxp.md)):

  ```text
  "ARXP" | version 1 | pack flags (bit 0: implicit coding order; others 0)
  | codec flags (bit 0 WARM, bit 1 FAST_DECODE; others 0) | reserved 0
  | var module_count | var stage1_blocks | var stage2_blocks
  | per module, in original order: var size, var block (global index; < stage1 = stage 1)
  | [unless implicit order] per module: var position in its block
  | per block: var (payload_length << 1 | stored)
  | 64-bit FNV-1a of the preceding header bytes, little-endian
  | block payloads in block order: a bare coder payload, or (stored) the
    block's module bytes concatenated in coding order
  ```

  - **Varints** are canonical unsigned LEB128: shortest form, at most 64
    bits. Non-minimal or overflowing encodings return `BAD_CONTAINER`; input
    ending inside one returns `TRUNCATED`.
  - **Implicit order** means ascending module index within each block, the
    encoder's default.
  - **Before any table is allocated,** the parser checks that the input
    holds every mandatory header byte: per module two varints (three with
    explicit order), per block one, and the checksum.
  - **Status order follows `arixv_decode`:**
    - under 4 bytes or a foreign magic → `BAD_CONTAINER` (an ARX2 blob
      included);
    - `"ARXP"` with nothing after it, or fewer than 8 bytes → `TRUNCATED`;
    - another version → `UNSUPPORTED_FORMAT`.
  - **The format version pins** the block codec revision, the meaning of
    the codec flags, and the shared-member order (stage-1 blocks in order,
    members in coding order). Incompatible changes bump the version; there
    is no extension directory.
  - **Size:** about 3 bytes per module. Solid packs are 0.04–0.6% larger
    than raw solid AXST, against 4–9% for v3's fixed-width prototype header.
- **F6. Stored blocks.** A stored block's payload must be exactly the sum of
  its module sizes (checked).
  - The encoder stores a block whenever coding would not make it smaller,
    so every pack fits within `arixv_pack_encode_bound`, which assumes every
    block is stored plus the worst-case header.
  - A stored stage-1 block publishes its bytes, which serve as
    exact-duplicate sources only, not as copy sources.
- **F7. Format limits:**
  - `module_count` ≤ 2^20;
  - module size ≤ 2^32 − 1 bytes;
  - block count ≤ `module_count`.

  Under these limits all header and scratch arithmetic fits in 64 bits.
  Sizes are also checked against the platform's addressable span
  (`PTRDIFF_MAX`, as in 1.0) before use.

Scratch size and layout are not part of the format. The library derives
them from the validated module sizes, so later versions can change the
scratch representation.

## 3. Decode API

```c
/* New status code (appended to the status enum; arixv_status_string updated). */
ARIXV_STATUS_DEPENDENCY_INCOMPLETE = 8

#define ARIXV_PACK_MAGIC_0 'A'                     /* "ARXP" */
#define ARIXV_PACK_MAGIC_1 'R'
#define ARIXV_PACK_MAGIC_2 'X'
#define ARIXV_PACK_MAGIC_3 'P'
#define ARIXV_PACK_INFO_V1_SIZE            40u
#define ARIXV_PACK_DECODE_OPTIONS_V1_SIZE   8u
#define ARIXV_PACK_OUTPUTS_V1_SIZE \
  ((uint32_t)(offsetof(arixv_pack_outputs, caps) + sizeof(const size_t*)))

/* arixv_pack_info::flags */
enum { ARIXV_PACK_INFO_FAST_DECODE = 1u << 0 };   /* encoded with FAST_DECODE */

/* arixv_pack_module_info::stage */
enum { ARIXV_PACK_STAGE_1 = 1, ARIXV_PACK_STAGE_2 = 2 };

typedef struct arixv_pack_info {
  uint32_t struct_size;          /* caller sets (v1: 40) */
  uint32_t magic;                /* little-endian 4CC */
  uint32_t flags;                /* ARIXV_PACK_INFO_* */
  uint32_t module_count;         /* >= 1 */
  uint32_t stage1_block_count;   /* 0 for solid and plain-block packs */
  uint32_t stage2_block_count;   /* >= 1; stage1 + stage2 <= module_count */
  uint64_t total_output_size;    /* equals the sum of module sizes */
  uint64_t scratch_size;         /* required scratch; 0 iff stage1_block_count == 0 */
} arixv_pack_info;

/* Array element; fixed 16-byte layout, not versioned. */
typedef struct arixv_pack_module_info {
  uint64_t size;                 /* decoded bytes */
  uint32_t block_index;          /* index within its stage */
  uint8_t  stage;                /* ARIXV_PACK_STAGE_1 or ARIXV_PACK_STAGE_2 */
  uint8_t  reserved;             /* 0 */
  uint16_t flags;                /* 0 in 1.1; test bits, never compare the field */
} arixv_pack_module_info;

/* Output descriptor: plain caller data, no lifetime of its own. One instance
 * serves every job of a decode. */
typedef struct arixv_pack_outputs {
  uint32_t      struct_size;     /* caller sets */
  uint32_t      module_count;    /* must equal arixv_pack_info::module_count */
  void* const*  outs;            /* [module_count], original module order */
  const size_t* caps;            /* [module_count] */
} arixv_pack_outputs;

typedef struct arixv_pack_decode_options {
  uint32_t struct_size;          /* caller sets (v1: 8) */
  uint32_t flags;                /* reserved; must be 0 */
  /* 1.x may append an optional per-job workspace (§3.5); extension fields
   * are read by byte offset, so the v1 prefix keeps its alignment. */
} arixv_pack_decode_options;

/* ---- Inspection (stateless, thread-safe) -------------------------------- */

/* Validates the whole header (O(module_count)). On OK the counts and sizes
 * are mutually consistent and every module belongs to exactly one block,
 * but the values are still the pack's own claims, so check them against
 * your budget. Reports full 64-bit sizes; never returns TOO_LARGE.
 * out_info is untouched on error. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_inspect(
    const void* in, size_t in_size, arixv_pack_info* out_info) ARIXV_NOEXCEPT;

/* Per-module size, stage and block in original module order (same
 * validation). `count` must equal module_count. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_inspect_modules(
    const void* in, size_t in_size,
    arixv_pack_module_info* out_modules, uint32_t count) ARIXV_NOEXCEPT;

/* ---- Two-stage decode (engine job system owns threading) ---------------- */

/* Starts a decode attempt on `scratch`: required once per attempt, before
 * any stage-1 call, whenever scratch_size > 0. Touches only the
 * completion-record prefix. No-op (OK) when the pack needs no scratch. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_reset_scratch(
    const void* in, size_t in_size,
    void* scratch, size_t scratch_size) ARIXV_NOEXCEPT;

/* Stage 1: block_index in [0, stage1_block_count). Writes the block's
 * modules into `outputs` and its own scratch slice. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_decode_stage1(
    const void* in, size_t in_size, uint32_t block_index,
    const arixv_pack_outputs* outputs,
    void* scratch, size_t scratch_size,
    const arixv_pack_decode_options* opts) ARIXV_NOEXCEPT;

/* Stage 2: block_index in [0, stage2_block_count). Precondition: every
 * stage-1 call of this attempt returned OK, with a happens-before edge to
 * this call. Reads `scratch` only. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_decode_stage2(
    const void* in, size_t in_size, uint32_t block_index,
    const arixv_pack_outputs* outputs,
    const void* scratch, size_t scratch_size,
    const arixv_pack_decode_options* opts) ARIXV_NOEXCEPT;

/* ---- Single-thread convenience ----------------------------------------- */

/* Validates and checks every capacity first, then runs stage 1 and stage 2
 * on the calling thread. Needs no scratch: stage-1 modules stay in memory. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_decode(
    const void* in, size_t in_size,
    const arixv_pack_outputs* outputs,
    const arixv_pack_decode_options* opts) ARIXV_NOEXCEPT;
```

### 3.1 Threading contract

- **May run concurrently:**
  - calls with distinct block indices within one stage of one attempt;
  - independent attempts with separate scratch and outputs;
  - inspection calls with anything.
- **Callers must not do any of these** (data races or broken preconditions):
  - run the same stage and index twice in one attempt;
  - overlap stage 1 and stage 2 of one attempt;
  - reset scratch while any call of the attempt may still be running;
  - start stage 2 after any stage-1 failure in that attempt. Abandon the
    attempt, drain running jobs, then reset and retry if wanted.
- **Any number of workers works**, including one, with the same two-stage
  graph.

### 3.2 Outputs

- `outputs->outs` and `outputs->caps` are indexed by original module index.
- A call reads only the `outs[i]` and `caps[i]` entries of its own block's
  modules, and writes only the buffers they point to. It never reads
  another block's entries, so those may be NULL or garbage, and workers may
  fill their own slots of a shared pointer array, for example with per-job
  temporary buffers.
- **Own entries:**
  - an output may be NULL only when its size is 0;
  - a capacity at least the module size is required;
  - both are checked for all of the block's modules before anything is
    written.
- **Overlap:** none of these may overlap: `in`, `scratch`, the descriptor,
  the `outs` and `caps` arrays, the options, and every output range.
  Disjoint slices of one caller arena are fine.
- **Lifetimes:** the input, descriptor, options and the call's own slots
  must stay valid and unchanged until the call returns.
- **Alignment:** outputs need not be aligned. Vulkan callers allocate
  `uint32_t`-aligned storage. A caller slicing one slab of
  `total_output_size` bytes must round each module's offset up to 4: a pack
  may hold non-SPIR-V modules whose sizes are not multiples of 4.
- **No size write-back:** on `BUFFER_TOO_SMALL`, pack decode calls do not
  report required sizes, unlike `arixv_decode`'s `*out_size`. Sizes come
  from inspection.
- **Publication:** a successful call publishes only its own block's
  outputs. They are complete when it returns OK. Publishing them to other
  threads uses the engine's own synchronization.
- **Early use:** stage-1 outputs may be consumed or freed as soon as their
  call returns OK, because stage 2 reads scratch, never `outs`. Use
  `arixv_pack_inspect_modules` to map modules to jobs: group module indices
  by `(stage, block_index)` once, before issuing jobs, rather than scanning
  every module per job. Publishing before the whole pack succeeds means the
  engine must handle a later block failing.
- **Partial decode:**
  - stage-1 modules only: run only the owning stage-1 jobs;
  - any stage-2 module: run all of stage 1, then the owning stage-2 jobs.

### 3.3 Scratch

- **Needed** only when `scratch_size > 0`.
- **Capacity:** any buffer at least `scratch_size` bytes works, at any
  alignment; the size includes alignment slack.
  - When `scratch_size == 0`, `(NULL, 0)` and any non-NULL buffer are both
    accepted, which suits pooled buffers.
  - When scratch is required, a NULL buffer returns `INVALID_ARGUMENT` and
    a short buffer returns `BUFFER_TOO_SMALL`, before any access.
- **Layout:** the library derives it from validated module sizes:
  - one completion record per stage-1 block, 64-byte stride, so concurrent
    stage-1 jobs don't share a cache line;
  - then one 64-byte-aligned slice per stage-1 block, holding each member's
    bytes, ID roles and instruction frame start offsets;
  - all at offsets computed from sizes alone.

  Stage 2 reads slices in place and copies nothing per job. Frames are
  stored as 4-byte start offsets: frame i spans `[start[i], start[i + 1])`.
  Measured size is 2.25–2.31× the stage-1 decoded bytes, down from 3.25×
  with 8-byte frame records.
- **Lifetime:**
  - The input bytes must stay unchanged for the whole attempt.
  - The scratch address must not change from `arixv_pack_reset_scratch`
    until the attempt's last call returns, and only the library modifies
    scratch in that time.
  - Scratch may be freed or reused, after a reset, once every scheduled
    call has drained, including after a partial or abandoned attempt.
- **Typed reads (implementation note):** stage 2 reads words and frames
  from scratch in place. The C++ implementation relies on implicit object
  creation for trivially copyable types: C++20 P0593, applied as a defect
  report by current compilers. Where that is not acceptable, it can use
  `memcpy` loads at a small cost.

### 3.4 Dependency checking, check order, errors

- **Completion records.** After its checks, each stage-1 call clears its
  own record before writing its slice. As its last step after success, it
  writes the record: marker, header checksum, block index. Each stage-2
  call verifies every record before decoding.
- **What `DEPENDENCY_INCOMPLETE` guarantees:** within an attempt that began
  with `arixv_pack_reset_scratch` and is correctly synchronized, it reports
  any stage-1 block that has not published, whether skipped or failed.
  - Stage 2 also returns it when a published slice descriptor does not
    match the validated header. That is a limited sanity check of the
    scratch.
- **Best effort only:** records carry the header checksum, so scratch
  published for a pack with a different header is usually rejected. That
  is a stale-scratch diagnostic, not an identity check (F3).
- **Not detected:**
  - scratch from a pack with an identical header but different payloads;
  - an attempt without a reset that follows an earlier decode of the same
    pack. The reset-once-per-attempt rule exists for this case.
  - concurrent misuse, which is a data race.

  It is a diagnostic, not a synchronization or identity mechanism.
- **Check order** for every decode call:
  1. **Header-independent arguments:** `in` and `outputs` non-NULL with
     valid `struct_size`; `outs`/`caps` non-NULL; `opts` NULL or valid;
     `scratch == NULL && scratch_size != 0` is invalid. →
     `INVALID_ARGUMENT`.
  2. **Whole-header validation (F1/F2).** → `BAD_CONTAINER`,
     `UNSUPPORTED_FORMAT` (recognized pack magic, unsupported version), or
     `TRUNCATED`.
  3. **Header-dependent arguments:** `module_count` mismatch or
     `block_index` out of range → `INVALID_ARGUMENT`. Required scratch or
     own output not addressable → `TOO_LARGE`. Required scratch with
     `scratch == NULL` → `INVALID_ARGUMENT`.
  4. **Buffers:** short scratch → `BUFFER_TOO_SMALL`. For each own module,
     a NULL output with non-zero size → `INVALID_ARGUMENT`, and a short
     capacity → `BUFFER_TOO_SMALL`.
  5. **Completion records** (stage 2 only) → `DEPENDENCY_INCOMPLETE`.
  6. **Decode:** stage 1 clears its record first. Corrupt payloads return
     `BAD_CONTAINER` or `TRUNCATED`.

  Steps 1–5 write nothing. Within step 4, every own module's NULL check
  precedes every capacity check, so the error doesn't depend on module
  order.
- **On failure in step 6,** the call's own outputs are unspecified and its
  peers are unaffected. A stage-1 call's slice is also unspecified, and its
  record stays cleared. Stage 2 writes neither a slice nor a record.
- **`DEPENDENCY_INCOMPLETE` is secondary.** If stage 1 failed, report that
  status as the root cause.
- **`arixv_pack_decode`** runs steps 1–4 for every block before writing.
  After decoding starts, a failure leaves earlier blocks decoded but
  returns no per-block results.
- **Hostile input:** output never exceeds declared sizes, and work and
  allocation are bounded by validated sizes.

### 3.5 Private per-call memory

- **v1.** Each call allocates its private working state internally and
  frees it before returning: model and tracker state, the block's modules
  as copy sources, and copy tables. This follows the 1.0 OOM contract. The
  no-lock promise covers library-managed locks only, since a general
  allocator may synchronize.
- **Shape:** `C + K1 × module_count + K2 × job decoded bytes`. 1.1
  documents this shape and the measured peaks below; it does not promise
  numeric constants.
  - **The module-count term** comes from whole-header validation
    (O(module_count) tables per call) and from stage 2's per-shared-member
    descriptors and bookkeeping, about 160 bytes per stage-1 module in the
    prototype. A tiny job in a pack with many modules still pays it. The
    format limit of 2^20 modules bounds it.
  - **The job-bytes term** covers the model, the block's modules kept as
    copy sources, and copy tables. Copy tables are keyed to the decoding job
    and grow from 8 entries only with the IDs it copies, so they do not
    scale with the size of the stage-1 modules it copies from.
  - **Measured peaks** in the prototype, which 1.1 ports unchanged:
    roughly 3–5× the job's decoded bytes plus 0.3–1.6 MB (§9). Independent
    ARX2 needs about 4–6× the module size per module. Decoding goes into
    per-call vectors and then copies out; decoding directly into outputs
    would save about 1×.
  - **`arixv_pack_decode`** also keeps every stage-1 module (bytes, roles
    and frame offsets, about 1.5× their decoded bytes) in memory for the
    whole call, in place of the scratch buffer.
  - **Not a guarantee.** Measured peaks are typical values, not a bound,
    and C, K1 and K2 are not part of the 1.1 contract. Deriving fixed
    constants from implementation limits belongs with the workspace
    extension below, which needs them to size the workspace.
  - **The encoder's `max_block_bytes`** limits job size. Decoders rely on
    validated sizes, not on encoder options.
- **Planned 1.x addition, appended to the options struct.** An optional
  caller workspace (`void* workspace; size_t workspace_size;`) plus a
  per-pack `arixv_pack_job_workspace_size` query. With a workspace of that
  size, decode allocates nothing internally; engines would keep one per
  worker thread. This needs the model and tracker to allocate from a
  bounded arena. Allocator hooks are not planned.

### 3.6 Engine integration pattern (C++)

```cpp
arixv_pack_info info = { sizeof info };
if (arixv_pack_inspect(pack, size, &info) != ARIXV_STATUS_OK) return Fail();
if (!BudgetOk(info.module_count, info.total_output_size, info.scratch_size))
  return Fail();                                   // also guards 32-bit narrowing

std::vector<arixv_pack_module_info> mods(info.module_count);  // heap, not stack
if (arixv_pack_inspect_modules(pack, size, mods.data(), info.module_count) !=
    ARIXV_STATUS_OK) return Fail();
// Allocate outs[i] (uint32_t-aligned) and caps[i] = mods[i].size.
arixv_pack_outputs outputs = { sizeof outputs, info.module_count, outs, caps };
void* scratch = info.scratch_size ? AllocScratch(info.scratch_size) : nullptr;  // any alignment
if (arixv_pack_reset_scratch(pack, size, scratch, info.scratch_size) !=
    ARIXV_STATUS_OK) return Fail();                // OK no-op without stage 1

std::atomic<arixv_status> stage1_error{ARIXV_STATUS_OK}, stage2_error{ARIXV_STATUS_OK};
auto keep_first = [](std::atomic<arixv_status>& e, arixv_status st) {
  arixv_status ok = ARIXV_STATUS_OK;
  e.compare_exchange_strong(ok, st);
};
JobGroup stage1;
for (uint32_t i = 0; i < info.stage1_block_count; ++i)  // index order = largest first
  stage1.Add([&, i] {                                   // block index by value
    arixv_status st = arixv_pack_decode_stage1(pack, size, i, &outputs,
                                               scratch, info.scratch_size, nullptr);
    if (st != ARIXV_STATUS_OK) keep_first(stage1_error, st);
  });
// The only dependency; skip it when stage 1 is empty, since some job systems
// mishandle a dependency on an empty group.
JobGroup stage2 = info.stage1_block_count ? JobGroup::After(stage1) : JobGroup();
for (uint32_t j = 0; j < info.stage2_block_count; ++j)
  stage2.Add([&, j] {
    if (stage1_error != ARIXV_STATUS_OK) return;        // inputs missing: skip
    arixv_status st = arixv_pack_decode_stage2(pack, size, j, &outputs,
                                               scratch, info.scratch_size, nullptr);
    if (st != ARIXV_STATUS_OK) keep_first(stage2_error, st);
  });
stage2.Wait();                                          // then free or reuse scratch
if (stage1_error != ARIXV_STATUS_OK) return Fail(stage1_error);  // root cause
if (stage2_error != ARIXV_STATUS_OK) return Fail(stage2_error);
for (uint32_t i = 0; i < info.module_count; ++i) Upload(i, outs[i], mods[i].size);
```

Each stage keeps its own first error: a stage-2 failure must not make other
stage-2 jobs skip, and stage 2 skips only when its inputs are missing. For
early use, group the module indices by `(mods[i].stage,
mods[i].block_index)` before issuing jobs and upload a block's modules when
its call returns OK (§3.2).

A caller that does not want threads calls
`arixv_pack_decode(pack, size, &outputs, nullptr)`.

## 4. Engine decisions at decode time

The engine decides only:

- budgets, from the validated inspection;
- where outputs live;
- whether to publish modules early.

The schedule is "reset, issue stage 1, then stage 2, each in index order".

## 5. Encode API

```c
#define ARIXV_PACK_ENCODE_OPTIONS_V1_SIZE 24u

#if ARIXV_HAS_ENCODER_
enum { ARIXV_PACK_ENCODE_FAST_DECODE = 1u << 0 };

typedef struct arixv_pack_encode_options {
  uint32_t struct_size;         /* caller sets (v1: 24) */
  uint32_t flags;               /* ARIXV_PACK_ENCODE_*; other bits 0 */
  uint32_t parallelism;         /* 0 = library default (8); 1 = solid; N = target */
  uint32_t latency_budget_pct;  /* 0 = default (150); 100..100000. Encoder
                                   heuristic: target decode critical path as a
                                   percent of an estimated ideal for
                                   `parallelism` workers. Not a decode-time
                                   guarantee (see Semantics). */
  uint64_t max_block_bytes;     /* 0 = no threshold */
} arixv_pack_encode_options;

/* Worst-case output size for these module sizes and options: the sum of
 * module sizes (every block stored) plus a worst-case header of 25 + 21 *
 * count bytes in the prototype, for any legal partition. Returns 0 if
 * sizes == NULL, count == 0 or beyond F7, any size beyond F7, opts is
 * invalid, or the result overflows. The no-BUFFER_TOO_SMALL guarantee holds
 * for the same sizes, count and options. */
ARIXV_API size_t ARIXV_CALL arixv_pack_encode_bound(
    const size_t* sizes, uint32_t count,
    const arixv_pack_encode_options* opts) ARIXV_NOEXCEPT;

/* Stateless, thread-safe, single-threaded; allocates internally.
 * `modules` and `sizes` non-NULL; modules[i] may be NULL only when
 * sizes[i] == 0; modules may alias each other but not `out`. Same out /
 * *out_size / BUFFER_TOO_SMALL rules as arixv_encode. count == 0, sizes or
 * counts beyond F7, or invalid opts → INVALID_ARGUMENT. */
ARIXV_API arixv_status ARIXV_CALL arixv_pack_encode(
    const void* const* modules, const size_t* sizes, uint32_t count,
    void* out, size_t* out_size,
    const arixv_pack_encode_options* opts) ARIXV_NOEXCEPT;
#endif
```

Semantics:

- **Module identity** is the input order. Decode outputs are addressed by
  original index, whatever the block assignment and coding order.
- **Duplicates** are coded as references wherever an identical module is
  reachable: earlier in the same block's coding order, or anywhere in stage
  1. Otherwise they are coded again. The default policy keeps exact
  duplicates with their first occurrence (below). Zero-byte, header-only
  and non-SPIR-V modules are accepted, and incompressible blocks are stored
  (F6).
- **`parallelism`** is a best-effort decode-width hint, not a block count.
  - `0` is the library default, documented as such without a fixed number.
    It is 8 today, and may later choose automatically, for example the knee
    of the size/latency curve.
  - `1` requests solid, split only by `max_block_bytes`.
  - Values above the module count act as the module count.
  - The research policy (policy 2), measured in §9. Budget B is
    `latency_budget_pct / 100`, and F the floor above:
    - **Stage 1:** up to P/2 modules the whole-set plan copies most from
      (exact duplicates count toward their first occurrence), capped at a
      quarter of the modules, one per block. Only modules of at most
      (B − 1) × F may join, because stage 1 runs before stage 2. At 100%
      there is no stage 1, and sets of a few large modules get a small one
      or none.
    - **Stage-2 order:** modules follow the module they copy most from, or
      the first occurrence of an exact duplicate (depth-first over that
      forest), so related modules share a block.
    - **Stage-2 blocks:** about 2P, of roughly equal decoded bytes (by count
      when all are empty), cut nearest to the target, each in ascending
      coding order.
      - The byte target is at least the largest stage-2 module: growing a
        block up to a size already on the critical path costs no latency.
      - List scheduling bounds stage 2 by rest / P + largest block, so the
        target may also grow to B × F − largest stage-1 module − rest / P.
        Spare budget buys larger blocks. At 150% this term is about zero on
        many-module packs.
    - Packs of 1–3 modules get no stage 1.
    - With about one block per worker, measured latency ran 1.5–2× the
      modelled critical path; two per worker leaves the scheduler slack.
- **`latency_budget_pct`** is the developer's compression-versus-decode-
  speed preference. It is an **encoder heuristic input**: it shapes how the
  encoder partitions the pack, and nothing at decode time reads or enforces
  it.
  - **Meaning:** the decode critical path the encoder should aim for, as a
    percentage of an estimated ideal parallel decode on `parallelism`
    workers. The ideal is the floor F = max(largest module, total bytes /
    P), with decode time modelled as proportional to decoded bytes. 100%
    asks for the fastest layout; 150% is the default; larger values allow
    larger shared stage-1 modules and then larger stage-2 blocks, moving
    toward solid while blocks still decode in parallel.
  - **What it guarantees:** only the structural guarantees below, which
    hold for every budget. Every budget produces a valid, at most
    two-stage pack within `arixv_pack_encode_bound`. Output is
    deterministic for the same inputs, options and library version. At
    100% there is no stage 1. `parallelism = 1` ignores the budget and
    produces a solid pack.
  - **What it does not guarantee:**
    - **Any decode time.** The model ignores per-instruction cost
      differences, cache and memory effects, all-core clock drop, and
      allocator and scheduler behaviour. Measured latency relative to
      independent ARX2 roughly tracks the budget (§9: about 0.9–1.4× at
      100%, 1.4–2.2× at 200%, 3.2–5.8× at 500%), but individual packs can
      exceed it.
    - **Reachability.** Module granularity sets a floor: a single large
      module is never split, so low budgets can be unreachable, and
      sets of a few large modules may not parallelize much at any budget.
    - **Monotone size.** A larger budget can change which modules join
      stage 1. The pack is usually smaller, but not always: in §9, Xenia
      is larger at 200% than at 150%.
    - **Any particular block count or layout,** or stability of the layout
      across library versions. The mapping from budget to layout is an
      encoder policy and may change, like the `parallelism` default.
    - **Hardware transfer.** The budget is relative to `parallelism`
      workers decoding this pack alone. Fewer workers, other concurrent
      work, or a different machine change the absolute times, though the
      job graph still works with any number of workers.
  - **Range:** 0 (the default, 150%) or 100..100000; other values →
    `INVALID_ARGUMENT`.
  - **Choosing a value:** measure on the target hardware. Large related
    sets typically gain little beyond 150–300%, and few-module packs with
    large shared modules gain most at 200% (§9).
- **`max_block_bytes`** is a threshold on each block's decoded bytes,
  counting duplicates, in both stages. A single larger module forms its own
  block. It shapes per-job latency and memory, but decoders do not rely on
  it.
- **Guarantees.**
  - At most two stages, with blocks in largest-first order (F4).
  - Output fits `arixv_pack_encode_bound`.
  - Output is byte-identical for identical inputs, options and library
    version, on any host.
    - **Planner and block assignment:** integer only. Costs are Q16
      fixed-point bits, `log2` is an exact integer routine, the budget is
      integer percent and bytes, and ties break on module index.
    - **Prototype evidence:** 32 ARXP and native-planned AXST streams on
      eight sets are byte-identical across gcc -O3, gcc -O0, and clang -O3
      `-march=native -ffast-math`.
    - **Scope:** the implementation requires a little-endian host, as before.
- **Cost.** About 2–3× a solid encode: a whole-set plan (2.5 s for
  Wicked's 21.7 MB) plus the block encodes. Encode is synchronous, and
  asset pipelines parallelize across packs.

## 6. Engine decisions at encode time

1. **Pack boundaries.** The dominant decision: which related shaders form
   one pack. Larger related sets gain most.
2. **`parallelism`.** About the number of loader workers decoding while
   something waits. The default suits most uses.
3. **Optional:** `max_block_bytes`, and `FAST_DECODE`, which measured 5–20%
   faster decode for about 0.5–2% larger output.

## 7. Open questions

1. **Encoder policy (latency-aware, done).**
   - The default budget (150%) limits stage-1 modules to F/2. Blender
     improves from 9.2 to about 6 ms at 16 workers.
   - Few-module packs whose hubs are large, like mobile_nerf, decode at
     ARX2 speed at that budget but barely smaller than ARX2.
   - `latency_budget_pct` (§5) lets the developer choose a parallel point
     between that and solid. On mobile_nerf, 200% gives 0.038 at 1.5 ms,
     against ARX2's 0.069 at 0.8 ms and solid's 0.031 at 4.2 ms (§9).
   - A reuse-based hub exemption was tried and rejected: it could not
     separate mobile_nerf's hubs from Blender's large modules.
   - Automatic choice of P is deferred (decided 2026-11-22): `parallelism =
     0` maps to 8 for the foreseeable future.
2. **Scratch frame representation (decided 2026-11-22: start offsets,
   done).** 2.25× the stage-1 bytes; the bitstream is unaffected.
3. **The 1.x workspace extension: deferred (decided 2026-11-22).** It is
   feasible: ModelState already uses a bump arena. The workspace query must
   cover header tables, stage-2 shared descriptors, copy tables and decode
   temporaries, and its result must be verified with an instrumented
   allocator.
4. **Measured versus modelled latency at 16 workers.** Measured latency is
   about 1.1–1.4× the modelled critical path with about two blocks per
   worker, and more with fewer. Separating all-core clock, cache and
   allocator effects needs hardware counters, which are unavailable on the
   measurement host.

## 8. Answers to earlier open questions

- **Outputs:** a descriptor struct with a `struct_size` field, adopted.
- **Scratch reset:** required once per attempt, as the one rule; zero-filled
  scratch no longer counts as reset.
- **Duplicate flag:** none. It isn't actionable without an alias target.
- **Dependency subsets:** a full join in v1. Later formats would add
  predecessor metadata and new entry points, or be rejected by these calls.

## 9. Prototype measurements

Measured with the research mirror (then named AXSB; its header v3 differs from ARXP v1 only in the magic and the codec-flags encoding), default policy 2,
`arixv_pack_api_bench`) on an idle Ryzen 9 5950X. Jobs ran on a persistent
worker pool in index order. Times are best of 7 (best of 4 for Wicked), and
the policies were interleaved per pack. All outputs are verified. Raw data:
`build/gate4/pack-api/final.md`. Policy sweeps: `policy2*.md`. Earlier
runs: `v5.md`, `paired.jsonl` (policy 0 against 1), `bench.md` (v3 header,
memory).

| Pack | ARX2 ratio | Solid (P=1) | Policy 1, P=16: ratio / 16w ms | Default (policy 2), P=8: ratio / 8w ms (ARX2) | Default (policy 2), P=16: ratio / 16w ms (ARX2) | Scratch / stage-1 bytes |
|---|---|---|---|---|---|---|
| Dota 2 | 0.1490 | 0.0403 | 0.0538 / 2.48 | 0.0514 / 3.39 (2.55) | 0.0566 / 2.34 (1.68) | 2.26 |
| Dota 2 (#unique) | 0.1490 | 0.0463 | 0.0573 / 2.41 | 0.0588 / 3.07 (2.24) | 0.0566 / 2.32 (1.54) | 2.26 |
| Blender | 0.1331 | 0.0966 | 0.1111 / 9.20 | 0.1178 / 6.35 (3.83) | 0.1178 / 6.29 (3.85) | 2.25 |
| mobile_nerf | 0.0688 | 0.0309 | 0.0378 / 1.52 | 0.0653 / 0.82 (0.66) | 0.0653 / 0.88 (0.83) | 2.31 |
| 0 A.D. | 0.1095 | 0.0346 | 0.0382 / 4.49 | 0.0356 / 5.20 (3.33) | 0.0398 / 2.79 (2.19) | 2.26 |
| Sascha glsl | 0.1491 | 0.0735 | 0.0839 / 2.55 | 0.0825 / 3.34 (2.36) | 0.0839 / 2.40 (1.81) | 2.27 |
| Xenia | 0.1802 | 0.0386 | 0.0812 / 2.87 | 0.0625 / 4.58 (3.07) | 0.0709 / 3.48 (2.34) | 2.26 |
| Wicked | 0.1113 | 0.0352 | 0.0394 / 40.99 | 0.0373 / 75.30 (50.81) | 0.0387 / 43.66 (27.76) | 2.25 |

Findings:

- **Large sets (Dota 2, 0 A.D., Wicked, Sascha) at P=16, against policy
  1:**
  - The default's latency is better or within single-run variance: 0 A.D.
    2.79 against 4.49 ms, Dota 2 2.34 against 2.48 ms, Wicked 43.7 against
    41.0 ms.
  - Ratio is within about 5%: Dota 2 +5%, 0 A.D. +4%, Wicked −2%,
    Sascha equal.
  - Packs are 1.8–2.9× smaller than ARX2, at 1.3–1.6× ARX2's 16-worker
    latency.
- **Xenia improves:** 0.0812 → 0.0709 at P=16.
- **Blender:** 9.2 → 6.3 ms at 16 workers, for 6% more bytes (0.1178
  against 0.1111).
- **mobile_nerf:** ARX2-level latency, at almost ARX2-level size (§7,
  Q1).
- **Measured against modelled latency:** about 1.1–1.4× the modelled
  critical path at two blocks per worker, and 1.5–2× at one block per
  worker. That is why the default uses about two.
- **Scratch** is 2.25–2.31× the stage-1 bytes with start offsets.
- **Header and memory** are unchanged from v5: solid packs are within 0.6%
  of raw AXST; whole-header validation costs at most 4.5 µs per call; the
  worst per-job peak is 3–7× the job's bytes plus a few hundred KB.

### Latency budget

Same setup. P=16 and 16 workers, ratio / measured ms; best of 7 (best of 4
for Wicked). Raw data, including P=8: `build/gate4/pack-api/budget.md`.

| Pack (ARX2 ratio / 16w ms) | 100% | 125% | 150% (default) | 200% | 300% | 500% | Solid (1 thread) |
|---|---|---|---|---|---|---|---|
| mobile_nerf (0.0688 / 0.77) | 0.0645 / 0.79 | 0.0653 / 0.87 | 0.0654 / 0.86 | 0.0378 / 1.49 | 0.0377 / 2.05 | 0.0377 / 3.20 | 0.0309 / 4.18 |
| Blender (0.1331 / 3.98) | 0.1248 / 4.50 | 0.1215 / 5.43 | 0.1178 / 6.55 | 0.1111 / 6.73 | 0.1110 / 12.67 | 0.1110 / 12.67 | 0.0966 / 16.61 |
| Dota 2 (0.1490 / 1.71) | 0.0981 / 2.13 | 0.0674 / 2.27 | 0.0566 / 2.02 | 0.0519 / 2.72 | 0.0507 / 4.48 | 0.0488 / 6.19 | 0.0403 / 19.54 |
| 0 A.D. (0.1095 / 2.82) | 0.0455 / 3.49 | 0.0398 / 3.79 | 0.0398 / 3.84 | 0.0394 / 4.08 | 0.0352 / 4.88 | 0.0363 / 10.10 | 0.0346 / 35.51 |
| Xenia (0.1802 / 1.89) | 0.1079 / 2.05 | 0.0880 / 2.79 | 0.0709 / 3.05 | 0.0770 / 3.28 | 0.0728 / 4.36 | 0.0713 / 7.33 | 0.0386 / 22.63 |
| Wicked (0.1113 / 30.68) | 0.0523 / 33.00 | 0.0387 / 42.17 | 0.0387 / 43.41 | 0.0391 / 59.17 | 0.0377 / 76.74 | 0.0383 / 136.78 | 0.0352 / 441.60 |

- **Measured latency relative to independent ARX2 tracks the budget:**

  | Budget | Measured / ARX2 |
  |---|---|
  | 100% | 0.9–1.4× |
  | 125–150% | 1.0–1.7× |
  | 200% | 1.4–2.2× |
  | 300% | 2.0–3.1× |
  | 500% | 3.2–5.8× |

  The worst overshoot is Wicked at P=8 and 500%, which is within the
  model's error.
- **150% reproduces the previous default:** identical sizes, apart from
  one pack that differs by one byte.
- **mobile_nerf** reaches nearly its best parallel ratio by 200% (0.038 at
  1.5 ms). Higher budgets add latency for nothing, because its blocks
  cannot share more.
- **Large sets** keep gaining up to 300%: 0 A.D. 0.0352 at 4.9 ms (solid
  0.0346 single-threaded at 35.5 ms), Dota 2 0.0507, Wicked 0.0377.
- **At 100%,** without stage 1, packs are 1.07× (mobile_nerf, Blender) to
  2.4× (0 A.D.) smaller than ARX2.
- **Size is not strictly monotone in the budget,** because a larger budget
  also changes which modules join stage 1:
  - Xenia is 0.0709 at 150% but 0.0770 at 200%;
  - Wicked and 0 A.D. tick up slightly at some higher budgets.

  Making the stage-1 choice budget-monotone (for example, never dropping a
  module chosen at a lower budget) is a policy refinement left open.

## 10. Decisions record

| Topic | Decision | Source |
|---|---|---|
| Stage naming | `stage1` / `stage2` | v1 review (Neon); user |
| Session vs stateless | Stateless; whole-header validation in every call (measured under 1.5% of decode work) | User; v2 reviews; §9 |
| Ownership | `(block, position)` per module, unique slots, non-empty blocks, prefix-sum payloads | v2 reviews |
| Inspection | Validates the whole header; sums are consistent; budgets come from validated values | v2 reviews (Sol) |
| Outputs | Descriptor with `struct_size`; a call touches only its own entries | v2 reviews (Neon) |
| Scratch | Library-derived layout, not in the format; capacity semantics; no-op without stage 1; 64-byte records; reset once per attempt | v2 reviews |
| Dependency errors | Diagnostic scope stated precisely | v2 reviews |
| Private memory | Internal allocation; shape `C + K1 × module_count + K2 × job bytes` with measured peaks, no numeric constants promised in 1.1; job-keyed copy tables; optional workspace in 1.x; no allocator hooks | User; v2 review (Neon); §9 |
| Scheduling info | None; largest-first order, documented | User |
| Default parallelism | Documented library default: 0 maps to 8 for the foreseeable future; automatic selection deferred | User |
| Flags | Separate info and encode namespaces | v2 reviews (Neon) |
| `max_block_bytes` | Decoded-byte threshold, both stages, singleton exception; not a decoder safety bound | v2 reviews |
| Determinism | Integer-cost planner (Q16 bits, integer log2), done; byte-identical across compilers and flags | v2 reviews; user |
| Encode job API | None in v1 | v1 reviews |
| Header format | Compact varint header with implicit coding order; bare per-block payloads (F5) | v4 measurement |
| Default policy | Clustered, byte-balanced stage 2; stage 1 capped at a quarter of the modules; duplicates clustered with their first occurrence | v4 measurement; v4 reviews |
| Stored blocks | Per-block stored bit (exact length); encode bound assumes all stored | v4 reviews |
| Varints and limits | Canonical LEB128; 1.0 short-input status order; explicit limits (F7) | v4 reviews |
| Dependency diagnostic | Guarantees missing publication within a reset attempt only; header match is best effort | v4 reviews |
| Memory bound | `C + K1 × module_count + K2 × job bytes` as a documented shape; constants deferred to the workspace extension | v4 reviews; implementation |
| Scratch frames | 4-byte start offsets (2.25× stage-1 bytes) | User (§7 Q2) |
| Workspace extension | Deferred | User (§7 Q3) |
| Latency-aware stage 1 | Policy 2: stage-1 modules at most (B − 1) × F; stage-2 target at least the largest stage-2 module, growing with spare budget; about 2 blocks per worker | User; §9 |
| Size/speed dial | `latency_budget_pct` encode option (default 150, 100..100000), in the former reserved field | User |
| Per-call arguments | Flat `(in, in_size, ..., scratch, scratch_size, opts)`; a "pack view" struct rejected: it would drop stage 2's `const` scratch, break the `(in, in_size)` prefix shared with `arixv.h`, look like a session, and save only a few arguments that engines wrap anyway | Snippet reviews (Neon; Sol for adopting); user |
| Inspection calls | `inspect` and `inspect_modules` stay separate: budget-first callers call twice anyway, and merging would break "output untouched on error" | Snippet reviews (Neon; Sol for merging); user |
| Scratch reset | Explicit call, an OK no-op without stage 1; must follow the drain of any earlier attempt on the same buffer | Snippet reviews (both); user |
| Module stage | Explicit `stage` byte (`ARIXV_PACK_STAGE_1/2`) plus `uint16_t flags`, same 16 bytes | Snippet reviews (Neon); user |
| Integration pattern | Per-stage first error; budget count, outputs and scratch; publish after success by default; early use groups modules by block once | Snippet reviews (both); user |
| Convenience decode | `arixv_pack_decode` reads stage-1 modules from memory, no scratch | Implementation |
| Version | Library 1.1.0; new symbols in ELF version node `ARIXV_1.1` | Implementation |
