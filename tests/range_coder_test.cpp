#include <cassert>
#include <cstdint>
#include <random>
#include <vector>

#include "prob.h"
#include "range_coder.h"
#include "tuning.h"

namespace {

void FixedSequence() {
  const std::vector<int> bits = {0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 1, 1};
  std::vector<uint8_t> encoded;
  arixv::RangeEncoder enc(encoded);
  arixv::Prob probs[3];
  for (size_t i = 0; i < bits.size(); ++i) {
    enc.EncodeBit(probs[i % 3], bits[i]);
  }
  enc.Flush();

  arixv::RangeDecoder dec(encoded.data(), encoded.size());
  arixv::Prob dprobs[3];
  for (size_t i = 0; i < bits.size(); ++i) {
    assert(dec.DecodeBit(dprobs[i % 3]) == bits[i]);
  }
  assert(!dec.exhausted());
}

void EmptyStream() {
  std::vector<uint8_t> encoded;
  arixv::RangeEncoder enc(encoded);
  enc.Flush();
  // Empty stream (no bits coded) emits 0 bytes; all 4 lookahead bytes are
  // don't-care slack.
  assert(encoded.empty());
  arixv::RangeDecoder dec(encoded.data(), encoded.size());
  assert(!dec.exhausted());
  assert(dec.consumed_size() == 0);
  // Attempting to decode a bit from an empty stream must mark exhausted.
  arixv::Prob p;
  (void)dec.DecodeBit(p);
  assert(dec.exhausted());
}

void AdaptiveFlushBoundaries() {
  // Exercise 1-bit streams (wide final range -> 1 byte emitted), narrow final
  // range (range < 0x01FFFFFFu -> 3 flush bytes), and carry propagation across
  // arbitrary trailing bytes (simulating adjacent rANS bytes).
  const uint16_t init_probs[] = {1, 2, 17, 1024, 2048, 3072, 4079, 4094, 4095};
  for (uint16_t pv : init_probs) {
    for (int bit = 0; bit <= 1; ++bit) {
      std::vector<uint8_t> encoded;
      arixv::RangeEncoder enc(encoded);
      arixv::Prob p = arixv::Prob::WithValue(pv);
      enc.EncodeBit(p, bit);
      enc.Flush();
      assert(encoded.size() >= 1 && encoded.size() <= 2);

      for (uint8_t pad : {uint8_t{0x00}, uint8_t{0x55}, uint8_t{0xAA},
                          uint8_t{0xFF}}) {
        std::vector<uint8_t> buf = encoded;
        buf.insert(buf.end(), {pad, pad, pad, pad});
        arixv::RangeDecoder dec(buf.data(), buf.size());
        arixv::Prob dp = arixv::Prob::WithValue(pv);
        assert(dec.DecodeBit(dp) == bit);
        assert(!dec.exhausted());
        assert(dec.consumed_size() == encoded.size());
      }
      // Every strict prefix must either report exhausted or fail to decode
      // the original bit (empty prefix must always report exhausted).
      for (size_t prefix = 0; prefix < encoded.size(); ++prefix) {
        std::vector<uint8_t> trunc(encoded.begin(), encoded.begin() + prefix);
        arixv::RangeDecoder dec(trunc.data(), trunc.size());
        arixv::Prob dp = arixv::Prob::WithValue(pv);
        const int got = dec.DecodeBit(dp);
        if (prefix == 0) {
          assert(dec.exhausted());
        } else {
          assert(dec.exhausted() || got != bit);
        }
      }
    }
  }

  // Sweep many short/medium streams to hit both wide (>= 0x01FFFFFFu) and
  // narrow (< 0x01FFFFFFu) final ranges as well as carry chains (0xFF runs).
  std::mt19937 rng(0xBADC0FFEu);
  size_t saw_wide = 0;
  size_t saw_narrow = 0;
  for (int trial = 0; trial < 600; ++trial) {
    const int n = 1 + (trial % 40);
    std::vector<int> bits(n);
    std::vector<bool> direct(n);
    std::vector<uint8_t> encoded;
    arixv::RangeEncoder enc(encoded);
    arixv::Prob p;
    for (int i = 0; i < n; ++i) {
      // Bias some trials toward all-1s to exercise carry propagation.
      direct[i] = (trial % 5 == 0);
      bits[i] = (trial % 7 == 0) ? 1 : static_cast<int>(rng() & 1u);
      if (direct[i]) {
        enc.EncodeBitDirect(bits[i]);
      } else {
        enc.EncodeBit(p, bits[i]);
      }
    }
    enc.Flush();

    for (uint8_t pad : {uint8_t{0x00}, uint8_t{0xFF}}) {
      std::vector<uint8_t> buf = encoded;
      buf.insert(buf.end(), {pad, pad, pad, pad});
      arixv::RangeDecoder dec(buf.data(), buf.size());
      arixv::Prob dp;
      for (int i = 0; i < n; ++i) {
        const int got =
            direct[i] ? dec.DecodeBitDirect() : dec.DecodeBit(dp);
        assert(got == bits[i]);
      }
      assert(!dec.exhausted());
      assert(dec.consumed_size() == encoded.size());
      if (pad == 0x00) {
        if (dec.TailSlack() == 3) ++saw_wide;
        if (dec.TailSlack() == 2) ++saw_narrow;
      }
    }

    // Truncating by 1 byte on the standalone stream must either exhaust or
    // fail to reproduce the bit sequence.
    if (!encoded.empty()) {
      std::vector<uint8_t> trunc(encoded.begin(), encoded.end() - 1);
      arixv::RangeDecoder dec(trunc.data(), trunc.size());
      arixv::Prob dp;
      bool matched = true;
      for (int i = 0; i < n; ++i) {
        const int got =
            direct[i] ? dec.DecodeBitDirect() : dec.DecodeBit(dp);
        if (got != bits[i]) {
          matched = false;
        }
      }
      assert(dec.exhausted() || !matched);
    }
  }
  assert(saw_wide > 0 && saw_narrow > 0);
}

void Truncation() {
  std::vector<uint8_t> encoded;
  arixv::RangeEncoder enc(encoded);
  arixv::Prob p;
  for (int i = 0; i < 2000; ++i) {
    enc.EncodeBit(p, (i * 17 + 3) & 1);
  }
  enc.Flush();

  for (size_t chop = 1; chop <= 8 && chop <= encoded.size(); ++chop) {
    std::vector<uint8_t> trunc(encoded.begin(), encoded.end() - chop);
    arixv::RangeDecoder dec(trunc.data(), trunc.size());
    arixv::Prob dp;
    for (int i = 0; i < 2000; ++i) {
      (void)dec.DecodeBit(dp);
    }
    assert(dec.exhausted());
  }
}

void RandomizedBits() {
  std::mt19937 rng(0x5A17C0DEu);
  for (int round = 0; round < 220; ++round) {
    const int slots = 1 + static_cast<int>(rng() % 97);
    const int count = 10000 + static_cast<int>(rng() % 90001);
    std::vector<arixv::Prob> probs(slots);
    std::vector<uint8_t> encoded;
    arixv::RangeEncoder enc(encoded);
    std::vector<uint8_t> kinds;
    std::vector<int> slots_used;
    std::vector<uint8_t> bits;
    std::vector<uint32_t> direct_values;
    std::vector<int> direct_widths;

    for (int i = 0; i < count; ++i) {
      if ((rng() % 11) == 0) {
        const int nbits = 1 + static_cast<int>(rng() % 24);
        const uint32_t value = rng() & ((1u << nbits) - 1u);
        enc.EncodeBitDirect(1);
        for (int b = nbits - 1; b >= 0; --b) {
          enc.EncodeBitDirect(static_cast<int>((value >> b) & 1u));
        }
        kinds.push_back(1);
        direct_values.push_back(value);
        direct_widths.push_back(nbits);
      } else {
        const int slot = static_cast<int>(rng() % slots);
        const int skew = static_cast<int>(rng() % 100);
        const int bit = skew < 75 ? 0 : 1;
        enc.EncodeBitDirect(0);
        enc.EncodeBit(probs[slot], bit);
        kinds.push_back(0);
        slots_used.push_back(slot);
        bits.push_back(static_cast<uint8_t>(bit));
      }
    }
    enc.Flush();

    arixv::RangeDecoder dec(encoded.data(), encoded.size());
    std::vector<arixv::Prob> dprobs(slots);
    size_t bit_pos = 0;
    size_t direct_pos = 0;
    for (uint8_t kind : kinds) {
      assert(dec.DecodeBitDirect() == kind);
      if (kind == 1) {
        uint32_t value = 0;
        const int nbits = direct_widths[direct_pos];
        for (int b = 0; b < nbits; ++b) {
          value = (value << 1) | static_cast<uint32_t>(dec.DecodeBitDirect());
        }
        assert(value == direct_values[direct_pos]);
        ++direct_pos;
      } else {
        assert(dec.DecodeBit(dprobs[slots_used[bit_pos]]) == bits[bit_pos]);
        ++bit_pos;
      }
    }
    assert(!dec.exhausted());
  }
}

void RandomizedAdaptiveContexts() {
  std::mt19937 rng(0x12345678u);
  for (int round = 0; round < 220; ++round) {
    const int slots = 1 + static_cast<int>(rng() % 113);
    const int count = 10000 + static_cast<int>(rng() % 90001);
    std::vector<int> slot_log;
    std::vector<int> bit_log;
    std::vector<bool> direct_log;
    std::vector<arixv::Prob> probs(slots);
    std::vector<uint8_t> encoded;
    arixv::RangeEncoder enc(encoded);
    for (int i = 0; i < count; ++i) {
      const int slot = static_cast<int>(rng() % slots);
      const int bit = (rng() % 100) < 80 ? 0 : 1;
      const bool direct = (rng() % 17) == 0;
      slot_log.push_back(slot);
      bit_log.push_back(bit);
      direct_log.push_back(direct);
      if (direct) {
        enc.EncodeBitDirect(bit);
      } else {
        enc.EncodeBit(probs[slot], bit);
      }
    }
    enc.Flush();

    arixv::RangeDecoder dec(encoded.data(), encoded.size());
    std::vector<arixv::Prob> dprobs(slots);
    for (int i = 0; i < count; ++i) {
      if (direct_log[i]) {
        assert(dec.DecodeBitDirect() == bit_log[i]);
      } else {
        assert(dec.DecodeBit(dprobs[slot_log[i]]) == bit_log[i]);
      }
    }
    assert(!dec.exhausted());
  }
}

void ProbabilityPrimitives() {
  std::vector<uint32_t> values = {0, 1, 2, 3, 7, 255, 256, 65535,
                                  0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};
  std::mt19937 rng(0xFACEB00Cu);
  for (int i = 0; i < 2000; ++i) {
    values.push_back(rng());
  }

  std::vector<uint8_t> encoded;
  arixv::RangeEncoder range_enc(encoded);
  arixv::BitEncoder enc(range_enc);
  arixv::Arena arena;
  arixv::UintModel uint_model;
  for (uint32_t v : values) {
    assert(arixv::CodeUint(enc, arena, uint_model, v) == v);
  }
  arixv::BitTree<1> tree1;
  arixv::BitTree<2> tree2;
  arixv::BitTree<3> tree3;
  arixv::BitTree<4> tree4;
  arixv::BitTree<5> tree5;
  arixv::BitTree<6> tree6;
  arixv::BitTree<7> tree7;
  arixv::BitTree<8> tree8;
  assert(tree1.CodeValue(enc, 1) == 1);
  assert(tree2.CodeValue(enc, 2) == 2);
  assert(tree3.CodeValue(enc, 5) == 5);
  assert(tree4.CodeValue(enc, 9) == 9);
  assert(tree5.CodeValue(enc, 17) == 17);
  assert(tree6.CodeValue(enc, 33) == 33);
  assert(tree7.CodeValue(enc, 65) == 65);
  assert(tree8.CodeValue(enc, 129) == 129);
  enc.Flush();

  arixv::RangeDecoder range_dec(encoded.data(), encoded.size());
  arixv::BitDecoder dec(range_dec);
  arixv::Arena darena;
  arixv::UintModel duint_model;
  for (uint32_t v : values) {
    assert(arixv::CodeUint(dec, darena, duint_model, 0) == v);
  }
  arixv::BitTree<1> dtree1;
  arixv::BitTree<2> dtree2;
  arixv::BitTree<3> dtree3;
  arixv::BitTree<4> dtree4;
  arixv::BitTree<5> dtree5;
  arixv::BitTree<6> dtree6;
  arixv::BitTree<7> dtree7;
  arixv::BitTree<8> dtree8;
  assert(dtree1.CodeValue(dec, 0) == 1);
  assert(dtree2.CodeValue(dec, 0) == 2);
  assert(dtree3.CodeValue(dec, 0) == 5);
  assert(dtree4.CodeValue(dec, 0) == 9);
  assert(dtree5.CodeValue(dec, 0) == 17);
  assert(dtree6.CodeValue(dec, 0) == 33);
  assert(dtree7.CodeValue(dec, 0) == 65);
  assert(dtree8.CodeValue(dec, 0) == 129);
  assert(!dec.exhausted());
  assert(dec.ConsumedExactly());
}

void RansMinimalFlushAndHostile() {
  // 0 symbols: emits 0 bytes and finishes cleanly.
  {
    std::vector<uint8_t> out;
    arixv::RansEncoder enc;
    enc.Flush(out);
    assert(out.empty());
    arixv::RansDecoder dec(out.data(), out.size());
    assert(!dec.exhausted());
    assert(dec.consumed_size() == 0);
    assert(dec.FinishedCleanly());
  }

  // 1 symbol across all alphabet sizes and symbol indices, including extreme
  // skewed priors (f == 1 and f == 32767).
  {
    auto test_single = [](int sym, const arixv::SymbolCdf& init_cdf) {
      std::vector<uint8_t> encoded;
      arixv::RangeEncoder rc(encoded);
      arixv::BitEncoder enc(rc);
      arixv::SymbolCdf ecdf = init_cdf;
      assert((enc.CodeSymbol<16, arixv::kSymbolRateCap>(ecdf, sym) ==
              static_cast<uint32_t>(sym)));
      enc.Flush();
      // Empty range stream (0 bytes) + 3-byte rANS state (x = 32768 + lo < 2^16).
      assert(encoded.size() == 3);

      arixv::RangeDecoder rcd(encoded.data(), encoded.size());
      arixv::BitDecoder dec(rcd);
      arixv::SymbolCdf dcdf = init_cdf;
      assert((dec.CodeSymbol<16, arixv::kSymbolRateCap>(dcdf, 0) ==
              static_cast<uint32_t>(sym)));
      assert(!dec.exhausted());
      assert(dec.ConsumedExactly());

      // Every strict prefix must fail.
      for (size_t prefix = 0; prefix < encoded.size(); ++prefix) {
        std::vector<uint8_t> trunc(encoded.begin(), encoded.begin() + prefix);
        arixv::RangeDecoder trc(trunc.data(), trunc.size());
        arixv::BitDecoder tdec(trc);
        arixv::SymbolCdf tcdf = init_cdf;
        (void)tdec.CodeSymbol<16, arixv::kSymbolRateCap>(tcdf, 0);
        assert(tdec.exhausted() || !tdec.ConsumedExactly());
      }
    };

    arixv::SymbolCdf uniform{};
    for (int s = 0; s < 16; ++s) {
      test_single(s, uniform);
    }
    uint32_t skew_lo[16] = {1000000, 0, 0, 0, 0, 0, 0, 0,
                            0,       0, 0, 0, 0, 0, 0, 0};
    arixv::SymbolCdf skewed_lo{};
    arixv::SymbolCdfSetPrior<16>(skewed_lo, skew_lo);
    test_single(0, skewed_lo);
    test_single(15, skewed_lo);

    uint32_t skew_hi[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 1000000};
    arixv::SymbolCdf skewed_hi{};
    arixv::SymbolCdfSetPrior<16>(skewed_hi, skew_hi);
    test_single(0, skewed_hi);
    test_single(15, skewed_hi);
  }

  // Multi-symbol streams hitting both 3-byte (MSB=0) and 4-byte (MSB=1) final
  // rANS states, plus every prefix truncation on short streams.
  std::mt19937 rng(0x51A7E123u);
  size_t saw_3byte = 0;
  size_t saw_4byte = 0;
  for (int trial = 0; trial < 400; ++trial) {
    const int n_syms = 1 + (trial % 30);
    const int n_bits = trial % 17;
    std::vector<uint32_t> syms(n_syms);
    std::vector<bool> bits(n_bits);
    std::vector<uint8_t> encoded;
    arixv::RangeEncoder rc(encoded);
    arixv::BitEncoder enc(rc);
    arixv::SymbolCdf ecdf{};
    arixv::Prob eprob;
    for (int i = 0; i < n_bits; ++i) {
      bits[i] = (rng() & 1u) != 0;
      enc.CodeBit(eprob, bits[i]);
    }
    for (int i = 0; i < n_syms; ++i) {
      syms[i] = rng() & 15u;
      enc.CodeSymbol<16, arixv::kSymbolRateCap>(ecdf, syms[i]);
    }
    enc.Flush();
    if ((encoded.back() & 0x80u) == 0) {
      ++saw_3byte;
    } else {
      ++saw_4byte;
    }

    arixv::RangeDecoder rcd(encoded.data(), encoded.size());
    arixv::BitDecoder dec(rcd);
    arixv::SymbolCdf dcdf{};
    arixv::Prob dprob;
    for (int i = 0; i < n_bits; ++i) {
      assert(dec.CodeBit(dprob, false) == bits[i]);
    }
    for (int i = 0; i < n_syms; ++i) {
      assert((dec.CodeSymbol<16, arixv::kSymbolRateCap>(dcdf, 0) == syms[i]));
    }
    assert(!dec.exhausted());
    assert(dec.ConsumedExactly());

    if (trial < 60) {
      for (size_t prefix = 0; prefix < encoded.size(); ++prefix) {
        std::vector<uint8_t> trunc(encoded.begin(), encoded.begin() + prefix);
        arixv::RangeDecoder trc(trunc.data(), trunc.size());
        arixv::BitDecoder tdec(trc);
        arixv::SymbolCdf tcdf{};
        arixv::Prob tprob;
        for (int i = 0; i < n_bits && !tdec.exhausted(); ++i) {
          (void)tdec.CodeBit(tprob, false);
        }
        for (int i = 0; i < n_syms && !tdec.exhausted(); ++i) {
          (void)tdec.CodeSymbol<16, arixv::kSymbolRateCap>(tcdf, 0);
        }
        assert(tdec.exhausted() || !tdec.ConsumedExactly());
      }
      // Appending an extra byte must fail exact tiling.
      std::vector<uint8_t> padded = encoded;
      padded.push_back(0);
      arixv::RangeDecoder prc(padded.data(), padded.size());
      arixv::BitDecoder pdec(prc);
      arixv::SymbolCdf pcdf{};
      arixv::Prob pprob;
      for (int i = 0; i < n_bits && !pdec.exhausted(); ++i) {
        (void)pdec.CodeBit(pprob, false);
      }
      for (int i = 0; i < n_syms && !pdec.exhausted(); ++i) {
        (void)pdec.CodeSymbol<16, arixv::kSymbolRateCap>(pcdf, 0);
      }
      assert(pdec.exhausted() || !pdec.ConsumedExactly());
    }
  }
  assert(saw_3byte > 0 && saw_4byte > 0);

  // Hostile rANS inputs: truncated, non-canonical 4-byte, out-of-range 4-byte.
  const std::vector<std::vector<uint8_t>> bad_states = {
      {},                              // 0 bytes
      {0x00},                          // 1 byte
      {0x00, 0x00},                    // 2 bytes
      {0x00, 0x00, 0x80},              // 4-byte tag with only 3 bytes
      {0x00, 0x00, 0x00, 0x80},        // non-canonical 4-byte (y = 0 < 2^23)
      {0xFF, 0xFF, 0x7F, 0x80},        // non-canonical 4-byte (y = 2^23 - 1)
      {0x00, 0x80, 0xFF, 0xFF},        // out-of-range (y = 0x7FFF8000, x = 2^31)
      {0xFF, 0xFF, 0xFF, 0xFF},        // out-of-range (y = 0x7FFFFFFF)
  };
  for (const auto& bad : bad_states) {
    arixv::RansDecoder dec(bad.data(), bad.size());
    arixv::SymbolCdf cdf{};
    (void)dec.DecodeSymbol<16, arixv::kSymbolRateCap>(cdf);
    assert(dec.exhausted());
    assert(!dec.FinishedCleanly());
    // Repeated call after failure remains exhausted and does not advance past
    // begin_.
    const size_t consumed = dec.consumed_size();
    (void)dec.DecodeSymbol<16, arixv::kSymbolRateCap>(cdf);
    assert(dec.exhausted());
    assert(dec.consumed_size() == consumed);
  }
}

}  // namespace

int main() {
  FixedSequence();
  EmptyStream();
  AdaptiveFlushBoundaries();
  Truncation();
  RandomizedAdaptiveContexts();
  RandomizedBits();
  ProbabilityPrimitives();
  RansMinimalFlushAndHostile();
  return 0;
}
