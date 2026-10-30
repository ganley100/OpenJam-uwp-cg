// NBA JAM: On Fire Edition - a controller drawn on the screen
//
// An iPhone has no controller unless one is paired, so the game gets one on
// the glass: a stick wherever the left thumb lands, the four face buttons
// under the right thumb, shoulders and triggers along the top corners, and
// Back and Start between them. To the game it is the first controller
// plugged in; a real one, if paired, is the second, through the runtime's own
// SDL driver.
//
// Fingers are followed from SDL's event stream directly, with a watch rather
// than the runtime's window events, because the runtime's SDL window does not
// pass touches on. Each finger is placed once, when it lands: on the left of
// the screen it becomes the stick, centred where it touched; anywhere else it
// presses whatever button is under it, and can slide from one to the next.
//
// The layout is in fractions of the screen's height, so the buttons are the
// same size under a thumb on any iPhone, and it is drawn with ImGui over the
// game's frame.

#include "ios_touch.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

namespace {

// A button: its centre in screen fractions (x of the width, y of the height),
// its radius as a fraction of the height, and what it presses.
struct Button {
  float x, y, r;
  uint16_t bits;
  const char* label;
  uint8_t trigger;  // 1 left, 2 right, else a button
};

constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
constexpr uint16_t kLB = 0x0100, kRB = 0x0200, kStart = 0x0010, kBack = 0x0020;

// Face buttons in the Xbox diamond under the right thumb; the corners for
// the shoulders, which on a phone held sideways are under the index fingers
// or reachable by the thumbs.
const Button kButtons[] = {
    {0.885f, 0.80f, 0.085f, kA, "A", 0},
    {0.955f, 0.64f, 0.085f, kB, "B", 0},
    {0.815f, 0.64f, 0.085f, kX, "X", 0},
    {0.885f, 0.48f, 0.085f, kY, "Y", 0},
    {0.07f, 0.10f, 0.075f, kLB, "LB", 0},
    {0.93f, 0.10f, 0.075f, kRB, "RB", 0},
    {0.07f, 0.28f, 0.075f, 0, "LT", 1},
    {0.93f, 0.28f, 0.075f, 0, "RT", 2},
    {0.42f, 0.07f, 0.05f, kBack, "BACK", 0},
    {0.58f, 0.07f, 0.05f, kStart, "START", 0},
};

// Where the stick may start: the left part of the screen, below the corner
// buttons.
constexpr float kStickZoneRight = 0.45f;
constexpr float kStickRadius = 0.14f;   // of the height: full deflection

struct Finger {
  SDL_FingerID id;
  float x, y;            // now, in pixels
  bool stick;
  float ox, oy;          // where the stick is centred, in pixels
};

struct Touch {
  std::mutex mutex;
  std::vector<Finger> fingers;
  float w = 1, h = 1;    // the window, in pixels
  uint32_t packet = 0;
};

Touch& T() {
  static Touch t;
  return t;
}

// Whether a point, in pixels, is on a button.
bool On(const Button& b, float x, float y, float w, float h) {
  const float dx = x - b.x * w, dy = y - b.y * h, r = b.r * h * 1.15f;
  return dx * dx + dy * dy <= r * r;
}

bool OnAnyButton(float x, float y, float w, float h) {
  for (const Button& b : kButtons) {
    if (On(b, x, y, w, h)) return true;
  }
  return false;
}

bool SDLCALL Watch(void*, SDL_Event* e) {
  if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION &&
      e->type != SDL_EVENT_FINGER_UP && e->type != SDL_EVENT_FINGER_CANCELED) {
    return true;
  }
  Touch& t = T();
  std::lock_guard lock(t.mutex);
  if (SDL_Window* win = SDL_GetWindowFromID(e->tfinger.windowID)) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    if (w > 0 && h > 0) {
      t.w = float(w);
      t.h = float(h);
    }
  }
  const float x = e->tfinger.x * t.w, y = e->tfinger.y * t.h;
  auto it = std::find_if(t.fingers.begin(), t.fingers.end(),
                         [&](const Finger& f) { return f.id == e->tfinger.fingerID; });
  if (e->type == SDL_EVENT_FINGER_DOWN) {
    if (it == t.fingers.end()) {
      const bool stick = x < kStickZoneRight * t.w && !OnAnyButton(x, y, t.w, t.h);
      t.fingers.push_back({e->tfinger.fingerID, x, y, stick, x, y});
    }
  } else if (e->type == SDL_EVENT_FINGER_MOTION) {
    if (it != t.fingers.end()) {
      it->x = x;
      it->y = y;
    }
  } else if (it != t.fingers.end()) {
    t.fingers.erase(it);
  }
  return true;
}

struct PadState {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0;
  bool stick = false;
  float sx = 0, sy = 0, kx = 0, ky = 0;   // stick centre and knob, pixels
};

PadState Read() {
  Touch& t = T();
  std::lock_guard lock(t.mutex);
  PadState s;
  const float full = kStickRadius * t.h;
  for (const Finger& f : t.fingers) {
    if (f.stick) {
      float dx = f.x - f.ox, dy = f.y - f.oy;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > full) {
        dx *= full / len;
        dy *= full / len;
      }
      s.stick = true;
      s.sx = f.ox;
      s.sy = f.oy;
      s.kx = f.ox + dx;
      s.ky = f.oy + dy;
      s.lx = int16_t(std::clamp(dx / full, -1.0f, 1.0f) * 32767.0f);
      s.ly = int16_t(std::clamp(-dy / full, -1.0f, 1.0f) * 32767.0f);
      continue;
    }
    for (const Button& b : kButtons) {
      if (!On(b, f.x, f.y, t.w, t.h)) continue;
      if (b.trigger == 1) s.lt = 255;
      else if (b.trigger == 2) s.rt = 255;
      else s.buttons |= b.bits;
    }
  }
  return s;
}

}  // namespace

namespace rex::input::openjam {

constexpr DeviceId kTouchDevice = static_cast<DeviceId>(0x544F5543);  // "TOUC"

class TouchInputDriver final : public InputDriver {
 public:
  TouchInputDriver() : InputDriver(nullptr, 0) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kTouchDevice;
    info.name = "Touch screen";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out) override {
    if (id != kTouchDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out) return X_ERROR_SUCCESS;
    const PadState s = Read();
    std::memset(out, 0, sizeof(*out));
    out->packet_number = ++T().packet;
    out->gamepad.buttons = s.buttons;
    out->gamepad.left_trigger = s.lt;
    out->gamepad.right_trigger = s.rt;
    out->gamepad.thumb_lx = s.lx;
    out->gamepad.thumb_ly = s.ly;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    if (id != kTouchDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;      // gamepad
      caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xFFFF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kTouchDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    return id == kTouchDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }
};

}  // namespace rex::input::openjam

namespace {

class TouchOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit TouchOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const PadState s = Read();
    // ImGui's units are the window's points; the fingers are in pixels.
    float w, h;
    {
      Touch& t = T();
      std::lock_guard lock(t.mutex);
      w = t.w;
      h = t.h;
    }
    const float scale = io.DisplaySize.y > 0 ? io.DisplaySize.y / h : 1.0f;
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    (void)w;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 idle = IM_COL32(255, 255, 255, 60), held = IM_COL32(255, 160, 40, 150);
    const ImU32 text = IM_COL32(255, 255, 255, 170);
    for (const Button& b : kButtons) {
      const bool down = b.trigger == 1   ? s.lt != 0
                        : b.trigger == 2 ? s.rt != 0
                                         : (s.buttons & b.bits) != 0;
      const ImVec2 c(b.x * W, b.y * H);
      const float r = b.r * H;
      dl->AddCircleFilled(c, r, down ? held : idle, 32);
      dl->AddCircle(c, r, IM_COL32(255, 255, 255, 110), 32, 2.0f);
      const ImVec2 ts = ImGui::CalcTextSize(b.label);
      dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), text, b.label);
    }
    if (s.stick) {
      const ImVec2 c(s.sx * scale, s.sy * scale);
      dl->AddCircleFilled(c, kStickRadius * H, IM_COL32(255, 255, 255, 40), 48);
      dl->AddCircleFilled(ImVec2(s.kx * scale, s.ky * scale), kStickRadius * H * 0.45f,
                          IM_COL32(255, 255, 255, 120), 32);
    } else {
      // Where the stick will be, until a thumb puts it somewhere.
      const ImVec2 c(0.17f * W, 0.70f * H);
      dl->AddCircle(c, kStickRadius * H, IM_COL32(255, 255, 255, 50), 48, 2.0f);
    }
  }
};

}  // namespace

void NbaTouchInstall() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  // Touches stay touches: not also a mouse, which would click ImGui.
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  SDL_AddEventWatch(Watch, nullptr);
}

std::unique_ptr<rex::input::InputSystem> NbaTouchInputSystem(bool tool_mode) {
  namespace in = rex::input;
  // X_STATUS_SUCCESS names rex's X_STATUS unqualified.
  using rex::X_STATUS;
  auto input = std::make_unique<in::InputSystem>(nullptr);
  if (!tool_mode) {
    input->AddDriver(std::make_unique<in::openjam::TouchInputDriver>());
    auto sdl_driver = std::make_unique<in::sdl::SDLInputDriver>(nullptr, 0);
    if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(sdl_driver));
    }
  }
  input->AddDriver(std::make_unique<in::nop::NopInputDriver>(nullptr, tool_mode ? 0 : 1));
  input->SetDeviceAssignment(std::make_unique<in::SlotAssignment>());
  return input;
}

void NbaTouchCreate(rex::ui::ImGuiDrawer* drawer) {
  if (drawer) new TouchOverlay(drawer);
}

bool NbaTouchPad(uint16_t* buttons, uint8_t* left_trigger) {
  const PadState s = Read();
  uint16_t b = s.buttons;
  // The chooser steers with the d-pad; the stick stands in for it.
  if (s.ly > 16000) b |= 0x0001;
  if (s.ly < -16000) b |= 0x0002;
  if (s.lx < -16000) b |= 0x0004;
  if (s.lx > 16000) b |= 0x0008;
  *buttons = b;
  *left_trigger = s.lt;
  return true;
}
