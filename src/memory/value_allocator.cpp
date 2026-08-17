#include "highcache/memory/value_allocator.h"

#include "highcache/common/error.h"
#include "highcache/memory/slab_allocator.h"

#include <cassert>
#include <cstddef>
#include <memory>
#include <new>

namespace highcache {
namespace {

class SystemAllocator final : public ValueAllocator {
public:
  [[nodiscard]] void *allocate(const std::size_t size) override {
    if (size == 0) {
      return nullptr;
    }

    void *const pointer = ::operator new(size);
    metrics_.allocated_bytes += size;
    metrics_.used_bytes += size;
    ++metrics_.allocation_count;
    return pointer;
  }

  void deallocate(void *const pointer,
                  const std::size_t size) noexcept override {
    if (pointer == nullptr) {
      assert(size == 0);
      return;
    }

    assert(size != 0);
    assert(metrics_.allocated_bytes >= size);
    assert(metrics_.used_bytes >= size);
    ::operator delete(pointer);
    metrics_.allocated_bytes -= size;
    metrics_.used_bytes -= size;
    ++metrics_.deallocation_count;
  }

  [[nodiscard]] const ValueAllocatorMetrics &metrics() const noexcept override {
    return metrics_;
  }

private:
  ValueAllocatorMetrics metrics_;
};

} // namespace

std::unique_ptr<ValueAllocator>
make_value_allocator(const AllocatorBackend backend) {
  switch (backend) {
  case AllocatorBackend::system:
    return std::make_unique<SystemAllocator>();
  case AllocatorBackend::slab:
    return std::make_unique<SlabAllocator>();
  }

  throw HighCacheError(ErrorCode::invalid_argument,
                       "unknown value allocator backend");
}

AllocatorBackend parse_allocator_backend(const std::string_view value) {
  if (value == "system") {
    return AllocatorBackend::system;
  }
  if (value == "slab") {
    return AllocatorBackend::slab;
  }
  throw HighCacheError(ErrorCode::invalid_argument,
                       "unsupported value allocator backend");
}

std::string_view to_string(const AllocatorBackend backend) noexcept {
  switch (backend) {
  case AllocatorBackend::system:
    return "system";
  case AllocatorBackend::slab:
    return "slab";
  }
  return "unknown";
}

} // namespace highcache
