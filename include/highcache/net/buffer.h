#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace highcache {

class Buffer final {
public:
  explicit Buffer(std::size_t initial_capacity = 4096);

  [[nodiscard]] const char *data() const noexcept;
  [[nodiscard]] std::string_view readable_view() const noexcept;
  [[nodiscard]] std::size_t readable_bytes() const noexcept;
  [[nodiscard]] std::size_t writable_bytes() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

  void append(const void *data, std::size_t length);
  void append(std::string_view data);
  void retrieve(std::size_t length) noexcept;
  void retrieve_all() noexcept;

private:
  void prepare_append(std::size_t length);

  std::vector<char> storage_;
  std::size_t reader_index_{0};
};

} // namespace highcache
