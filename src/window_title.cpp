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

#include <algorithm>
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

#if NBAJAM_UWP
#include <fstream>

#include "uwp_platform.h"
#endif

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
#if NBAJAM_UWP
    // A console shows no window title, so the rate goes in the corner.
    char corner[32];
    std::snprintf(corner, sizeof(corner), "%.0f fps", fps_);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(corner);
    dl->AddRectFilled(ImVec2(16, 16), ImVec2(28 + size.x, 24 + size.y), IM_COL32(0, 0, 0, 140), 4);
    dl->AddText(ImVec2(22, 20), fps_ >= 55 ? IM_COL32(120, 255, 120, 255)
                                           : IM_COL32(255, 200, 80, 255), corner);
#endif
    since_ += io.DeltaTime;
    if (since_ < kRewriteEvery) {
      return;
    }
    const uint64_t frames = g_guest_frames.load(std::memory_order_relaxed);
    const double fps = double(frames - last_frames_) / double(since_);
    fps_ = fps;
#if NBAJAM_UWP
    Record(fps);
#endif
    last_frames_ = frames;
    since_ = 0.0f;
    char text[128];
    std::snprintf(text, sizeof(text), "%s - %.0f fps", kTitle.c_str(), fps);
    window_->SetTitle(text);
  }

 private:
#if NBAJAM_UWP
  // fps.txt in LocalState: every five seconds, the average rate and the
  // lowest half-second in them, so a session can be read afterwards from
  // Device Portal. Started afresh each launch.
  void Record(double fps) {
    if (!log_.is_open() && !tried_) {
      tried_ = true;
      const auto local = NbaUwpLocalState();
      if (!local.empty()) {
        log_.open(local / "fps.txt", std::ios::trunc);
        log_ << "seconds  average  lowest   (the game's frames a second)\n";
      }
    }
    if (!log_.is_open()) return;
    sum_ += fps;
    lowest_ = samples_ == 0 ? fps : std::min(lowest_, fps);
    if (++samples_ < 10) return;   // ten half-seconds
    elapsed_ += 5;
    char line[64];
    std::snprintf(line, sizeof(line), "%7d  %7.1f  %6.1f\n", elapsed_, sum_ / samples_, lowest_);
    log_ << line;
    log_.flush();
    sum_ = 0.0;
    samples_ = 0;
  }

  std::ofstream log_;
  bool tried_ = false;
  double sum_ = 0.0, lowest_ = 0.0;
  int samples_ = 0, elapsed_ = 0;
#endif
  double fps_ = 0.0;
  rex::ui::Window* window_ = nullptr;
  uint64_t last_frames_ = 0;
  float since_ = kRewriteEvery;   // so the first frame writes it
};

}  // namespace

// A cap on the game's own frame rate: the pad poll that counts a frame
// holds the guest there until the frame's share of a second has passed. Off
// by default: the game's pace is tied to its frame rate, so a cap below the
// 60 it runs at slows the game itself down.
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
