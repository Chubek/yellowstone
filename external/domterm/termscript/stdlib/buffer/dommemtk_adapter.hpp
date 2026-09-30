#pragma once
/* Optional C++ embedding adapter. The ordinary Termscript archive remains
 * C-only; hosts that already use DomMEMTk can opt in before creating buffers. */
#include <termscript/stdlib/buffer/allocator.h>
#include <dommemtk/space/free_list.hpp>
#include <dommemtk/core/types.hpp>
#include <cstddef>
#include <memory>
#include <mutex>
#include <limits>

namespace termscript {
class DomMEMTkBufferArena {
 public:
  explicit DomMEMTkBufferArena(std::size_t bytes)
      : words_((bytes / sizeof(std::max_align_t)) +
               (bytes % sizeof(std::max_align_t) != 0)),
        storage_(std::make_unique<std::max_align_t[]>(words_)),
        free_list_(DomMEMTk::as_address(storage_.get()),
                   DomMEMTk::as_address(storage_.get()) +
                       words_ * sizeof(std::max_align_t)) {}

  DomMEMTkBufferArena(const DomMEMTkBufferArena&) = delete;
  DomMEMTkBufferArena& operator=(const DomMEMTkBufferArena&) = delete;

  TS_BufferAllocator callbacks() noexcept {
    return TS_BufferAllocator{this, &allocate, &release};
  }
  std::size_t free_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return free_list_.free_bytes();
  }

 private:
  static void* allocate(void* ctx, std::size_t bytes) noexcept {
    auto& self = *static_cast<DomMEMTkBufferArena*>(ctx);
    try {
      std::lock_guard<std::mutex> lock(self.mutex_);
      auto result = self.free_list_.allocate(bytes, alignof(std::max_align_t));
      return result.is_ok() ? reinterpret_cast<void*>(result.unwrap()) : nullptr;
    } catch (...) { return nullptr; }
  }
  static void release(void* ctx, void* ptr, std::size_t bytes) noexcept {
    auto& self = *static_cast<DomMEMTkBufferArena*>(ctx);
    try {
      std::lock_guard<std::mutex> lock(self.mutex_);
      self.free_list_.free(DomMEMTk::as_address(ptr), bytes);
    } catch (...) { /* A host must keep its arena alive through handle release. */ }
  }
  std::size_t words_;
  std::unique_ptr<std::max_align_t[]> storage_;
  mutable std::mutex mutex_;
  DomMEMTk::FreeListAllocator free_list_;
};
} // namespace termscript
