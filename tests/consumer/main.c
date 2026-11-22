/* Copyright (c) 2026, Eric Werness
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <arixv/arixv.h>

/* Private headers under src/ must never leak onto downstream include paths. */
#ifdef __has_include
#if __has_include("tuning.h") || __has_include("platform.h") || \
    __has_include("model.h") || __has_include("tracker.h") || \
    __has_include("container.h")
#error "Private ARIX-V headers leaked into public include path"
#endif
#endif

#if CONSUMER_HAS_ENCODER
#if !ARIXV_HAS_ENCODER_
#error "arixv::encoder must propagate ARIXV_ENCODER=1"
#endif
#else
#if ARIXV_HAS_ENCODER_
#error "arixv::decoder must not expose encoder declarations"
#endif
#endif

int main(void) {
  static const uint8_t kMinimalArx2[] = {
      0x41, 0x52, 0x58, 0x32, 0x02, 0x00, 0x10, 0x6c, 0xe3, 0x3f, 0xa3
  };
  static const uint8_t kMinimalSpv[] = {
      0x03, 0x02, 0x23, 0x07,
      0x00, 0x03, 0x01, 0x00,
      0x00, 0x00, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00
  };
  uint32_t expected_version =
      ((uint32_t)ARIXV_VERSION_MAJOR << 16) |
      ((uint32_t)ARIXV_VERSION_MINOR << 8) |
      (uint32_t)ARIXV_VERSION_PATCH;
  arixv_header_info info;
  arixv_decode_options dec_opts;
  uint32_t decoded_words[5];
  size_t decoded_size = 0;

  if (arixv_version() != expected_version) {
    fprintf(stderr, "consumer_smoke (C11): version mismatch\n");
    return 1;
  }

  memset(&info, 0, sizeof(info));
  info.struct_size = (uint32_t)sizeof(info);
  if (arixv_inspect(kMinimalArx2, sizeof(kMinimalArx2), &info) !=
          ARIXV_STATUS_OK ||
      info.mode != ARIXV_MODE_SPIRV ||
      info.uncompressed_size != sizeof(kMinimalSpv)) {
    fprintf(stderr, "consumer_smoke (C11): inspect failed\n");
    return 1;
  }

  if (arixv_decode(kMinimalArx2, sizeof(kMinimalArx2), NULL, &decoded_size,
                   NULL) != ARIXV_STATUS_OK ||
      decoded_size != sizeof(kMinimalSpv)) {
    fprintf(stderr, "consumer_smoke (C11): decode size query failed\n");
    return 1;
  }

  memset(&dec_opts, 0, sizeof(dec_opts));
  dec_opts.struct_size = (uint32_t)sizeof(dec_opts);
  if (arixv_decode(kMinimalArx2, sizeof(kMinimalArx2), decoded_words,
                   &decoded_size, &dec_opts) != ARIXV_STATUS_OK) {
    fprintf(stderr, "consumer_smoke (C11): decode failed\n");
    return 1;
  }
  if (decoded_size != sizeof(kMinimalSpv) ||
      memcmp(decoded_words, kMinimalSpv, sizeof(kMinimalSpv)) != 0) {
    fprintf(stderr, "consumer_smoke (C11): decoded bytes mismatch\n");
    return 1;
  }

#if CONSUMER_HAS_ENCODER
  {
    arixv_encode_options enc_opts;
    uint8_t encoded[64];
    size_t bound;
    size_t encoded_size;

    memset(&enc_opts, 0, sizeof(enc_opts));
    enc_opts.struct_size = (uint32_t)sizeof(enc_opts);
    bound = arixv_encode_bound(sizeof(kMinimalSpv), &enc_opts);
    if (bound < 6 || bound > sizeof(encoded)) {
      fprintf(stderr, "consumer_smoke (C11): encode bound invalid\n");
      return 1;
    }
    encoded_size = bound;
    if (arixv_encode(decoded_words, sizeof(kMinimalSpv), encoded,
                     &encoded_size, &enc_opts) != ARIXV_STATUS_OK) {
      fprintf(stderr, "consumer_smoke (C11): encode failed\n");
      return 1;
    }
    if (encoded_size != sizeof(kMinimalArx2) ||
        memcmp(encoded, kMinimalArx2, sizeof(kMinimalArx2)) != 0) {
      fprintf(stderr, "consumer_smoke (C11): re-encoded bytes mismatch\n");
      return 1;
    }
  }
#endif

  if (arixv_status_string(ARIXV_STATUS_OK) == NULL) {
    return 1;
  }
  printf("consumer_smoke (C11): ok (encoder=%d)\n", (int)CONSUMER_HAS_ENCODER);
  return 0;
}
