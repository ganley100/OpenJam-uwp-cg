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
#include "unpack_container.h"
#include "uwp_platform.h"
#include "watchdog.h"

#include <windows.h>

#include <fstream>
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
    const fs::path game = local / "game";
    std::error_code ec;
    if (fs::exists(game / "default.xex", ec)) {
      status << "game: " << game.string() << "\n";
      return game;
    }
    // A pack from tools/xbox_pack.py: what the mod apps need, and the base
    // game can take one too.
    const fs::path pack = NbaFindPack(local);
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
    // A mod app plays the mod, and an Xbox Live container is the plain game:
    // only a pack will do.
    if (!NbaUwpVariant().empty()) {
      status << "No game yet. Make a pack of the " << NbaUwpVariant()
             << " mod with tools/xbox_pack.py and copy it into this folder.\n";
      return {};
    }
    const fs::path container = NbaFindLiveContainer(local);
    if (container.empty()) {
      status << "No game yet. Copy the Xbox Live container for NBA JAM: On Fire\n"
                "Edition (the 42-character file with no extension) into this\n"
                "folder with Device Portal's File Explorer, then start the app\n"
                "again.\n";
      return {};
    }
    status << "unpacking " << container.filename().string() << " into "
           << game.string() << "\n";
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

void NbaUwpNoGame() {
  using namespace winrt::Windows::UI;
  const std::wstring local = NbaUwpLocalState().wstring();
  const std::string variant = NbaUwpVariant();
  const std::wstring wvariant(variant.begin(), variant.end());
  const std::wstring text =
      variant.empty()
          ? L"No game found.\n\n"
            L"Copy your Xbox Live download of NBA JAM: On Fire Edition (the file "
            L"with a 42-character name and no extension, about 850 MB) into this "
            L"app's LocalState folder with Device Portal's File Explorer:\n\n" +
                local +
                L"\n\nThen start the app again. The first start after that unpacks the "
                L"game, which takes a minute or two."
          : L"No game found.\n\n"
            L"This app plays the " + wvariant + L" mod. Make a pack of it on your "
            L"PC - python tools/xbox_pack.py <the mod's staged folder> " + wvariant +
                L".ojpk - and copy the .ojpk into this app's LocalState folder with "
                L"Device Portal's File Explorer:\n\n" +
                local +
                L"\n\nThen start the app again. The first start after that unpacks it, "
                L"which takes a minute or two.";
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
