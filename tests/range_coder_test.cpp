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
  assert(encoded.size() == 5);
  assert(encoded[0] == 0);
  arixv::RangeDecoder dec(encoded.data(), encoded.size());
  assert(!dec.exhausted());
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
    arixv::RangeDecoder dec(encoded.data(), encoded.size() - chop);
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
  arixv::UintModel uint_model;
  for (uint32_t v : values) {
    assert(arixv::CodeUint(enc, uint_model, v) == v);
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
  arixv::Prob rank_slots[arixv::kRankCount + 1];
  for (int rank = 0; rank <= arixv::kRankCount; ++rank) {
    assert(arixv::CodeRank(enc, rank_slots, rank) ==
           (rank == arixv::kRankCount ? arixv::kRankEscape : rank));
  }
  enc.Flush();

  arixv::RangeDecoder range_dec(encoded.data(), encoded.size());
  arixv::BitDecoder dec(range_dec);
  arixv::UintModel duint_model;
  for (uint32_t v : values) {
    assert(arixv::CodeUint(dec, duint_model, 0) == v);
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
  arixv::Prob drank_slots[arixv::kRankCount + 1];
  for (int rank = 0; rank <= arixv::kRankCount; ++rank) {
    const int expected =
        rank == arixv::kRankCount ? arixv::kRankEscape : rank;
    assert(arixv::CodeRank(dec, drank_slots, 0) == expected);
  }
  assert(!dec.exhausted());
}

}  // namespace

int main() {
  FixedSequence();
  EmptyStream();
  Truncation();
  RandomizedAdaptiveContexts();
  RandomizedBits();
  ProbabilityPrimitives();
  return 0;
}
