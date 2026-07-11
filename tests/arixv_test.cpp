#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "arixv.h"
#include "tuning.h"

namespace {

uint8_t ContainerMode(const std::vector<uint8_t>& encoded) {
  assert(encoded.size() > arixv::kContainerMagicBytes);
  assert(std::equal(arixv::kContainerMagic,
                    arixv::kContainerMagic + arixv::kContainerMagicBytes,
                    encoded.begin()));
  return encoded[arixv::kContainerMagicBytes];
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  assert(size >= 0);
  in.seekg(0, std::ios::beg);
  data.resize(static_cast<size_t>(size));
  if (!data.empty()) {
    in.read(reinterpret_cast<char*>(data.data()), size);
  }
  return static_cast<bool>(in);
}

void RoundTripBytes(const std::vector<uint8_t>& input, bool force_coded) {
  arixv::EncodeOptions opts;
  opts.force_coded = force_coded;
  const std::vector<uint8_t> encoded =
      arixv::Encode(input.data(), input.size(), opts);
  (void)ContainerMode(encoded);
  std::vector<uint8_t> decoded;
  assert(arixv::Decode(encoded.data(), encoded.size(), decoded) ==
         arixv::Status::kOk);
  assert(decoded == input);
}

void BasicRoundTrips() {
  RoundTripBytes({}, false);
  RoundTripBytes({}, true);
  RoundTripBytes({0xA5}, false);
  RoundTripBytes({0xA5}, true);

  const std::string text =
      "SPIR-V byte fallback model test. Repeated text compresses enough to "
      "exercise coded mode without relying on SPIR-V structure.";
  RoundTripBytes(std::vector<uint8_t>(text.begin(), text.end()), false);
  RoundTripBytes(std::vector<uint8_t>(text.begin(), text.end()), true);
}

void GoldenSpirvEncoding() {
  // Minimal little-endian SPIR-V module: header only, version 1.3, bound 1.
  // This is an exact ARX1 format vector. Any byte-format change must use a
  // new magic and update this vector for that format.
  const std::vector<uint8_t> input = {
      0x03, 0x02, 0x23, 0x07, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const std::vector<uint8_t> expected = {
      0x41, 0x52, 0x58, 0x31, 0x02, 0x14, 0x00,
      0x44, 0xbf, 0xf8, 0x45, 0x76, 0xe8, 0x00,
  };
  arixv::EncodeOptions options;
  options.force_coded = true;
  const std::vector<uint8_t> encoded =
      arixv::Encode(input.data(), input.size(), options);
  assert(encoded == expected);
  assert(ContainerMode(encoded) == arixv::kModeSpirv);

  std::vector<uint8_t> decoded;
  assert(arixv::Decode(expected.data(), expected.size(), decoded) ==
         arixv::Status::kOk);
  assert(decoded == input);
}

void RandomRoundTrips() {
  std::mt19937 rng(0xC001D00Du);
  for (size_t size : {size_t{1024}, size_t{1024 * 1024}}) {
    std::vector<uint8_t> input(size);
    for (uint8_t& b : input) {
      b = static_cast<uint8_t>(rng());
    }
    // Hard expansion contract: default encoding falls back to
    // stored mode, output <= input + 10 bytes.
    const std::vector<uint8_t> stored =
        arixv::Encode(input.data(), input.size());
    assert(stored.size() <= input.size() + 10);

    // Forced-coded path: the accelerated early-adaptation schedule costs a
    // few percent on incompressible data (oscillation before saturation);
    // that trade buys ~4% absolute corpus ratio and only force_coded (a test
    // knob) ever exposes it. Bound it loosely so regressions still surface.
    arixv::EncodeOptions opts;
    opts.force_coded = true;
    const std::vector<uint8_t> encoded =
        arixv::Encode(input.data(), input.size(), opts);
    assert(encoded.size() <= input.size() + input.size() / 20 + 1024);
    std::vector<uint8_t> decoded;
    assert(arixv::Decode(encoded.data(), encoded.size(), decoded) ==
           arixv::Status::kOk);
    assert(decoded == input);
    std::vector<uint8_t> stored_decoded;
    assert(arixv::Decode(stored.data(), stored.size(), stored_decoded) ==
           arixv::Status::kOk);
    assert(stored_decoded == input);
  }
}

void CorpusRoundTrips() {
  const std::filesystem::path dir = "corpus/spv";
  if (!std::filesystem::exists(dir)) {
    return;
  }
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() != ".spv") {
      continue;
    }
    std::vector<uint8_t> input;
    assert(ReadFile(entry.path(), input));
    RoundTripBytes(input, false);
    RoundTripBytes(input, true);
    ++count;
  }
  assert(count > 0);
}

void StoredModeForIncompressible() {
  std::mt19937 rng(0x51525354u);
  std::vector<uint8_t> input(16384);
  for (uint8_t& b : input) {
    b = static_cast<uint8_t>(rng());
  }
  const std::vector<uint8_t> encoded =
      arixv::Encode(input.data(), input.size());
  assert(!encoded.empty());
  assert(ContainerMode(encoded) == arixv::kModeStored);
  assert(encoded.size() <= input.size() + 10);
}

void HostileInputs() {
  const std::vector<uint8_t> input = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  arixv::EncodeOptions opts;
  opts.force_coded = true;
  const std::vector<uint8_t> valid =
      arixv::Encode(input.data(), input.size(), opts);
  for (size_t prefix = 0; prefix < valid.size(); ++prefix) {
    std::vector<uint8_t> out;
    const arixv::Status status = arixv::Decode(valid.data(), prefix, out);
    assert(status != arixv::Status::kOk);
  }
  std::vector<uint8_t> padded = valid;
  padded.push_back(0);
  std::vector<uint8_t> out;
  assert(arixv::Decode(padded.data(), padded.size(), out) ==
         arixv::Status::kBadContainer);

  std::vector<uint8_t> wrong_magic = valid;
  wrong_magic[0] ^= 1;
  assert(arixv::Decode(wrong_magic.data(), wrong_magic.size(), out) ==
         arixv::Status::kBadContainer);
  assert(out.empty());

  std::vector<uint8_t> stored =
      arixv::Encode(input.data(), input.size(), arixv::EncodeOptions{});
  stored[arixv::kContainerMagicBytes] = arixv::kModeStored;
  for (size_t prefix = 0; prefix < stored.size(); ++prefix) {
    std::vector<uint8_t> out;
    const arixv::Status status = arixv::Decode(stored.data(), prefix, out);
    assert(status != arixv::Status::kOk);
  }

  std::vector<uint8_t> huge(arixv::kContainerMagic,
                            arixv::kContainerMagic + arixv::kContainerMagicBytes);
  huge.insert(huge.end(), {arixv::kModeStored, 0x80, 0x80, 0x80, 0x80,
                            0x80, 0x20});
  const arixv::Status status = arixv::Decode(huge.data(), huge.size(), out);
  assert(status == arixv::Status::kBadContainer ||
         status == arixv::Status::kTooLarge);

  std::vector<uint8_t> unknown(arixv::kContainerMagic,
                               arixv::kContainerMagic + arixv::kContainerMagicBytes);
  unknown.insert(unknown.end(), {99, 0});
  assert(arixv::Decode(unknown.data(), unknown.size(), out) ==
         arixv::Status::kBadContainer);
}

void ExpectReusableDecode(arixv::Decoder& decoder,
                          const std::vector<uint8_t>& encoded,
                          const std::vector<uint8_t>& expected) {
  std::vector<uint8_t> reused;
  assert(decoder.Decode(encoded.data(), encoded.size(), reused) ==
         arixv::Status::kOk);
  assert(reused == expected);

  arixv::Decoder fresh_decoder;
  std::vector<uint8_t> fresh;
  assert(fresh_decoder.Decode(encoded.data(), encoded.size(), fresh) ==
         arixv::Status::kOk);
  assert(fresh == expected);
  assert(reused == fresh);
}

void DecoderReuse() {
  arixv::Decoder decoder;
  arixv::EncodeOptions opts;
  opts.force_coded = true;

  const std::string text1 =
      "first reusable decoder byte payload, deliberately repeated repeated";
  const std::vector<uint8_t> bytes1(text1.begin(), text1.end());
  const std::vector<uint8_t> encoded1 =
      arixv::Encode(bytes1.data(), bytes1.size(), opts);
  assert(ContainerMode(encoded1) == arixv::kModeCoded);
  ExpectReusableDecode(decoder, encoded1, bytes1);

  std::vector<uint8_t> bad = encoded1;
  bad.push_back(0);
  std::vector<uint8_t> out;
  assert(decoder.Decode(bad.data(), bad.size(), out) ==
         arixv::Status::kBadContainer);
  assert(out.empty());

  const std::string text2 =
      "second payload uses the same decoder after an error path";
  const std::vector<uint8_t> bytes2(text2.begin(), text2.end());
  const std::vector<uint8_t> encoded2 =
      arixv::Encode(bytes2.data(), bytes2.size(), opts);
  assert(ContainerMode(encoded2) == arixv::kModeCoded);
  ExpectReusableDecode(decoder, encoded2, bytes2);

  const std::filesystem::path dir = "corpus/spv";
  if (!std::filesystem::exists(dir)) {
    return;
  }
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() != ".spv") {
      continue;
    }
    std::vector<uint8_t> input;
    assert(ReadFile(entry.path(), input));
    const std::vector<uint8_t> encoded =
        arixv::Encode(input.data(), input.size(), opts);
    assert(ContainerMode(encoded) == arixv::kModeSpirv);
    ExpectReusableDecode(decoder, encoded, input);
    if (++count == 2) {
      break;
    }
  }
  assert(count >= 2);
}

}  // namespace

int main() {
  BasicRoundTrips();
  GoldenSpirvEncoding();
  RandomRoundTrips();
  CorpusRoundTrips();
  StoredModeForIncompressible();
  HostileInputs();
  DecoderReuse();
  return 0;
}
