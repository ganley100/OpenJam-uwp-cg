// NBA JAM: On Fire Edition - what the iPhone build does instead
//
// Built in place of mod_swap.cpp and watchdog.cpp when NBAJAM_IOS is on, as
// uwp_platform.cpp is on the Xbox, and for the same reasons: an iPhone app
// cannot start another process, so each game - the base game and each mod -
// is an app of its own, and switching mod is switching app.
//
// The app carries only the program. The game goes in its Documents folder,
// which the Files app shows under On My iPhone: the Xbox Live download for
// the base game, or a pack from tools/xbox_pack.py for a mod. The first start
// after that unpacks it into Documents/game.

#include "ios_platform.h"
#include "mod_swap.h"
#include "unpack_container.h"
#include "watchdog.h"

#import <Foundation/Foundation.h>

#include <SDL3/SDL.h>

#include <cstdlib>
#include <fstream>

#include <rex/logging.h>

std::filesystem::path NbaIosDocuments() {
  @autoreleasepool {
    NSArray<NSString*>* dirs =
        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    if (dirs.count == 0) return {};
    return std::filesystem::path([dirs[0] fileSystemRepresentation]);
  }
}

std::string NbaIosVariant() {
  static const std::string variant = [] {
    @autoreleasepool {
      id v = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"OpenJamVariant"];
      return std::string([v isKindOfClass:[NSString class]] ? [(NSString*)v UTF8String] : "");
    }
  }();
  return variant;
}

std::filesystem::path NbaIosGameRoot() {
  namespace fs = std::filesystem;
  static bool decided = false;
  static fs::path decided_root;
  if (decided) return decided_root;
  decided = true;
  decided_root = []() -> fs::path {
    const fs::path docs = NbaIosDocuments();
    if (docs.empty()) return {};
    std::ofstream status(docs / "status.txt");
    const fs::path game = docs / "game";
    std::error_code ec;
    if (fs::exists(game / "default.xex", ec)) {
      status << "game: " << game.string() << "\n";
      return game;
    }
    const fs::path pack = NbaFindPack(docs);
    if (!pack.empty()) {
      status << "unpacking " << pack.filename().string() << " into " << game.string() << "\n";
      status.flush();
      const int n = NbaUnpackPack(pack, game, status);
      if (n <= 0 || !fs::exists(game / "default.xex", ec)) {
        status << "unpacking failed (" << n << " file(s))\n";
        return {};
      }
      status << n << " file(s) unpacked. The .ojpk can be deleted now.\n";
      return game;
    }
    if (!NbaIosVariant().empty()) {
      status << "No game yet. Make a pack of the " << NbaIosVariant()
             << " mod with tools/xbox_pack.py and copy it into this folder.\n";
      return {};
    }
    const fs::path container = NbaFindLiveContainer(docs);
    if (container.empty()) {
      status << "No game yet. Copy the Xbox Live container for NBA JAM: On Fire\n"
                "Edition (the 42-character file with no extension) into this\n"
                "app's folder in the Files app, then start the app again.\n";
      return {};
    }
    status << "unpacking " << container.filename().string() << " into " << game.string()
           << "\n";
    status.flush();
    const int n = NbaUnpackLiveContainer(container, game);
    if (n <= 0 || !fs::exists(game / "default.xex", ec)) {
      status << "unpacking failed (" << n << " file(s))\n";
      return {};
    }
    status << n << " file(s) unpacked. The container can be deleted now.\n";
    return game;
  }();
  return decided_root;
}

void NbaIosNoGame() {
  const std::string variant = NbaIosVariant();
  const std::string text =
      variant.empty()
          ? "No game found.\n\nOpen the Files app, go to On My iPhone > NBA JAM, and copy "
            "your Xbox Live download of NBA JAM: On Fire Edition there (the file with a "
            "42-character name and no extension, about 850 MB).\n\nThen start the app "
            "again. The first start after that unpacks the game, which takes a minute or "
            "two."
          : "No game found.\n\nThis app plays the " + variant +
                " mod. Make a pack of it on your PC - python tools/xbox_pack.py <the mod's "
                "staged folder> " + variant +
                ".ojpk - and copy the .ojpk into this app's folder in the Files app (On My "
                "iPhone).\n\nThen start the app again. The first start after that unpacks "
                "it, which takes a minute or two.";
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "NBA JAM: On Fire Edition", text.c_str(),
                           nullptr);
  std::exit(0);
}

bool NbaModList(std::vector<NbaMod>* mods, size_t* active) {
  mods->clear();
  *active = 0;
  return false;
}

void NbaModSwapTo(const NbaMod& mod) {
  REXLOG_INFO("mod swap: '{}' is a separate app on this phone", mod.name);
}

void NbaModRestart() {
  REXLOG_INFO("restart: not available to an app; close it from the app switcher");
}

void NbaModSwapUseRoot(const std::filesystem::path&) {}

void NbaModSwapWaitForPredecessor() {}

bool NbaModSwapIsRelaunch() { return false; }

void NbaModSwapTakeForeground(void*) {}

void NbaStartWatchdog() {}

void NbaWatchdogWatchWindow(void*) {}

void NbaWatchdogCreate(rex::ui::ImGuiDrawer*) {}
