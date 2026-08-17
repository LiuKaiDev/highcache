#include "highcache/net/buffer.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace highcache {

Buffer::Buffer(const std::size_t initial_capacity) {
  storage_.reserve(initial_capacity);
}

const char *Buffer::data() const noexcept {
  return empty() ? nullptr : storage_.data() + reader_index_;
}

std::string_view Buffer::readable_view() const noexcept {
  if (empty()) {
    return {};
  }
  return {data(), readable_bytes()};
}

std::size_t Buffer::readable_bytes() const noexcept {
  return storage_.size() - reader_index_;
}

std::size_t Buffer::writable_bytes() const noexcept {
  return storage_.capacity() - storage_.size();
}

bool Buffer::empty() const noexcept { return readable_bytes() == 0; }

void Buffer::append(const void *const data, const std::size_t length) {
  if (length == 0) {
    return;
  }
  if (data == nullptr) {
    throw std::invalid_argument("buffer append data must not be null");
  }

  prepare_append(length);
  const auto old_size = storage_.size();
  storage_.resize(old_size + length);
  std::memcpy(storage_.data() + old_size, data, length);
}

void Buffer::append(const std::string_view data) {
  append(data.data(), data.size());
}

void Buffer::retrieve(const std::size_t length) noexcept {
  assert(length <= readable_bytes());
  if (length == readable_bytes()) {
    retrieve_all();
    return;
  }
  reader_index_ += length;
}

void Buffer::retrieve_all() noexcept {
  storage_.clear();
  reader_index_ = 0;
}

void Buffer::prepare_append(const std::size_t length) {
  const auto readable = readable_bytes();
  if (length > storage_.max_size() - readable) {
    throw std::length_error("buffer size exceeds vector maximum");
  }

  if (length <= writable_bytes()) {
    return;
  }

  if (length <= reader_index_ + writable_bytes()) {
    std::memmove(storage_.data(), storage_.data() + reader_index_, readable);
    storage_.resize(readable);
    reader_index_ = 0;
    return;
  }

  const auto required = readable + length;
  const auto doubled =
      storage_.capacity() <= std::numeric_limits<std::size_t>::max() / 2
          ? storage_.capacity() * 2
          : storage_.max_size();
  std::vector<char> replacement;
  replacement.reserve(std::max(required, doubled));
  replacement.insert(replacement.end(),
                     storage_.begin() +
                         static_cast<std::ptrdiff_t>(reader_index_),
                     storage_.end());
  storage_.swap(replacement);
  reader_index_ = 0;
}

} // namespace highcache
