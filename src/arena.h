// Copyright (c) 2026, Eric Werness
// SPDX-License-Identifier: BSD-2-Clause
//
// Bump allocator for the model's lazily created, trivially destructible
// context objects (bit trees, uint models, count models, ...). A stateless
// decode used to make ~220 separate heap allocations and as many frees for
// them; with an arena that is a handful of chunk allocations, and tearing the
// model down frees chunks instead of walking every context. Objects live as
// long as the arena: a reusable decoder keeps its high-water contexts exactly
// as before (they are reset in place by generation stamps, never freed).

#ifndef ARIXV_ARENA_H_
#define ARIXV_ARENA_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>

namespace arixv {

class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena() {
    Block* block = head_;
    while (block != nullptr) {
      Block* next = block->next;
      std::free(block);
      block = next;
    }
  }

  // A value-initialized T (T{}), valid until the arena is destroyed.
  template <typename T>
  T* New() {
    static_assert(std::is_trivially_destructible_v<T>,
                  "arena objects are never destroyed individually");
    static_assert(alignof(T) <= kMaxAlign, "arena alignment too small");
    return new (Allocate(sizeof(T), alignof(T))) T{};
  }

 private:
  static constexpr size_t kMaxAlign = 16;
  static constexpr size_t kBlockBytes = size_t{64} << 10;

  struct alignas(kMaxAlign) Block {
    Block* next;
  };

  void* Allocate(size_t size, size_t align) {
    size_t at = (used_ + align - 1) & ~(align - 1);
    if (cur_ == nullptr || at + size > cap_) {
      // Large objects get a block of their own so they don't strand the
      // tail of the current one.
      const size_t payload = size > kBlockBytes / 4 ? size : kBlockBytes;
      void* raw = std::malloc(sizeof(Block) + payload);
      if (raw == nullptr) {
        throw std::bad_alloc();
      }
      Block* block = static_cast<Block*>(raw);
      block->next = head_;
      head_ = block;
      if (payload == size && cur_ != nullptr) {
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
  // Back to a fresh T{} in place (what dropping and re-creating it would
  // give), without abandoning arena memory on every reusable decode.
  void reset() {
    if (p_ != nullptr) {
      *p_ = T{};
    }
  }

 private:
  T* p_ = nullptr;
};

}  // namespace arixv

#endif  // ARIXV_ARENA_H_
