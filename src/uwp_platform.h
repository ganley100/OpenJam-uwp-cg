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

// The game root on the console. The package holds only the program; the game
// lives in LocalState\game, unpacked there on first launch from the Xbox Live
// container the player copied into LocalState through Device Portal. Empty if
// there is neither. What happened is written to LocalState\status.txt, since
// this runs before there is a log.
std::filesystem::path NbaUwpGameRoot();

// With no game found, say what to do on screen and end the app, rather than
// leaving the player at a splash screen that closes. Does not return.
[[noreturn]] void NbaUwpNoGame();
