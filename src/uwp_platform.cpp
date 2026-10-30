// NBA JAM: On Fire Edition - what the Xbox build does instead
//
// Built in place of mod_swap.cpp and watchdog.cpp when NBAJAM_UWP is on. See
// XBOX.md for the build as a whole.
//
// On a PC, choosing a mod relaunches this executable against that mod's game
// root, and the watchdog's way out of a stopped game is the same relaunch.
// A UWP app cannot do either: it may not start a process, its own included,
// and the console gives each app one instance. So on the Xbox each game - the
// base game and each mod - is packaged as an app of its own, with that game's
// root as the package. Switching mod is switching app, from the console's
// own menu.
//
// What is left here is the same interface with nothing to swap to. There is
// no mods.list in a package, so the chooser finds an empty list and does
// nothing when opened.
//
// The watchdog walks other threads' stacks with DbgHelp and offers a restart,
// and neither is available to an app. A game that stops on the console is
// closed from the console's own menu, which is what that menu is for.

#include "mod_swap.h"
#include "ps3_mod.h"
#include "unpack_container.h"
#include "uwp_platform.h"
#include "watchdog.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <string>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Popups.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/logging.h>

std::filesystem::path NbaUwpLocalState() {
  try {
    return std::filesystem::path(std::wstring(
        winrt::Windows::Storage::ApplicationData::Current()
            .LocalFolder()
            .Path()));
  } catch (const winrt::hresult_error&) {
    return {};
  }
}

namespace {

// Which of the three apps this is, from its package name: "" for the base
// game, or the mod's name ("2020s", "Legends"), the part after the last dot
// of OpenJam.NBAJamOFE.<mod>.
std::string Variant() {
  try {
    const std::wstring name(
        winrt::Windows::ApplicationModel::Package::Current().Id().Name());
    const std::wstring base = L"OpenJam.NBAJamOFE";
    if (name.size() > base.size() + 1 && name.compare(0, base.size(), base) == 0) {
      std::string out;
      for (wchar_t ch : name.substr(base.size() + 1)) out += char(ch);
      return out;
    }
  } catch (...) {
  }
  return {};
}

}  // namespace

std::string NbaUwpVariant() {
  static const std::string variant = Variant();
  return variant;
}

namespace {

// What a mod app was last given, so it is applied once: the names and sizes
// of the packages or the folder it came from.
std::string ModSignature(const std::vector<std::filesystem::path>& pkgs,
                         const std::filesystem::path& folder) {
  std::string sig;
  std::error_code ec;
  for (const auto& p : pkgs) {
    sig += p.filename().string() + " " + std::to_string(std::filesystem::file_size(p, ec)) + "\n";
  }
  if (!folder.empty()) sig += folder.filename().string() + "/\n";
  return sig;
}

std::string ReadText(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// Where the game comes from, first match wins:
//
//   LocalState\default.xex        loose files, played where they are
//   LocalState\game\default.xex   loose or already unpacked, played as is
//   LocalState\*.ojpk             a pack from tools/xbox_pack.py
//   the Xbox Live download         unpacked into LocalState\game
//   LocalState\*.iso              an Xbox 360 disc image, unpacked the same
//
// A mod app then lays its mod over that, once: the PS3 .pkg files in
// LocalState, or an already opened mod in LocalState\mod (src/ps3_mod.cpp).
// A mod app that unpacked the plain game itself and found no mod to put on
// it stops there, rather than passing the base game off as the mod.
std::filesystem::path NbaUwpGameRoot() {
  namespace fs = std::filesystem;
  // Asked more than once while starting; the unpack must only happen once.
  static bool decided = false;
  static fs::path decided_root;
  if (decided) return decided_root;
  decided = true;
  decided_root = []() -> fs::path {
    const fs::path local = NbaUwpLocalState();
    if (local.empty()) return {};
    std::ofstream status(local / "status.txt");
    const std::string variant = NbaUwpVariant();
    std::error_code ec;

    fs::path root;
    bool unpacked_base = false;  // the plain game, by this app
    if (fs::exists(local / "default.xex", ec)) {
      root = local;
      status << "game: loose files in " << root.string() << "\n";
    } else if (fs::exists(local / "game" / "default.xex", ec)) {
      root = local / "game";
      status << "game: " << root.string() << "\n";
    } else {
      const fs::path game = local / "game";
      int n = 0;
      std::string what;
      if (const fs::path pack = NbaFindPack(local); !pack.empty()) {
        // A pack is a whole game root, a mod's already applied.
        status << "unpacking " << pack.filename().string() << " into " << game.string() << "\n";
        status.flush();
        n = NbaUnpackPack(pack, game, status);
        what = ".ojpk";
        if (n > 0) std::ofstream(game / "openjam_mod.txt") << "pack " << pack.filename().string() << "\n";
      } else if (const fs::path container = NbaFindLiveContainer(local); !container.empty()) {
        status << "unpacking " << container.filename().string() << " into " << game.string() << "\n";
        status.flush();
        n = NbaUnpackLiveContainer(container, game);
        what = "container";
        unpacked_base = true;
      } else if (const fs::path iso = NbaFindDiscImage(local); !iso.empty()) {
        status << "unpacking " << iso.filename().string() << " into " << game.string() << "\n";
        status.flush();
        n = NbaUnpackDiscImage(iso, game);
        what = ".iso";
        unpacked_base = true;
      } else {
        status << "No game yet. Copy the Xbox Live container for NBA JAM: On Fire\n"
                  "Edition (the 42-character file with no extension), a .iso, an\n"
                  ".ojpk pack, or the unpacked game folder into this folder with\n"
                  "Device Portal's File Explorer, then start the app again.\n";
        return {};
      }
      if (n <= 0 || !fs::exists(game / "default.xex", ec)) {
        status << "unpacking failed (" << n << " file(s))\n";
        return {};
      }
      status << n << " file(s) unpacked. The " << what << " can be deleted now.\n";
      if (unpacked_base) std::ofstream(game / "openjam_unpacked.txt") << what << "\n";
      root = game;
    }

    if (variant.empty()) return root;

    // The mod.
    const std::vector<fs::path> pkgs = NbaFindPs3Pkgs(local);
    const fs::path folder = fs::is_directory(local / "mod", ec) ? local / "mod" : fs::path();
    const fs::path marker = root / "openjam_mod.txt";
    if (pkgs.empty() && folder.empty()) {
      // The plain game, unpacked by this app, with nothing put on it yet.
      if (fs::exists(root / "openjam_unpacked.txt", ec) && !fs::exists(marker, ec)) {
        status << "No " << variant << " mod yet. Copy the mod's PS3 .pkg file(s) into\n"
                  "this folder, or a pack from tools/xbox_pack.py, then start the app\n"
                  "again. The game is unpacked already and stays.\n";
        return {};
      }
      if (!fs::exists(marker, ec)) {
        status << "mod: none given; playing the files as they are\n";
      }
      return root;
    }
    const std::string sig = ModSignature(pkgs, folder);
    if (ReadText(marker) == sig) {
      status << "mod: already applied\n";
      return root;
    }
    fs::path opened = folder;
    const fs::path scratch = local / "mod_ps3";
    if (opened.empty()) {
      fs::remove_all(scratch, ec);
      for (const auto& p : pkgs) {
        status << "opening " << p.filename().string() << "\n";
        status.flush();
        if (NbaExtractPs3Pkg(p, scratch, status) < 0) {
          status << "could not open " << p.filename().string() << "\n";
          return {};
        }
      }
      opened = scratch;
    }
    status << "applying the " << variant << " mod to " << root.string() << "\n";
    status.flush();
    const int n = NbaApplyPs3Mod(opened, root, status);
    fs::remove_all(scratch, ec);
    if (n <= 0) {
      status << "the mod did not apply\n";
      return {};
    }
    std::ofstream(marker, std::ios::binary) << sig;
    status << "The mod is applied. The .pkg file(s) can be deleted now; the app\n"
              "remembers it was applied.\n";
    return root;
  }();
  return decided_root;
}

void NbaUwpNoGame() {
  using namespace winrt::Windows::UI;
  const std::wstring local = NbaUwpLocalState().wstring();
  const std::string variant = NbaUwpVariant();
  const std::wstring wvariant(variant.begin(), variant.end());
  const std::wstring text =
      variant.empty()
          ? L"No game found.\n\n"
            L"Copy one of these into this app's LocalState folder with Device "
            L"Portal's File Explorer:\n\n"
            L"- your Xbox Live download of NBA JAM: On Fire Edition (the file with a "
            L"42-character name and no extension, about 850 MB)\n"
            L"- a disc image of it (.iso)\n"
            L"- an .ojpk pack from tools/xbox_pack.py\n"
            L"- the unpacked game itself, default.xex and data, in a folder named game\n\n" +
                local +
                L"\n\nThen start the app again. The first start after that unpacks the "
                L"game, which takes a minute or two. status.txt in that folder says "
                L"how it went."
          : L"No game found.\n\n"
            L"This app plays the " + wvariant + L" mod. Copy into this app's "
            L"LocalState folder with Device Portal's File Explorer:\n\n"
            L"- the game: your Xbox Live download (the 42-character file with no "
            L"extension) or a .iso\n"
            L"- the mod: its PS3 .pkg file(s), as the mod is published\n\n"
            L"or instead an .ojpk pack of the mod from tools/xbox_pack.py.\n\n" +
                local +
                L"\n\nThen start the app again. The first start unpacks the game and "
                L"applies the mod, which takes a few minutes. status.txt in that "
                L"folder says how it went.";
  bool done = false;
  try {
    Popups::MessageDialog dialog(winrt::hstring(text), L"NBA JAM: On Fire Edition");
    dialog.ShowAsync().Completed([&done](auto&&, auto&&) { done = true; });
    // Pump the window's events here, on its thread, until the dialog closes.
    auto dispatcher = Core::CoreWindow::GetForCurrentThread().Dispatcher();
    while (!done) {
      dispatcher.ProcessEvents(Core::CoreProcessEventsOption::ProcessOneIfPresent);
      Sleep(16);
    }
  } catch (...) {
  }
  winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
  std::exit(0);
}

bool NbaModList(std::vector<NbaMod>* mods, size_t* active) {
  mods->clear();
  *active = 0;
  return false;
}

void NbaModSwapTo(const NbaMod& mod) {
  REXLOG_INFO("mod swap: '{}' is a separate app on this console", mod.name);
}

void NbaModRestart() {
  REXLOG_INFO("restart: not available to an app; close it from the console");
}

void NbaModSwapUseRoot(const std::filesystem::path&) {}

void NbaModSwapWaitForPredecessor() {}

bool NbaModSwapIsRelaunch() { return false; }

void NbaModSwapTakeForeground(void*) {}

void NbaStartWatchdog() {}

void NbaWatchdogWatchWindow(void*) {}

void NbaWatchdogCreate(rex::ui::ImGuiDrawer*) {}
