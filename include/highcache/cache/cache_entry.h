#pragma once

#include <string>

namespace highcache {

class CacheEntry final {
public:
  explicit CacheEntry(std::string value);

  [[nodiscard]] const std::string &value() const noexcept;
  void replace_value(std::string value) noexcept;

private:
  std::string value_;
};

} // namespace highcache
