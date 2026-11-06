#pragma once
#include <cstdint>

namespace charging {
// Volatile, bounded phone-access window. Never marks cached telemetry fresh.
struct BmsHandoff {
  enum class State : uint8_t { IDLE, PAUSED, RECONNECTING };
  static constexpr uint32_t PAUSE_MS = 300000, RECONNECT_MS = 45000;
  State state{State::IDLE};
  uint32_t started_ms{0};
  bool active() const { return state != State::IDLE; }
  bool paused() const { return state == State::PAUSED; }
  bool start(uint32_t now, bool safe, bool mains_on) {
    if (active() || !safe || !mains_on) return false;
    state = State::PAUSED;
    started_ms = now;
    return true;
  }
  void reconnect(uint32_t now) {
    if (!paused()) return;  // Repeated requests cannot extend the grace period.
    state = State::RECONNECTING;
    started_ms = now;
  }
  void stop() { state = State::IDLE; }
  uint32_t remaining_seconds(uint32_t now) const {
    if (!active()) return 0;
    auto limit = paused() ? PAUSE_MS : RECONNECT_MS;
    auto elapsed = uint32_t(now - started_ms);
    return elapsed >= limit ? 0 : (limit - elapsed + 999) / 1000;
  }
  void update(uint32_t now, bool received_battery, bool fault) {
    if (!active()) return;
    if (fault) { stop(); return; }
    if (paused() && uint32_t(now - started_ms) >= PAUSE_MS) reconnect(now);
    else if (!paused() && (received_battery || uint32_t(now - started_ms) >= RECONNECT_MS)) stop();
  }
};
static_assert(sizeof(BmsHandoff) <= 16, "Phone handoff must remain a small volatile state");
}  // namespace charging
