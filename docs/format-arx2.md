# ARX2 Format Specification (v1.0)

`ARX2` is the frozen v1.0 single-module container and bitstream format for
ARIX-V. Every ARIX-V blob is self-contained, carries its own 4-byte versioned
magic, and decodes without external dictionaries or corpus-trained tables.

## 1. Format Identity & Freeze Boundary

An `ARX2` bitstream is normatively defined by the combination of:

1. The container framing and mode definitions in this document.
2. The tagged v1.0 reference decoder implementation:
   [`src/arixv.cpp`](../src/arixv.cpp), [`src/container.h`](../src/container.h),
   [`src/model.cpp`](../src/model.cpp), [`src/prob.h`](../src/prob.h),
   [`src/range_coder.h`](../src/range_coder.h),
   [`src/symbol_coder.h`](../src/symbol_coder.h),
   [`src/tracker.h`](../src/tracker.h) / [`src/tracker.cpp`](../src/tracker.cpp),
   and [`src/operand_cursor.h`](../src/operand_cursor.h) /
   [`src/operand_cursor.cpp`](../src/operand_cursor.cpp).
3. The frozen entropy coder and structural model wire constants in
   [`src/tuning.h`](../src/tuning.h) (standard library builds reject `-D`
   overrides of any `ARIXV_K*` constant at compile time; experimental builds
   with `-DARIXV_RESEARCH_TUNING` emit the distinct magic `"ARXR"` so they
   cannot masquerade as `ARX2`).
4. The frozen SPIR-V grammar tables, cold-start CDF priors, and string primer
   generated into [`src/grammar_tables.cpp`](../src/grammar_tables.cpp) by
   [`scripts/gen_grammar_tables.py`](../scripts/gen_grammar_tables.py) from the
   pinned Khronos `SPIRV-Headers` revision recorded in
   [`deps/grammar-sources.lock.json`](../deps/grammar-sources.lock.json):
   - Repository: `https://github.com/KhronosGroup/SPIRV-Headers.git`
   - Revision: `02c0394e57af6dfdda7f68973df6aa20fc3f5def`

Any change to container framing, wire constants, grammar-derived model tables,
string primer contents, or adaptive model state transitions that can alter how
bits are interpreted **must** allocate a new 4-byte magic (for example, `ARX3`
or `ARXB`). Updating `SPIRV-Headers` without changing the magic is forbidden
because enumerant ordinals, operand-kind indices, the string primer, and
cold-start priors directly affect the `ARX2` bitstream.

## 2. Container Header Layout

All multi-byte integers in the container header are little-endian. A valid
`ARX2` header is **6 to 10 bytes** long:

| Offset | Size | Field | Description |
|---|---|---|---|
| `0` | 4 B | `magic` | ASCII `"ARX2"` (`0x41 0x52 0x58 0x32`, little-endian 4CC `0x32585241`) |
| `4` | 1 B | `mode` | Payload mode: `0` (`stored`), `1` (`coded`), or `2` (`spirv`) |
| `5` | 1–5 B | `header_size` | Canonical compact unsigned 32-bit size (see §2.2) |
| `6..10` | variable | `payload` | Mode-specific payload extending to the exact end of the input span |

### 2.1 Magic & Mode Validation

- Inputs shorter than 4 bytes or whose first 3 bytes are not `"ARX"` are
  rejected with `ARIXV_STATUS_BAD_CONTAINER`.
- Inputs starting with `"ARX"` whose 4th byte is not `'2'` are rejected with
  `ARIXV_STATUS_UNSUPPORTED_FORMAT` (reserving `ARX?` for future single-module,
  multi-module, or experimental research container versions).
- Inputs with `size == 4` (missing mode byte) or a truncated `header_size` are
  rejected with `ARIXV_STATUS_TRUNCATED`.
- Any `mode` value other than `0`, `1`, or `2` is rejected with
  `ARIXV_STATUS_BAD_CONTAINER`.

### 2.2 Canonical Compact `uint32` Encoding (`header_size`)

Let `b0` be the first byte of `header_size`:

| First byte `b0` | Total bytes | Value range | Formula |
|---|---|---|---|
| `0x00..0xBF` | 1 | `0 .. 191` | `b0` |
| `0xC0..0xFB` | 2 | `192 .. 15,551` | `192 + ((b0 - 0xC0) << 8) + b1` |
| `0xFC` | 3 | `15,552 .. 81,087` | `15,552 + u16_le(b1..b2)` |
| `0xFD` | 4 | `81,088 .. 16,858,303` | `81,088 + u24_le(b1..b3)` |
| `0xFE` | 5 | `16,858,304 .. 4,294,967,295` | `u32_le(b1..b4)` |
| `0xFF` | — | invalid | Rejected (`ARIXV_STATUS_BAD_CONTAINER`) |

For `b0 == 0xFE`, values `< 16,858,304` are non-canonical and **must** be
rejected with `ARIXV_STATUS_BAD_CONTAINER`.

### 2.3 Declared Uncompressed Size

- For `mode == 0` (`ARIXV_MODE_STORED`) and `mode == 1` (`ARIXV_MODE_CODED`):
  `uncompressed_size = header_size` (bytes).
- For `mode == 2` (`ARIXV_MODE_SPIRV`):
  `header_size` encodes the number of 32-bit SPIR-V words **after** the 20-byte
  SPIR-V module header:
  - `header_size` must satisfy `header_size <= (UINT32_MAX - 20) / 4`
    (`1,073,741,818`); larger values are rejected with
    `ARIXV_STATUS_BAD_CONTAINER`.
  - `uncompressed_size = 20 + 4 * header_size` (bytes).

## 3. Payload Modes & Dual-Stream Framing

Compressed payloads (`mode == 1` and `mode == 2`) share ARIX-V's two-sided
entropy layout (`BitEncoder` / `BitDecoder` in [`src/prob.h`](../src/prob.h)):

- **Front (forward) stream — binary range coder (`RangeEncoder` / `RangeDecoder`):**
  12-bit adaptive binary probabilities, 24-bit renormalization threshold
  (`kRangeTop = 1 << 24`), and adaptive minimal flush emitting **0 bytes**
  (when no binary bits were coded), **2 bytes** (`range >= 0x01FFFFFF`), or
  **3 bytes** (`range < 0x01FFFFFF`). Decoded forward from byte `0` of the
  payload.
- **Back (backward) stream — 16-way adaptive rANS symbol coder (`RansEncoder` / `RansDecoder`):**
  15-bit CDF scale (`SymbolCdf`, up to 16 symbols per table), 16-bit
  renormalization words appended after the range-coded prefix, terminated by a
  3- or 4-byte final state offset (`bit 7` of the final payload byte selects 3
  vs. 4 bytes). Decoded backward from the end of the payload.
- **Exact tiling invariant (`ConsumedExactly()`):** On successful decode,
  `range.consumed_size() + rans.consumed_size() == payload_size` and the rANS
  decoder finishes cleanly (`FinishedCleanly()`). Any mismatch, unconsumed gap,
  or trailing bytes return `ARIXV_STATUS_BAD_CONTAINER`.

### 3.1 Mode `0`: Stored (`ARIXV_MODE_STORED`)

The payload contains the exact `uncompressed_size` raw bytes verbatim.

- If `payload_size < uncompressed_size`, `arixv_decode` fails with
  `ARIXV_STATUS_TRUNCATED` (including on `out == NULL` size queries).
- If `payload_size > uncompressed_size`, `arixv_decode` fails with
  `ARIXV_STATUS_BAD_CONTAINER` (no trailing bytes allowed; also checked on
  `out == NULL` size queries).

### 3.2 Mode `1`: Generic Byte-Coded (`ARIXV_MODE_CODED`)

Used for arbitrary non-SPIR-V byte streams (or SPIR-V inputs ineligible for
mode `2`). Each output byte is coded via an order-1 previous-byte two-nibble
adaptive model (`ByteModel`, `LazyLeafTree8`): one 16-way rANS symbol for the
high nibble followed by one 16-way rANS symbol for the low nibble.

- Mode `1` emits **no** binary range-coder decisions, so the forward range-coder
  prefix is 0 bytes (`RangeEncoder::Flush()` emits 0 bytes) and the entire
  payload is the backward-decoded rANS symbol stream (or 0 bytes when
  `uncompressed_size == 0`).
- **Error contract:** Because mode `1` uses only the backward rANS stream,
  running past the start of the payload, corrupt state, or unconsumed bytes
  (`!ConsumedExactly()`) return `ARIXV_STATUS_BAD_CONTAINER`.

### 3.3 Mode `2`: SPIR-V Structural Model (`ARIXV_MODE_SPIRV`)

Eligible inputs satisfy `in_size >= 20`, `in_size % 4 == 0`, and a first 32-bit
word equal to either the little-endian SPIR-V magic (`0x07230203`) or the
byte-swapped big-endian SPIR-V magic (`0x03022307`).

The payload uses the dual-stream range + rANS layout described above to code:

1. **Module header (5 words):** endianness flag (1 bit; preserves big-endian
   modules byte-for-byte), version major/minor (cold-start priors from the
   pinned grammar), generator ID/version (registered tool count from
   `spir-v.xml`), ID bound, and schema word.
2. **Instruction stream:** section-tracked opcodes with grammar-derived
   cold-start priors, word-count coding (fixed, grammar-implied, optional-tail,
   or variable), grammar-driven operand classification (`OperandCursor`),
   semantic ID/type tracking (`Tracker`) with deferred type-algebra result-type
   prediction, per-type and kind-aware adaptive ID recency lists, structured
   literal coding (floats, small ints, enum strides, spec constants), and
   order-2 match/dictionary/primer string coding seeded from the frozen
   `kStringPrimer` and `kPrimedStringMatch`.

- **Error contract:** Exhausting the forward range stream returns
  `ARIXV_STATUS_TRUNCATED`; exhausting the backward rANS stream or failing
  `ConsumedExactly()` returns `ARIXV_STATUS_BAD_CONTAINER`. If a SPIR-V-shaped
  input ends with a final instruction whose declared word count exceeds the
  remaining words in the module, the reference codec bounds operand coding to
  the remaining words and reconstructs the original input bytes losslessly
  rather than rejecting the blob.
- **No built-in integrity checksum:** `ARX2` intentionally omits an embedded
  CRC/hash to keep per-shader framing minimal. Bit flips inside a coded payload
  can occasionally decode to altered bytes without returning an error; callers
  requiring corruption or tamper detection should verify blobs with a package-
  or container-level checksum.
- **Forward compatibility with newer SPIR-V versions:** Opcodes, enumerants,
  and extended instruction sets absent from the frozen `02c0394e…` grammar
  tables fall back automatically to escape/literal coding. A v1.0 `ARX2`
  decoder therefore round-trips modules from newer SPIR-V revisions losslessly
  without requiring grammar updates.

### 3.4 Encoder Mode Selection & `arixv_encode_bound`

The public encoder (`arixv_encode`) attempts mode `2` for SPIR-V-eligible
inputs and mode `1` otherwise (except on 32-bit `ILP32` when a non-SPIR-V input
exceeds the mode-1 rANS staging ceiling `PTRDIFF_MAX / 8`, ~256 MiB, in which
case `arixv_encode` emits mode `0` directly). If the resulting coded blob is
not strictly smaller than a `stored` container for the same input,
`arixv_encode` emits mode `0` (`stored`) instead. Consequently, for every valid
input size `in_size` within platform limits:

```text
arixv_encode_bound(in_size, opts) == 5 + CompactUint32Size(in_size) + in_size
                                  <= in_size + 10
```

## 4. Format Limits vs. Platform Limits

| Limit | 64-bit (`LP64`/`LLP64`) | 32-bit (`ILP32`) |
|---|---|---|
| `ARX2` format max `uncompressed_size` | `UINT32_MAX` (`4,294,967,295` B) | `UINT32_MAX` (`4,294,967,295` B) |
| Platform max addressable `uncompressed_size` | `UINT32_MAX` (~4 GiB) | `PTRDIFF_MAX - 16` (`2,147,483,631` B, ~2 GiB) |
| Max compressed input span (`in_size`) | `PTRDIFF_MAX` | `PTRDIFF_MAX` |
| Mode `1` (`coded`) encoder staging ceiling | `UINT32_MAX` | `PTRDIFF_MAX / 8` (~256 MiB; larger non-SPIR-V inputs fall back to `stored`) |

- `arixv_inspect` validates header framing only (not payload length) and
  reports the full 64-bit declared `uncompressed_size` even if it exceeds the
  host platform's limit.
- `arixv_decode` returns `ARIXV_STATUS_TOO_LARGE` when a valid `ARX2` header
  declares an `uncompressed_size` above the host platform's addressable limit
  (for example, decoding a 3 GiB blob on 32-bit `ILP32`). For compressed modes
  (`1` and `2`), `arixv_decode(..., NULL, ...)` validates header framing only;
  for mode `0` (`stored`), it also validates the exact payload length in O(1).
- `arixv_encode` and `arixv_encode_bound` reject inputs larger than the host
  platform's addressable limit (`ARIXV_STATUS_INVALID_ARGUMENT` / `0`), ensuring
  that `arixv_decode(arixv_encode(x))` never rejects its own output on any
  supported platform.
- **Hostile-input guardrails:** Upfront pre-reservations in the structural
  SPIR-V tracker (`Tracker::ReserveForModule` in `src/model.cpp`) are capped by
  `min(expected_size, payload_size * 32)` and `kMaxReserveWords` (4M words),
  preventing tiny crafted headers from triggering large upfront tracker
  allocations. This cap does not cover the caller's own output buffer or
  subsequent adaptive model growth during decoding; callers using the two-call
  decode pattern should validate `*out_size` against their application budget
  before allocating an output buffer.

## 5. Compatibility & Determinism Guarantees

- **Decoder compatibility (permanent for `ARX2`):** Every valid `ARX2` blob
  within the host platform's addressable limits (§4) will continue to decode to
  the exact original byte sequence across all future 1.x releases and across
  all supported architectures (x86_64 SSE2, AArch64 NEON, 32-bit `ILP32`, and
  scalar fallback).
- **Encoder byte determinism:** Within each platform's coded-mode limits (§4),
  the v1.0 reference `ARX2` encoder produces byte-identical output across
  architectures, compilers, and SIMD/scalar builds, verified by the immutable
  golden fixture suite in [`tests/golden/`](../tests/golden/) (`golden_test`).
  On 32-bit `ILP32`, non-SPIR-V inputs larger than `PTRDIFF_MAX / 8` (~256 MiB)
  fall back directly to stored mode `0` while 64-bit builds may still emit mode
  `1`; both produce valid `ARX2` blobs that decode identically on every
  platform. Any future encoder change that alters `ARX2` coded-stream output
  bytes must allocate a new container magic rather than mutating `ARX2` output.
- **Additive evolution:** Future multi-module / shader-pack formats or revised
  single-module bitstreams will use distinct `ARX?` magic values and additive
  API entry points without breaking `ARX2` decoding or the v1.0 C ABI.
