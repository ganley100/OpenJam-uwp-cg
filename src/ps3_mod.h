// NBA JAM: On Fire Edition - a PS3 mod, applied on the console itself
//
// See ps3_mod.cpp.

#pragma once

#include <filesystem>
#include <iosfwd>
#include <vector>

// Opens a PlayStation 3 .pkg into `out`. Returns the file count, or -1.
int NbaExtractPs3Pkg(const std::filesystem::path& pkg, const std::filesystem::path& out,
                     std::ostream& status);

// Applies a PS3 mod to an unpacked game, in place. `mod` is the mod's own
// folder as a package opens out: the one with data/ in it (a USRDIR), or a
// folder above one. Returns the number of files applied, or -1.
int NbaApplyPs3Mod(const std::filesystem::path& mod, const std::filesystem::path& game,
                   std::ostream& status);

// Every .pkg in a folder, largest first: a mod ships as a large part and
// sometimes a small one.
std::vector<std::filesystem::path> NbaFindPs3Pkgs(const std::filesystem::path& folder);

// Converts one PS3 .ast archive's textures to this build's format. `stock` is
// the base game's copy of the same archive, or empty. True when `dst` was
// written.
bool NbaRepackAst(const std::filesystem::path& src, const std::filesystem::path& dst,
                  const std::filesystem::path& stock);
