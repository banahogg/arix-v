// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// RESEARCH ONLY: instruction frames and ID roles as the model codes a
// module (arixv_structure_dump). Included by src/model.cpp inside namespace
// arixv::v1::research when ARIXV_RESEARCH_PACK is defined. The structural
// pack prototypes (AXST, the ARXP mirror) became the production ARXP codec
// in src/pack_block.inc.

#if defined(ARIXV_ENCODER) && (ARIXV_ENCODER != 0)
bool ModelIdRoles(const uint8_t* data, size_t size,
                  std::vector<IdRoleFrame>* frames,
                  std::vector<uint8_t>* mask) {
  std::vector<pack::FrameSpan> f;
  frames->clear();
  if (!pack::ModuleIdRoles(data, size, &f, mask)) return false;
  for (const pack::FrameSpan& s : f) frames->push_back({s.begin, s.nwords});
  return true;
}
#endif
