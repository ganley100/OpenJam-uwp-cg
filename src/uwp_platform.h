// NBA JAM: On Fire Edition - what the Xbox build does instead
//
// See uwp_platform.cpp.

#pragma once

#include <filesystem>

// The app's own writable folder, LocalState. The package itself is installed
// read-only, so saves cannot go beside the game the way they do on a PC.
// Empty if the app has no package identity, which only happens when the
// executable is started outside its package.
std::filesystem::path NbaUwpLocalState();
