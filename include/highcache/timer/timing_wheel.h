#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <vector>

namespace highcache {

struct TimerEvent final {
  std::string key;
  std::uint64_t generation;
  std::uint64_t remaining_rounds;
};

class TimingWheel final {
public:
  static constexpr std::size_t slot_count = 60;
  static constexpr std::chrono::seconds tick_resolution{1};

  void schedule(std::string key, std::uint64_t generation,
                std::chrono::milliseconds ttl);
  [[nodiscard]] std::vector<TimerEvent> tick();

private:
  std::array<std::list<TimerEvent>, slot_count> slots_;
  std::size_t current_slot_{0};
};

} // namespace highcache
