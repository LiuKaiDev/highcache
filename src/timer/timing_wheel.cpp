#include "highcache/timer/timing_wheel.h"

#include <cassert>
#include <utility>

namespace highcache {

void TimingWheel::schedule(std::string key, const std::uint64_t generation,
                           const std::chrono::milliseconds ttl) {
  assert(ttl.count() > 0);

  const auto ttl_milliseconds = static_cast<std::uint64_t>(ttl.count());
  const auto resolution_milliseconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(tick_resolution)
          .count());
  const auto ticks = ttl_milliseconds / resolution_milliseconds +
                     (ttl_milliseconds % resolution_milliseconds != 0);
  const auto target_slot = static_cast<std::size_t>(
      (current_slot_ + ticks % slot_count) % slot_count);
  const auto remaining_rounds = (ticks - 1) / slot_count;

  slots_[target_slot].push_back(
      TimerEvent{std::move(key), generation, remaining_rounds});
}

std::vector<TimerEvent> TimingWheel::tick() {
  const auto next_slot = (current_slot_ + 1) % slot_count;
  auto &events = slots_[next_slot];

  std::vector<TimerEvent> expired;
  expired.reserve(events.size());
  current_slot_ = next_slot;

  for (auto event = events.begin(); event != events.end();) {
    if (event->remaining_rounds > 0) {
      --event->remaining_rounds;
      ++event;
      continue;
    }

    expired.push_back(std::move(*event));
    event = events.erase(event);
  }

  return expired;
}

} // namespace highcache
