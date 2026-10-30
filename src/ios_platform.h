// NBA JAM: On Fire Edition - what the iPhone build does instead
//
// See ios_platform.mm.

#pragma once

#include <filesystem>
#include <string>

// The app's Documents folder: what the Files app shows as this app's folder
// under On My iPhone, and the one place it can write.
std::filesystem::path NbaIosDocuments();

// The game to play, found or unpacked into Documents/game; empty when there
// is none yet.
std::filesystem::path NbaIosGameRoot();

// "" for the base game, or the mod this app plays ("2020s", "Legends").
std::string NbaIosVariant();

// Says how to get the game onto the phone, then quits.
[[noreturn]] void NbaIosNoGame();
