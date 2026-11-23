# ARXP Format Specification (v1)

`ARXP` is ARIX-V's multi-module shader-pack container. It holds an ordered
set of separate modules (SPIR-V or arbitrary bytes) and decodes them in at
most two stages of independent blocks, scheduled by the caller's job
system. It is additive to the frozen single-module [`ARX2`](format-arx2.md)
format.

**Status: v1, shipped with library 1.1.** The layout below is implemented,
golden-tested and fuzzed, and is frozen once 1.1 is tagged. The reference
implementation is:
- [`src/arixv_pack.cpp`](../src/arixv_pack.cpp) for the container and the
  two-stage decode;
- [`src/pack_block.inc`](../src/pack_block.inc) for block coding;
- [`src/arixv_pack_encode.cpp`](../src/arixv_pack_encode.cpp) for the
  encoder's layout policy.

The public API is `arixv_pack_*` in
[`include/arixv/arixv.h`](../include/arixv/arixv.h); its design record is
[`docs/development/multi-module-pack-api.md`](development/multi-module-pack-api.md).

## 1. Format identity and freeze boundary

An `ARXP` v1 stream is normatively defined by:

1. the container layout and block-coding structure in this document;
2. the reference implementation above;
3. the frozen `ARX2` model, entropy coders, wire constants, grammar tables
   and string primer, exactly as listed in [`format-arx2.md`](format-arx2.md)
   §1. ARXP block coding reuses that model unchanged. It is driven through
   `CodeModule`'s hook interface in [`src/model.cpp`](../src/model.cpp),
   whose default (`NoModuleHook`) is the ARX2 coder itself; the block hook
   is in [`src/pack_block.inc`](../src/pack_block.inc).

Any change that alters how pack bits are interpreted needs a new version
byte (§2). A change to the shared ARX2 model also needs a new ARX2 magic,
so it cannot alter ARXP v1 silently.

## 2. Container layout

```text
offset 0   4 B   magic         "ARXP" (0x41 0x52 0x58 0x50)
       4   1 B   version       1
       5   1 B   pack_flags    bit 0: implicit coding order; bits 1-7: 0
       6   1 B   codec_flags   bit 0: WARM; bit 1: FAST_DECODE; bits 2-7: 0
       7   1 B   reserved      0
       8   var   module_count  M
           var   stage1_blocks S1
           var   stage2_blocks S2
           M x   { var size, var block }           original module order
           M x   { var position }                  only if bit 0 of pack_flags is 0
     (S1+S2) x   { var (payload_length << 1 | stored) }   block order
           8 B   checksum      FNV-1a 64 of all preceding bytes, little-endian
           ...   payloads      one per block, in block order, contiguous
```

- **`var`** is canonical unsigned LEB128: 7 bits per byte, least
  significant group first, high bit set on every byte but the last, at
  most 64 bits, and the shortest form only. A non-minimal or overflowing
  encoding is `BAD_CONTAINER`; input ending inside one is `TRUNCATED`.
- **Blocks** are numbered globally: `[0, S1)` are stage 1 and `[S1, S1+S2)`
  are stage 2. The API addresses them by stage and index within the stage.
- **`block`** is the module's global block number. **`position`** is its
  coding position within that block. With implicit order, positions are the
  ascending order of original module indices within each block.
- **`stored`** is bit 0 of the length field. A stored block's payload is
  its modules' bytes concatenated in coding order. A coded block's payload
  is one block-coder stream (§3).
- **Payload offsets** are the header length plus the prefix sums of the
  payload lengths. The last payload ends exactly at the end of the input.

### 2.1 Validation

Every decode call validates the whole header before using it, in this
order:

1. **Magic and version:**
   - fewer than 4 bytes, or a first 4 bytes other than `"ARXP"` (an ARX2
     blob included) → `BAD_CONTAINER`;
   - `"ARXP"` with no version byte → `TRUNCATED`;
   - version ≠ 1 → `UNSUPPORTED_FORMAT`;
   - fewer than 8 bytes → `TRUNCATED`.
2. **Flags and reserved byte:** reserved bits must be 0 and byte 7 must be
   0 → `BAD_CONTAINER`.
3. **Counts:**
   - `1 ≤ M ≤ 2^20`, `S2 ≥ 1`, `S1 + S2 ≤ M` → `BAD_CONTAINER`;
   - the input must hold every mandatory header byte (two varints per
     module, three with explicit order, one per block, and the checksum)
     before any table is allocated → `TRUNCATED`.
4. **Module table:**
   - `block < S1 + S2`, and `size ≤ 2^32 − 1` → `BAD_CONTAINER`;
   - every block owns at least one module;
   - explicit positions must be in range and not repeated, so ownership is
     a partition.
5. **Checksum** → `BAD_CONTAINER`. It detects header corruption only; it is
   not an identity or authentication mechanism.
6. **Payloads:**
   - lengths must fit the input → `TRUNCATED`;
   - a stored payload must equal its modules' total size → `BAD_CONTAINER`;
   - the payloads must end exactly at the input end → `BAD_CONTAINER`.

The frozen `ARX2` decoder (`arixv_decode`) reports an ARXP stream as
`UNSUPPORTED_FORMAT`, because `"ARX?"` is reserved for successor formats
(`format-arx2.md` §2.1).

## 3. Block coding

Each coded block is one continuous ARX2 dual stream: the forward range
coder and backward rANS, as in `format-arx2.md` §3. The stream must be
consumed exactly. The block's modules are coded in coding order.

### 3.1 Member index space

Within a block, members are addressed by a global index `g`:

- **Stage-1 blocks** have no shared members. Their own modules are
  `g = 0, 1, …` in coding order.
- **Stage-2 blocks** first see every stage-1 module as a shared member:
  stage-1 blocks in block order, each block's modules in coding order.
  With `N` stage-1 modules, a stage-2 block's own modules are
  `g = N, N + 1, …`. Shared members are read-only, and each stage-2 block
  decodes independently of the others.

A member is a **copy source** if all of these hold:

- it was coded with the structural model, not as a duplicate or with the
  byte model;
- it is not byte-swapped;
- it has at least one instruction.

Modules of stored blocks are never copy sources. Any member, shared or
local, can be an **exact-duplicate source**.

### 3.2 Per member

For member `g`:

1. **Duplicate flag** (if `g > 0`). If set, an Elias-γ distance
   `d ∈ [1, g]` follows (§3.4), and the module is a byte copy of member
   `g − d`, whose size must equal its declared size.
2. **Otherwise, a structural flag.** If it is clear, the module is coded
   with the ARX2 mode-1 byte model (`CodeBytePayload`). If it is set, the
   declared size must be at least 20 and a multiple of 4, and the module is
   coded with the ARX2 structural model (§3.3).

### 3.3 Structural members

A structural member starts from a fresh model. With **WARM**, it instead
starts from the previous structurally coded member of the same block,
carrying the allowlisted statistics families:
- opcode models;
- word-count models;
- value- and bit-enum models.

The values and adaptation ages carry over; everything else starts fresh
(`pack_transfer::NextModel` in `src/pack_block.inc`). Duplicate and
byte-model members don't break the chain, and the first structural member
of each block always starts fresh.

The module is then coded by `CodeModule` with the block hook, in four
steps:

1. **Header.** The module header is coded as in ARX2.
2. **Copy signalling.** Before each instruction, and only when the target
   is not byte-swapped and some member `[0, g)` is a copy source, one copy
   flag follows. It has two contexts, chosen by whether the previous
   instruction of this module was copied. If the flag is clear, the
   instruction is coded as in ARX2. If it is set, a copy follows:
   - **Base and position:** if the previous copy of this module left a
     base with a next instruction, a **rep** flag says whether to continue
     there. Otherwise, or if the flag is clear, an Elias-γ distance gives
     base `g − d`, which must be a copy source. Then `BitWidth(n − 1)`
     direct bits give the start instruction `pos < n`, where `n` is the
     base's instruction count.
   - **Length:** Elias-γ, with separate models for rep and new copies,
     `1 ≤ len ≤ n − pos`.
   - **Words:** each copied instruction takes the base instruction's
     words. Every ID word (role 1, a result ID; or role 2, an ID use, as
     the model's operand categories classify it) is restored through that
     target–base pair's correspondence map and running delta:
     - **map hit:** if the map has the base ID, a role-specific bit says
       whether to reuse the mapped target ID;
     - **delta hit:** otherwise, a bit (by role and whether a map entry
       existed) says whether the target ID is the base ID plus the running
       delta;
     - **explicit:** otherwise, an Elias-γ zigzag offset from that
       prediction follows (role-specific models).

     The map and the delta are then updated. They start empty and zero
     for each target module.
   - **Train-through:** each copied instruction then advances the model as
     if it had been coded, using the encoder path on the known words.
     Normally this updates probabilities and all tracker state. With
     **FAST_DECODE** it updates tracker and ID state only, leaving
     probability tables untouched. The instruction must frame to exactly
     the base instruction's word count.
3. **ID-role recording.** The coder records each ID word's role while
   coding, so later members can copy from this one.
4. **Tail.** The bound and the tail are coded as in ARX2.

### 3.4 Shared primitives

- **Elias-γ** for `v ≥ 1`:
  - the bit length `L` in unary: bit `i` (from 1) is 1 while `L > i`, one
    adaptive model per `i`, at most 32;
  - then the `L − 1` bits below the leading 1, most significant first. The
    first two use adaptive models per `(L, depth)`; the rest are direct
    bits.
- **Zigzag** maps `x` to `(x << 1) ^ (x >> 31)`, with arithmetic shift on
  the 32-bit two's complement value.
- **Signal models** are per block and start at the default initial state
  of the coder's adaptive bit and γ models:
  - duplicate and structural flags;
  - copy flag (2 contexts);
  - rep flag;
  - map hit (by role);
  - delta hit (by role × had-map-entry);
  - γ models for the duplicate distance, base distance, rep length, new
    length and explicit ID offsets (by role).

## 4. Decoding model and scratch

- **Order:** stage-1 blocks decode independently, then every stage-2 block
  decodes independently. Each stage-2 block needs all stage-1 modules and
  their instruction frames and ID roles.
- **Scratch:** how a decoder shares that data is an implementation matter,
  not part of the format. The reference decoder writes it into a
  caller-owned scratch buffer whose layout is derived from module sizes.
- **Partial decode:**
  - stage-1 modules need only their own blocks;
  - any stage-2 module needs all of stage 1 plus its own block.

## 5. Limits

| Limit | Value |
|---|---|
| Modules per pack | 1 … 2^20 |
| Module size | 0 … 2^32 − 1 bytes (and within the platform's addressable span, `PTRDIFF_MAX`) |
| Blocks | 1 … module count; stage 2 has at least one |
| Encoded size bound | sum of module sizes + 25 + 21 × module count (every block stored, worst-case header) |

Under these limits all header and scratch arithmetic fits in 64 bits. A
pack that exceeds the platform's addressable span decodes with
`TOO_LARGE` where the API applies, while inspection still reports the full
sizes.

## 6. Compatibility and determinism

- **Decoding is deterministic.** Every valid ARXP v1 stream decodes to the
  same bytes on every supported platform, as ARX2 does.
- **Encoding is deterministic:** byte-identical output for the same
  inputs, options and library version on any host.
  - The planner and block assignment use integer arithmetic only.
  - Verified with gcc and clang, at -O0 and -O3, including
    `-march=native -ffast-math`.
- **The encoder's layout policy is not part of the format.** Block
  assignment, the latency budget and the parallelism default may change
  between library versions; streams from any version decode with any v1
  decoder.
- **Golden vectors:** [`tests/golden/pack/`](../tests/golden/pack/) holds
  eight ARXP v1 streams, with expectations in `expected.inc`:
  - solid;
  - two-stage;
  - plain blocks;
  - explicit coding order;
  - FAST_DECODE;
  - cold statistics;
  - mixed content with duplicates and header-only modules;
  - stored noise.

  `pack_test --golden-dir tests/golden` checks that each stream decodes
  through both the two-stage and convenience paths, and that re-encoding
  the deterministic inputs reproduces it byte for byte. `golden_test` also
  decodes every stream through the shipped library (including decoder-only
  and shared builds) and, with the encoder, re-encodes those expressible
  through the public options.
- **Host requirement:** the reference implementation currently requires a
  little-endian host.
