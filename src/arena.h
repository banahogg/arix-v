// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// Bump allocator for the model's lazily created, trivially destructible
// context objects (bit trees, uint models, count models, ...). A stateless
// decode used to make ~220 separate heap allocations and as many frees for
// them; with an arena that is a handful of chunk allocations, and tearing the
// model down frees chunks instead of walking every context. Objects live as
// long as the arena, i.e. for one encode or decode.

#ifndef ARIXV_ARENA_H_
#define ARIXV_ARENA_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "platform.h"

#ifdef ARIXV_MEMSTATS
#include <map>
#include <string>
#include <utility>
#endif

namespace arixv {
inline namespace v1 {

#ifdef ARIXV_MEMSTATS
// Diagnostics build only: every arena object by type (count, bytes), and the
// bytes of blocks the arenas requested.
inline std::map<std::string, std::pair<uint64_t, uint64_t>> g_arena_objects;
inline uint64_t g_arena_block_bytes = 0;
template <typename T>
const char* ArenaTypeName() {
#if defined(_MSC_VER)
  return __FUNCSIG__;
#else
  return __PRETTY_FUNCTION__;
#endif
}
#endif

class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena() {
    Block* block = head_;
    while (block != nullptr) {
      Block* next = block->next;
      AlignedFree(block);
      block = next;
    }
  }

  // A value-initialized T (T{}), valid until the arena is destroyed.
  template <typename T>
  T* New() {
    static_assert(std::is_trivially_destructible_v<T>,
                  "arena objects are never destroyed individually");
    static_assert(alignof(T) <= kMaxAlign, "arena alignment too small");
#ifdef ARIXV_MEMSTATS
    auto& entry = g_arena_objects[ArenaTypeName<T>()];
    ++entry.first;
    entry.second += sizeof(T);
#endif
    return new (Allocate(sizeof(T), alignof(T))) T{};
  }

 private:
  static constexpr size_t kMaxAlign = 16;
  // Blocks start small and double up to kMaxBlockBytes, so a small module
  // (or a ByteModel holding a few trees) touches a few KB, not 64 KB per
  // arena: every fresh page is a page fault in a short-lived process or
  // after the allocator trims the heap.
  static constexpr size_t kFirstBlockBytes = size_t{4} << 10;
  static constexpr size_t kMaxBlockBytes = size_t{64} << 10;

  static void* AlignedAlloc(size_t bytes) {
#if defined(_WIN32)
    return _aligned_malloc(bytes, kMaxAlign);
#else
    void* ptr = nullptr;
    return posix_memalign(&ptr, kMaxAlign, bytes) == 0 ? ptr : nullptr;
#endif
  }

  static void AlignedFree(void* ptr) {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
  }

  struct alignas(kMaxAlign) Block {
    Block* next;
  };

  void* Allocate(size_t size, size_t align) {
    size_t at = (used_ + align - 1) & ~(align - 1);
    if (cur_ == nullptr || at + size > cap_) {
      // Large objects get a block of their own so they don't strand the
      // tail of the current one.
      const bool own_block = size > kMaxBlockBytes / 4;
      size_t payload = size;
      if (!own_block) {
        payload = next_block_ > size ? next_block_ : size;
        next_block_ = next_block_ < kMaxBlockBytes ? next_block_ * 2
                                                   : kMaxBlockBytes;
      }
#ifdef ARIXV_MEMSTATS
      g_arena_block_bytes += sizeof(Block) + payload;
#endif
      void* raw = AlignedAlloc(sizeof(Block) + payload);
      if (raw == nullptr) {
#if ARIXV_HAS_EXCEPTIONS
        throw std::bad_alloc();
#else
        std::abort();
#endif
      }
      Block* block = static_cast<Block*>(raw);
      block->next = head_;
      head_ = block;
      if (own_block && cur_ != nullptr) {
        return reinterpret_cast<unsigned char*>(block + 1);
      }
      cur_ = reinterpret_cast<unsigned char*>(block + 1);
      cap_ = payload;
      at = 0;
    }
    used_ = at + size;
    return cur_ + at;
  }

  Block* head_ = nullptr;
  unsigned char* cur_ = nullptr;
  size_t used_ = 0;
  size_t cap_ = 0;
  size_t next_block_ = kFirstBlockBytes;
};

// Non-owning, trivially destructible handle to an arena object, standing in
// for the std::unique_ptr members the model used for lazy contexts.
template <typename T>
class ArenaPtr {
 public:
  explicit operator bool() const { return p_ != nullptr; }
  T& operator*() const { return *p_; }
  T* operator->() const { return p_; }
  T* get() const { return p_; }
  void Make(Arena& arena) { p_ = arena.New<T>(); }

 private:
  T* p_ = nullptr;
};

}  // namespace v1
}  // namespace arixv

#endif  // ARIXV_ARENA_H_
