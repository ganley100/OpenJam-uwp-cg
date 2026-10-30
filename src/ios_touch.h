// NBA JAM: On Fire Edition - a controller drawn on the screen
//
// See ios_touch.cpp.

#pragma once

#include <cstdint>
#include <memory>

namespace rex::input {
class InputSystem;
}
namespace rex::ui {
class ImGuiDrawer;
}

// Starts following fingers on the screen. Once, before the window exists.
void NbaTouchInstall();

// The runtime's input, with the on-screen pad as its first controller, then
// any real controller, then the runtime's own fallbacks.
std::unique_ptr<rex::input::InputSystem> NbaTouchInputSystem(bool tool_mode);

// Draws the pad over the game, every frame.
void NbaTouchCreate(rex::ui::ImGuiDrawer* drawer);

// What the pad has held down, in XINPUT's bits, and the left trigger. For the
// mod chooser, which reads the controller itself (src/host_pad.cpp).
bool NbaTouchPad(uint16_t* buttons, uint8_t* left_trigger);
