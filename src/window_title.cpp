// NBA JAM: On Fire Edition - what the window calls itself
//
// The window came up named after the executable, `nbajam_ofe`, which is what
// the runtime falls back to. It says the game's name now, and carries the
// frame rate after it.
//
// The number is the game's frame rate, not the window's. ImGui hands out a
// smoothed rate for free and it is the wrong one: it counts presented frames,
// which on a 120 Hz display is 120 whatever the game is doing, so it says the
// same thing whether the game is running well or badly.
//
// What is counted instead is the guest's own frame, at the one point in it
// this port already stands - the pad read that src/scripted_input.cpp sits on,
// which the game does once a frame. The rate is then frames over the wall
// clock between one rewrite and the next.
//
// The cost of that choice is that a loading screen the game does not poll
// through reads as nothing, because from the game's side nothing is happening.
//
// The dialog is only there to be called every frame; it draws nothing. The
// title is rewritten twice a second, which is often enough to watch and rare
// enough not to spend a window message per frame on.

#include "window_title.h"

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

#include <rex/cvar.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>

namespace {

const std::string kTitle = "NBA JAM: On Fire Edition PC";

constexpr float kRewriteEvery = 0.5f;   // seconds

// Written from a guest thread, read from the UI thread.
std::atomic<uint64_t> g_guest_frames{0};

class FrameRateTitle : public rex::ui::ImGuiDialog {
 public:
  FrameRateTitle(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window)
      : ImGuiDialog(drawer), window_(window) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    since_ += io.DeltaTime;
    if (since_ < kRewriteEvery) {
      return;
    }
    const uint64_t frames = g_guest_frames.load(std::memory_order_relaxed);
    const double fps = double(frames - last_frames_) / double(since_);
    last_frames_ = frames;
    since_ = 0.0f;
    char text[128];
    std::snprintf(text, sizeof(text), "%s - %.0f fps", kTitle.c_str(), fps);
    window_->SetTitle(text);
  }

 private:
  rex::ui::Window* window_ = nullptr;
  uint64_t last_frames_ = 0;
  float since_ = kRewriteEvery;   // so the first frame writes it
};

}  // namespace

// A cap on the game's own frame rate: the pad poll that counts a frame
// holds the guest there until the frame's share of a second has passed. The
// Xbox build sets it to 30 (xbox/game/nbajam_ofe.toml), for an even pace on
// the console's CPU rather than one that swings with how busy the court is.
REXCVAR_DEFINE_INT32(frame_rate_limit, 0, "Game",
                     "Cap the game's frame rate, in frames a second; 0 for no cap");

namespace {

// Waits until `until`. Windows' ordinary sleep is good to 15 ms or so, which
// is half a frame at 30; a high-resolution timer is good to well under one.
void WaitUntil(std::chrono::steady_clock::time_point until) {
#if defined(_WIN32)
  static thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const auto left = until - std::chrono::steady_clock::now();
  if (timer && left > std::chrono::microseconds(500)) {
    LARGE_INTEGER due;
    due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100;
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
    }
  }
  // The last fraction, and everything where there is no such timer.
  while (std::chrono::steady_clock::now() < until) {
    std::this_thread::yield();
  }
#else
  std::this_thread::sleep_until(until);
#endif
}

void LimitFrameRate() {
  const int32_t fps = REXCVAR_GET(frame_rate_limit);
  if (fps <= 0) return;
  using clock = std::chrono::steady_clock;
  const auto period = std::chrono::nanoseconds(1000000000LL / fps);
  static clock::time_point next{};
  const auto now = clock::now();
  if (next == clock::time_point{} || now > next + period * 4) {
    // First frame, or far behind (a load, a suspend): start the beat again
    // from here rather than racing to catch up.
    next = now + period;
    return;
  }
  if (now < next) WaitUntil(next);
  next += period;
}

}  // namespace

void NbaGuestFrame() {
  LimitFrameRate();
  g_guest_frames.fetch_add(1, std::memory_order_relaxed);
}

unsigned long long NbaGuestFrameCount() {
  return g_guest_frames.load(std::memory_order_relaxed);
}

const std::string& NbaWindowTitle() { return kTitle; }

void NbaTrackFrameRateInTitle(rex::ui::ImGuiDrawer* drawer,
                              rex::ui::Window* window) {
  if (!drawer || !window) {
    return;
  }
  // A dialog adds itself to the drawer and is owned by it from then on; this
  // one is never closed, so it lives as long as the drawer does.
  new FrameRateTitle(drawer, window);
}
