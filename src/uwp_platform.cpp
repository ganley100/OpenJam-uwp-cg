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
#include "uwp_fromapp.h"
#include "uwp_platform.h"
#include "watchdog.h"

#include <windows.h>
#include <fileapifromapp.h>

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
#include <cwctype>
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

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Outside the app's own storage
//
// A USB drive and the console's Development Files folder are reached with
// broadFileSystemAccess (AppxManifest.xml). Every file call in the process is
// routed through the FromApp versions that allow it (src/uwp_fromapp.cpp), so
// the game is unpacked, modded and played where it is - D: has the room, and
// the app's own storage does not. Only saves and logs stay in LocalState.

struct XEntry {
  std::wstring name;
  bool dir;
};

std::vector<XEntry> XList(const fs::path& dir) {
  std::vector<XEntry> out;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileExFromAppW((dir / L"*").c_str(), FindExInfoBasic, &fd,
                                     FindExSearchNameMatch, nullptr, 0);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    const std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    out.push_back({name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}

bool XIsDir(const fs::path& p) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  return GetFileAttributesExFromAppW(p.c_str(), GetFileExInfoStandard, &d) &&
         (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring LowerW(std::wstring s) {
  for (wchar_t& c : s) c = wchar_t(towlower(c));
  return s;
}

// Whether a folder name is meant for this app: the exact name, or near it -
// "NBA Jam 2020", "nbajam-legends" - since a name typed in a browser is easy
// to get a character off. The base game's folder is one with "jam" in it and
// no mod's name.
bool NameFits(const std::wstring& folder, const std::string& variant) {
  const std::wstring f = LowerW(folder);
  if (f.find(L"jam") == std::wstring::npos) return false;
  const bool is_2020 = f.find(L"2020") != std::wstring::npos;
  const bool is_legends = f.find(L"legend") != std::wstring::npos;
  if (variant.empty()) return !is_2020 && !is_legends;
  if (variant == "2020s") return is_2020;
  if (variant == "Legends") return is_legends;
  return false;
}

// The folders looked in, in order: D:\DevelopmentFiles, then each drive's
// root (a USB drive).
std::vector<fs::path> ExternalRoots() {
  std::vector<fs::path> roots = {L"D:\\DevelopmentFiles"};
  for (wchar_t d = L'E'; d <= L'Z'; ++d) roots.push_back(std::wstring(1, d) + L":\\");
  return roots;
}

// The folder this app's game is in on a USB drive or in Development Files,
// or nothing: the exact name first, anywhere, then a near one.
fs::path ExternalFolder(const std::wstring& name, const std::string& variant) {
  for (const fs::path& root : ExternalRoots()) {
    if (XIsDir(root / name)) return root / name;
  }
  for (const fs::path& root : ExternalRoots()) {
    for (const XEntry& e : XList(root)) {
      if (e.dir && NameFits(e.name, variant)) return root / e.name;
    }
  }
  return {};
}

// What is in Development Files, for status.txt when nothing fits.
std::string DevelopmentFilesListing() {
  std::string out;
  for (const XEntry& e : XList(L"D:\\DevelopmentFiles")) {
    if (e.dir) out += "  " + fs::path(e.name).string() + "\\\n";
  }
  return out.empty() ? "  (nothing, or not readable)\n" : out;
}

std::wstring FolderName(const std::string& variant) {
  return variant.empty() ? L"NBA JAM" : L"NBA JAM " + std::wstring(variant.begin(), variant.end());
}

// What a mod app was last given, so it is applied once: the names and sizes
// of the packages, or the opened mod folder.
std::string ModSignature(const std::vector<fs::path>& pkgs, bool folder) {
  std::string sig;
  std::error_code ec;
  for (const auto& p : pkgs) {
    sig += p.filename().string() + " " + std::to_string(fs::file_size(p, ec)) + "\n";
  }
  if (folder) sig += "mod/\n";
  return sig;
}

std::string ReadText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The unpacked game in a folder: the folder itself or its game\, or nothing.
fs::path GameIn(const fs::path& folder) {
  std::error_code ec;
  if (folder.empty()) return {};
  if (fs::exists(folder / "default.xex", ec)) return folder;
  if (fs::exists(folder / "game" / "default.xex", ec)) return folder / "game";
  return {};
}

// Unpacks the first source in `from` - an .ojpk, the Xbox Live download or a
// .iso - into `into`. Returns the file count (0 when there is none, -1 on
// failure), and says which in `what`.
int UnpackFrom(const fs::path& from, const fs::path& into, bool packs, std::string* what,
               std::ostream& status) {
  if (from.empty()) return 0;
  fs::path src;
  int n = 0;
  if (packs && !(src = NbaFindPack(from)).empty()) {
    *what = ".ojpk";
    status << "unpacking " << src.string() << " into " << into.string() << "\n";
    status.flush();
    n = NbaUnpackPack(src, into, status);
  } else if (!(src = NbaFindLiveContainer(from)).empty()) {
    *what = "container";
    status << "unpacking " << src.string() << " into " << into.string() << "\n";
    status.flush();
    n = NbaUnpackLiveContainer(src, into);
  } else if (!(src = NbaFindDiscImage(from)).empty()) {
    *what = ".iso";
    status << "unpacking " << src.string() << " into " << into.string() << "\n";
    status.flush();
    n = NbaUnpackDiscImage(src, into);
  } else {
    return 0;
  }
  std::error_code ec;
  if (n <= 0 || !fs::exists(into / "default.xex", ec)) {
    status << "unpacking failed (" << n << " file(s))\n";
    return -1;
  }
  status << n << " file(s) unpacked\n";
  return n;
}

}  // namespace

// Where the game comes from. The app's folder is looked for first:
//
//   D:\DevelopmentFiles\<name>   the console's own folder for loose files
//   <USB drive>:\<name>          the same, on a drive
//
// where <name> is NBA JAM, NBA JAM 2020s or NBA JAM Legends (or near it).
// Everything happens in that folder, which has the room: the game there is
// played as it is (default.xex and data, in the folder or in its game\), or
// unpacked into game\ from an .ojpk, the Xbox Live download or a .iso found
// there. A mod app takes the plain game from the NBA JAM folder when its own
// has none - its unpacked game\ copied, or its download unpacked - and puts
// the mod on it once, from the PS3 .pkg file(s) in its folder (or an opened
// mod in mod\).
//
// With no such folder, LocalState is used the same way, as before.
std::filesystem::path NbaUwpGameRoot() {
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

    status << "file access: " << NbaRouteFilesThroughFromApp()
           << " file call(s) routed through FromApp\n";
    const fs::path own = ExternalFolder(FolderName(variant), variant);
    const fs::path shared = variant.empty() ? fs::path() : ExternalFolder(FolderName(""), "");
    status << "this app's folder (" << fs::path(FolderName(variant)).string()
           << "): " << (own.empty() ? "not found" : own.string()) << "\n";
    if (!variant.empty()) {
      status << "the plain game's folder (NBA JAM): "
             << (shared.empty() ? "not found" : shared.string()) << "\n";
    }
    if (own.empty()) {
      status << "folders in D:\\DevelopmentFiles:\n" << DevelopmentFilesListing();
    } else if (!fs::is_directory(own, ec)) {
      status << "but the game cannot open it directly (" << ec.message()
             << "); using LocalState instead\n";
    }
    const bool direct = !own.empty() && fs::is_directory(own, ec);
    const fs::path work = direct ? own : local;
    status << "working in " << work.string() << "\n";
    status.flush();

    // The game: here already, or unpacked here.
    fs::path root = GameIn(work);
    std::string what;
    const fs::path game = work / "game";
    const fs::path marker_name = "openjam_mod.txt", base_name = "openjam_unpacked.txt";
    if (root.empty() && direct && variant.empty()) {
      // The base app's earlier copy in LocalState still plays.
      root = GameIn(local);
    }
    if (root.empty()) {
      const int n = UnpackFrom(work, game, true, &what, status);
      if (n < 0) return {};
      if (n > 0) {
        root = game;
        if (what == ".ojpk") std::ofstream(game / marker_name) << "pack\n";
        else std::ofstream(game / base_name) << what << "\n";
      }
    }
    if (root.empty() && !shared.empty()) {
      // A mod app, from the plain game's folder.
      if (const fs::path from = GameIn(shared); !from.empty()) {
        status << "copying the game in " << from.string() << " into " << game.string() << "\n";
        status.flush();
        fs::copy(from, game, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 ec);
        fs::remove(game / marker_name, ec);
        if (!fs::exists(game / "default.xex", ec)) {
          status << "copying it failed (" << ec.message() << ")\n";
          return {};
        }
        std::ofstream(game / base_name) << "copied\n";
        root = game;
      } else {
        const int n = UnpackFrom(shared, game, false, &what, status);
        if (n < 0) return {};
        if (n > 0) {
          std::ofstream(game / base_name) << what << "\n";
          root = game;
        }
      }
    }
    if (root.empty()) {
      status << "No game yet. Put the game in a folder named "
             << fs::path(FolderName(variant)).string()
             << " in D:\\DevelopmentFiles or on a USB drive (or in this app's\n"
                "LocalState): the Xbox Live download (the 42-character file with no\n"
                "extension), a .iso, an .ojpk pack, or the unpacked game (default.xex\n"
                "and data). Then start the app again.\n";
      return {};
    }
    status << "game: " << root.string() << "\n";
    if (variant.empty()) return root;

    // The mod.
    const fs::path marker = root / marker_name;
    const std::vector<fs::path> pkgs = NbaFindPs3Pkgs(work);
    const fs::path folder = fs::is_directory(work / "mod", ec) ? work / "mod" : fs::path();
    if (pkgs.empty() && folder.empty()) {
      if (fs::exists(root / base_name, ec) && !fs::exists(marker, ec)) {
        status << "No " << variant << " mod yet. Put the mod's PS3 .pkg file(s) in "
               << work.string() << ", then start the app again. The game is ready and stays.\n";
        return {};
      }
      if (!fs::exists(marker, ec)) status << "mod: none given; playing the files as they are\n";
      return root;
    }
    const std::string sig = ModSignature(pkgs, !folder.empty());
    if (ReadText(marker) == sig) {
      status << "mod: already applied\n";
      return root;
    }
    fs::path opened = folder;
    const fs::path scratch = work / "mod_ps3";
    if (opened.empty()) {
      fs::remove_all(scratch, ec);
      for (const fs::path& p : pkgs) {
        status << "opening " << p.string() << "\n";
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
    status << "The mod is applied, and is applied again only when the .pkg file(s)\n"
              "change.\n";
    return root;
  }();
  return decided_root;
}

void NbaUwpNoGame() {
  using namespace winrt::Windows::UI;
  const std::wstring local = NbaUwpLocalState().wstring();
  const std::string variant = NbaUwpVariant();
  const std::wstring wvariant(variant.begin(), variant.end());
  const std::wstring name = FolderName(variant);
  const std::wstring where =
      L"- a folder named \"" + name + L"\" on a USB drive\n"
      L"- D:\\DevelopmentFiles\\" + name + L" (Device Portal, File Explorer, DevelopmentFiles)\n"
      L"- this app's LocalState folder:\n  " + local + L"\n\n";
  const std::wstring text =
      variant.empty()
          ? L"No game found.\n\nPut NBA JAM: On Fire Edition in one of these:\n\n" + where +
                L"as any of: your Xbox Live download (the file with a 42-character name "
                L"and no extension, about 850 MB), a .iso, an .ojpk pack, or the "
                L"unpacked game (default.xex and data).\n\n"
                L"Then start the app again. The first start unpacks it, which takes a "
                L"minute or two; status.txt in LocalState says how it went."
          : L"No game found.\n\nThis app plays the " + wvariant +
                L" mod. Put in one of these:\n\n" + where +
                L"the mod's PS3 .pkg file(s), and the game itself: your Xbox Live "
                L"download (the 42-character file with no extension) or a .iso. The "
                L"game can instead go in an \"NBA JAM\" folder on the USB drive, shared "
                L"with the other two apps. An .ojpk pack of the mod does it all in one "
                L"file.\n\n"
                L"Then start the app again. The first start unpacks the game and "
                L"applies the mod, which takes a few minutes; status.txt in LocalState "
                L"says how it went.";
  // What status.txt says comes first: it names the step that stopped, which
  // is what decides what to do, and the TV is easier to read than Device
  // Portal.
  std::wstring said;
  {
    const std::string st = ReadText(NbaUwpLocalState() / "status.txt");
    said.assign(st.begin(), st.end());
    if (said.size() > 1500) said = L"...\n" + said.substr(said.size() - 1500);
  }
  const std::wstring shown =
      said.empty() ? text : L"What happened:\n\n" + said + L"\n\n" + text;
  bool done = false;
  try {
    Popups::MessageDialog dialog(winrt::hstring(shown), L"NBA JAM: On Fire Edition");
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
