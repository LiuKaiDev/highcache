#include "highcache/memory/slab_allocator.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace highcache {
namespace {

constexpr std::size_t slab_alignment = alignof(std::max_align_t);
static_assert(SlabAllocator::size_classes.front() % slab_alignment == 0);

} // namespace

class SlabAllocator::SlabClass final {
public:
  explicit SlabClass(const std::size_t object_size)
      : object_size_(object_size) {}

  [[nodiscard]] void *allocate() {
    if (free_head_ == nullptr) {
      add_slab();
    }

    auto *const block = free_head_;
    free_head_ = free_head_->next;
    return block;
  }

  void deallocate(void *const pointer) noexcept {
    auto *const block = ::new (pointer) FreeNode{free_head_};
    free_head_ = block;
  }

  [[nodiscard]] std::size_t slab_count() const noexcept {
    return slabs_.size();
  }

private:
  struct FreeNode final {
    FreeNode *next;
  };

  class Slab final {
  public:
    Slab()
        : data_(::operator new(SlabAllocator::slab_size,
                               std::align_val_t{slab_alignment})) {}

    ~Slab() { ::operator delete(data_, std::align_val_t{slab_alignment}); }

    Slab(const Slab &) = delete;
    Slab &operator=(const Slab &) = delete;
    Slab(Slab &&) = delete;
    Slab &operator=(Slab &&) = delete;

    [[nodiscard]] std::byte *data() noexcept {
      return static_cast<std::byte *>(data_);
    }

  private:
    void *data_;
  };

  void add_slab() {
    auto slab = std::make_unique<Slab>();
    auto *const data = slab->data();
    slabs_.push_back(std::move(slab));

    const auto object_count = SlabAllocator::slab_size / object_size_;
    for (std::size_t index = 0; index < object_count; ++index) {
      deallocate(data + index * object_size_);
    }
  }

  std::size_t object_size_;
  FreeNode *free_head_{nullptr};
  std::vector<std::unique_ptr<Slab>> slabs_;
};

SlabAllocator::SlabAllocator() {
  for (std::size_t index = 0; index < size_classes.size(); ++index) {
    classes_[index] = std::make_unique<SlabClass>(size_classes[index]);
  }
}

SlabAllocator::~SlabAllocator() = default;

void *SlabAllocator::allocate(const std::size_t size) {
  if (size == 0) {
    return nullptr;
  }

  if (size > size_classes.back()) {
    void *const pointer = ::operator new(size);
    metrics_.allocated_bytes += size;
    metrics_.used_bytes += size;
    ++metrics_.allocation_count;
    return pointer;
  }

  const auto index = class_index(size);
  const auto class_size = size_classes[index];
  const auto slabs_before = classes_[index]->slab_count();
  void *const pointer = classes_[index]->allocate();
  if (classes_[index]->slab_count() != slabs_before) {
    metrics_.allocated_bytes += slab_size;
    metrics_.free_bytes += slab_size;
    ++metrics_.slab_count;
  }

  metrics_.used_bytes += size;
  metrics_.free_bytes -= class_size;
  metrics_.internal_fragmentation += class_size - size;
  ++metrics_.allocation_count;
  return pointer;
}

void SlabAllocator::deallocate(void *const pointer,
                               const std::size_t size) noexcept {
  if (pointer == nullptr) {
    assert(size == 0);
    return;
  }

  assert(size != 0);
  assert(metrics_.used_bytes >= size);
  if (size > size_classes.back()) {
    assert(metrics_.allocated_bytes >= size);
    ::operator delete(pointer);
    metrics_.allocated_bytes -= size;
    metrics_.used_bytes -= size;
    ++metrics_.deallocation_count;
    return;
  }

  const auto index = class_index(size);
  const auto class_size = size_classes[index];
  const auto fragmentation = class_size - size;
  assert(metrics_.internal_fragmentation >= fragmentation);
  classes_[index]->deallocate(pointer);
  metrics_.used_bytes -= size;
  metrics_.free_bytes += class_size;
  metrics_.internal_fragmentation -= fragmentation;
  ++metrics_.deallocation_count;
}

const SlabAllocatorMetrics &SlabAllocator::metrics() const noexcept {
  return metrics_;
}

std::size_t
SlabAllocator::allocation_size(const std::size_t requested_size) noexcept {
  if (requested_size == 0 || requested_size > size_classes.back()) {
    return requested_size;
  }
  return size_classes[class_index(requested_size)];
}

std::size_t
SlabAllocator::class_index(const std::size_t requested_size) noexcept {
  assert(requested_size > 0);
  assert(requested_size <= size_classes.back());
  for (std::size_t index = 0; index < size_classes.size(); ++index) {
    if (requested_size <= size_classes[index]) {
      return index;
    }
  }

  assert(false);
  return size_classes.size() - 1;
}

} // namespace highcache
