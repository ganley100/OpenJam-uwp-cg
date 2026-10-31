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
// broadFileSystemAccess (AppxManifest.xml), through the FromApp file calls:
// the plain ones, which std::filesystem and the runtime use, are refused
// there. So what is found outside is copied into LocalState first, and
// everything after that works on LocalState as before.

struct XEntry {
  std::wstring name;
  bool dir;
  uint64_t size;
  uint64_t mtime;
};

bool XStat(const fs::path& p, XEntry* out) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExFromAppW(p.c_str(), GetFileExInfoStandard, &d)) return false;
  out->name = p.filename().wstring();
  out->dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  out->size = uint64_t(d.nFileSizeHigh) << 32 | d.nFileSizeLow;
  out->mtime = uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32 | d.ftLastWriteTime.dwLowDateTime;
  return true;
}

bool XIsDir(const fs::path& p) {
  XEntry e;
  return XStat(p, &e) && e.dir;
}

std::vector<XEntry> XList(const fs::path& dir) {
  std::vector<XEntry> out;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileExFromAppW((dir / L"*").c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, 0);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    const std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    out.push_back({name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                   uint64_t(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow,
                   uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32 |
                       fd.ftLastWriteTime.dwLowDateTime});
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}

// Why the last XCopy failed, for status.txt.
std::string g_copy_error;

std::string ErrorText(const char* what, DWORD code) {
  const char* name = code == ERROR_ACCESS_DENIED        ? " (access denied)"
                     : code == ERROR_SHARING_VIOLATION   ? " (the file is in use)"
                     : code == ERROR_DISK_FULL           ? " (the drive is full)"
                     : code == ERROR_FILE_NOT_FOUND      ? " (not found)"
                     : code == ERROR_PATH_NOT_FOUND      ? " (path not found)"
                     : code == ERROR_HANDLE_DISK_FULL    ? " (the drive is full)"
                                                         : "";
  return std::string(what) + ": Windows error " + std::to_string(code) + name;
}

// Copies one file, keeping its write time so a later sync can tell it is
// the same. The system's own copy first; then by hand, sharing the file with
// whatever else has it open (Device Portal may, just after an upload).
bool XCopy(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  g_copy_error.clear();
  if (CopyFileFromAppW(from.c_str(), to.c_str(), FALSE)) return true;
  g_copy_error = ErrorText("system copy", GetLastError());

  constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  HANDLE in = CreateFile2FromAppW(from.c_str(), GENERIC_READ, kShare, OPEN_EXISTING, nullptr);
  if (in == INVALID_HANDLE_VALUE) {
    g_copy_error += "; " + ErrorText("opening it", GetLastError());
    return false;
  }
  HANDLE out = CreateFile2FromAppW(to.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
  if (out == INVALID_HANDLE_VALUE) {
    g_copy_error += "; " + ErrorText("creating the copy", GetLastError());
    CloseHandle(in);
    return false;
  }
  std::vector<char> buf(8u << 20);
  bool ok = true;
  for (;;) {
    DWORD got = 0, put = 0;
    if (!ReadFile(in, buf.data(), DWORD(buf.size()), &got, nullptr)) {
      g_copy_error += "; " + ErrorText("reading it", GetLastError());
      ok = false;
      break;
    }
    if (got == 0) break;
    if (!WriteFile(out, buf.data(), got, &put, nullptr) || put != got) {
      g_copy_error += "; " + ErrorText("writing the copy", GetLastError());
      ok = false;
      break;
    }
  }
  FILETIME created{}, accessed{}, written{};
  if (ok && GetFileTime(in, &created, &accessed, &written)) {
    SetFileTime(out, nullptr, nullptr, &written);
  }
  CloseHandle(in);
  CloseHandle(out);
  if (ok) {
    g_copy_error.clear();
  } else {
    fs::remove(to, ec);
  }
  return ok;
}

// Lowercase extension of a name.
std::wstring Ext(const std::wstring& name) {
  std::wstring e = fs::path(name).extension().wstring();
  for (wchar_t& c : e) c = wchar_t(towlower(c));
  return e;
}

// What the game is shipped in, as opposed to the game: never synced as
// loose files.
bool IsSource(const XEntry& e) {
  const std::wstring x = Ext(e.name);
  return !e.dir && (x == L".pkg" || x == L".iso" || x == L".ojpk" ||
                    (x.empty() && e.size >= 40ull * 1024 * 1024));
}

// Mirrors a folder of loose files into LocalState, copying what is missing
// or differs. Returns the number of files copied, or -1.
int XSyncTree(const fs::path& from, const fs::path& to, bool top, std::ostream& status) {
  int copied = 0;
  for (const XEntry& e : XList(from)) {
    if (top && (IsSource(e) || e.name == L"saves" || e.name == L"logs" || e.name == L"mod" ||
                e.name == L"status.txt")) {
      continue;
    }
    const fs::path src = from / e.name, dst = to / e.name;
    if (e.dir) {
      const int n = XSyncTree(src, dst, false, status);
      if (n < 0) return -1;
      copied += n;
      continue;
    }
    XEntry have;
    if (XStat(dst, &have) && have.size == e.size && have.mtime == e.mtime) continue;
    if (!XCopy(src, dst)) {
      status << "could not copy " << src.string() << " - " << g_copy_error << "\n";
      return -1;
    }
    if (++copied % 250 == 0) {
      status << "  " << copied << " file(s) copied\n";
      status.flush();
    }
  }
  return copied;
}

// Names, sizes and write times of everything a mirror would copy: when it
// is the same as last time, nothing outside has changed.
void XStamp(const fs::path& from, bool top, std::string& out) {
  for (const XEntry& e : XList(from)) {
    if (top && (IsSource(e) || e.name == L"saves" || e.name == L"logs" || e.name == L"mod" ||
                e.name == L"status.txt")) {
      continue;
    }
    if (e.dir) {
      XStamp(from / e.name, false, out);
      continue;
    }
    out += (from / e.name).string() + " " + std::to_string(e.size) + " " +
           std::to_string(e.mtime) + "\n";
  }
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

// The loose game in an outside folder: the folder itself, or its game\.
fs::path ExternalLooseGame(const fs::path& folder) {
  XEntry e;
  if (folder.empty()) return {};
  if (XStat(folder / L"default.xex", &e) && !e.dir) return folder;
  if (XStat(folder / L"game" / L"default.xex", &e) && !e.dir) return folder / L"game";
  return {};
}

// The first file in an outside folder that the test says is the game's
// source: brought into LocalState\import, where the readers can open it.
fs::path Bring(const fs::path& folder, const fs::path& local, bool (*want)(const XEntry&),
               std::ostream& status) {
  if (folder.empty()) return {};
  for (const XEntry& e : XList(folder)) {
    if (e.dir || !want(e)) continue;
    const fs::path to = local / L"import" / e.name;
    XEntry have;
    if (!(XStat(to, &have) && have.size == e.size && have.mtime == e.mtime)) {
      status << "copying " << (folder / e.name).string() << " into LocalState\n";
      status.flush();
      if (!XCopy(folder / e.name, to)) {
        status << "could not copy it - " << g_copy_error << "\n";
        continue;
      }
    }
    return to;
  }
  return {};
}

bool WantOjpk(const XEntry& e) { return Ext(e.name) == L".ojpk"; }
bool WantIso(const XEntry& e) { return Ext(e.name) == L".iso"; }
bool WantContainer(const XEntry& e) {
  return Ext(e.name).empty() && e.size >= 40ull * 1024 * 1024;
}

// What a mod app was last given, so it is applied once: the names and sizes
// of the packages or the folder it came from.
std::string ModSignature(const std::vector<XEntry>& pkgs, bool folder) {
  std::string sig;
  for (const auto& p : pkgs) {
    sig += fs::path(p.name).string() + " " + std::to_string(p.size) + "\n";
  }
  if (folder) sig += "mod/\n";
  return sig;
}

std::string ReadText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// Where the game comes from. Three places are looked in, in this order:
//
//   D:\DevelopmentFiles\NBA JAM...   the console's own folder for loose files
//   <USB drive>:\NBA JAM...          the folder name is the app's own:
//                                    NBA JAM, NBA JAM 2020s, NBA JAM Legends
//   LocalState                       the app's storage, as before
//
// In any of them the game can be the loose files (default.xex and data, in
// the folder or in its game\), an .ojpk pack, the Xbox Live download or a
// .iso. Loose files outside are mirrored into LocalState\game on every
// start, so changing one there and starting again is enough. A mod app can
// take the plain game from the NBA JAM folder, so one copy on a drive does
// for all three apps, and then puts its mod on it once from the mod's PS3
// .pkg file(s) (or an opened mod in mod\), in any of the places.
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
    const fs::path game = local / "game";
    const fs::path marker = game / "openjam_mod.txt";
    const fs::path base_marker = game / "openjam_unpacked.txt";
    std::error_code ec;

    const fs::path own = ExternalFolder(FolderName(variant), variant);
    const fs::path shared = variant.empty() ? fs::path() : ExternalFolder(FolderName(""), "");
    status << "looked for " << fs::path(FolderName(variant)).string()
           << " in D:\\DevelopmentFiles and on USB drives: "
           << (own.empty() ? "not found" : own.string()) << "\n";
    if (!variant.empty()) {
      status << "and for the plain game in NBA JAM: "
             << (shared.empty() ? "not found" : shared.string()) << "\n";
    }
    if (own.empty()) {
      status << "folders in D:\\DevelopmentFiles:\n" << DevelopmentFilesListing();
    }

    // Loose files outside, mirrored in. A change means the mod has to go on
    // again, since the mirror just put the plain files back.
    fs::path loose = ExternalLooseGame(own);
    bool loose_is_base = false;
    if (loose.empty() && !variant.empty()) {
      loose = ExternalLooseGame(shared);
      loose_is_base = !loose.empty();
    }
    const fs::path loose_stamp = local / "loose_stamp.txt";
    if (!loose.empty()) {
      std::string stamp;
      XStamp(loose, true, stamp);
      if (stamp == ReadText(loose_stamp) && fs::exists(game / "default.xex", ec)) {
        status << "loose game in " << loose.string() << ": unchanged since last time\n";
      } else {
        status << "mirroring the loose game in " << loose.string() << " into " << game.string()
               << "\n";
        status.flush();
        const int n = XSyncTree(loose, game, true, status);
        if (n < 0) return {};
        status << n << " file(s) copied\n";
        // The mirror put plain files back, so a mod goes on again.
        if (n > 0) fs::remove(marker, ec);
        if (loose_is_base) std::ofstream(base_marker) << "loose\n";
        std::ofstream(loose_stamp, std::ios::binary) << stamp;
      }
    }

    fs::path root;
    std::vector<fs::path> brought;  // copies in LocalState\import, removed after
    if (fs::exists(local / "default.xex", ec)) {
      root = local;
      status << "game: loose files in " << root.string() << "\n";
    } else if (fs::exists(game / "default.xex", ec)) {
      root = game;
      status << "game: " << root.string() << "\n";
    } else {
      // A source, here or outside.
      fs::path pack = NbaFindPack(local);
      if (pack.empty() && !(pack = Bring(own, local, WantOjpk, status)).empty()) brought.push_back(pack);
      fs::path container, iso;
      if (pack.empty()) {
        container = NbaFindLiveContainer(local);
        for (const fs::path& from : {own, shared}) {
          if (!container.empty()) break;
          const fs::path c = Bring(from, local, WantContainer, status);
          if (!c.empty() && NbaIsLiveContainer(c)) {
            container = c;
            brought.push_back(c);
          } else if (!c.empty()) {
            fs::remove(c, ec);
          }
        }
      }
      if (pack.empty() && container.empty()) {
        iso = NbaFindDiscImage(local);
        for (const fs::path& from : {own, shared}) {
          if (!iso.empty()) break;
          if (!(iso = Bring(from, local, WantIso, status)).empty()) brought.push_back(iso);
        }
      }
      int n = 0;
      std::string what;
      if (!pack.empty()) {
        // A pack is a whole game root, a mod's already applied.
        status << "unpacking " << pack.filename().string() << " into " << game.string() << "\n";
        status.flush();
        n = NbaUnpackPack(pack, game, status);
        what = ".ojpk";
        if (n > 0) std::ofstream(marker) << "pack " << pack.filename().string() << "\n";
      } else if (!container.empty()) {
        status << "unpacking " << container.filename().string() << " into " << game.string()
               << "\n";
        status.flush();
        n = NbaUnpackLiveContainer(container, game);
        what = "container";
      } else if (!iso.empty()) {
        status << "unpacking " << iso.filename().string() << " into " << game.string() << "\n";
        status.flush();
        n = NbaUnpackDiscImage(iso, game);
        what = ".iso";
      } else {
        status << "No game yet. Put the game in one of these, then start the app again:\n"
                  "  a folder named " << fs::path(FolderName(variant)).string()
               << " on a USB drive\n"
                  "  D:\\DevelopmentFiles\\" << fs::path(FolderName(variant)).string()
               << "\n  this app's LocalState\n"
                  "as the Xbox Live download (the 42-character file with no extension),\n"
                  "a .iso, an .ojpk pack, or the unpacked game (default.xex and data).\n";
        return {};
      }
      for (const fs::path& b : brought) fs::remove(b, ec);
      if (n <= 0 || !fs::exists(game / "default.xex", ec)) {
        status << "unpacking failed (" << n << " file(s))\n";
        return {};
      }
      status << n << " file(s) unpacked.\n";
      if (what != ".ojpk") std::ofstream(base_marker) << what << "\n";
      root = game;
    }

    if (variant.empty()) return root;

    // The mod: PS3 packages, or an opened mod, here or in this app's own
    // folder outside (never the shared one, which is the plain game's).
    std::vector<XEntry> pkgs;
    std::vector<fs::path> pkg_paths;
    for (const fs::path& from : {local, own}) {
      if (from.empty()) continue;
      for (const XEntry& e : XList(from)) {
        if (!e.dir && Ext(e.name) == L".pkg") {
          pkgs.push_back(e);
          pkg_paths.push_back(from / e.name);
        }
      }
    }
    fs::path folder;
    if (fs::is_directory(local / "mod", ec)) {
      folder = local / "mod";
    } else if (!own.empty() && XIsDir(own / L"mod")) {
      status << "copying the mod folder in " << (own / L"mod").string() << "\n";
      if (XSyncTree(own / L"mod", local / "mod", false, status) < 0) return {};
      folder = local / "mod";
    }
    if (pkgs.empty() && folder.empty()) {
      // The plain game, put here by this app, with nothing on it yet.
      if (fs::exists(base_marker, ec) && !fs::exists(marker, ec)) {
        status << "No " << variant << " mod yet. Put the mod's PS3 .pkg file(s) beside\n"
                  "the game (in the NBA JAM " << variant << " folder, or LocalState),\n"
                  "then start the app again. The game itself is ready and stays.\n";
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
    const fs::path scratch = local / "mod_ps3";
    if (opened.empty()) {
      fs::remove_all(scratch, ec);
      // Largest first: a mod ships as a large part and sometimes a small one.
      std::vector<size_t> order(pkgs.size());
      for (size_t i = 0; i < order.size(); ++i) order[i] = i;
      std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return pkgs[a].size > pkgs[b].size; });
      for (size_t i : order) {
        fs::path p = pkg_paths[i];
        bool copy = false;
        if (p.parent_path() != local) {
          status << "copying " << p.string() << " into LocalState\n";
          status.flush();
          const fs::path to = local / L"import" / pkgs[i].name;
          if (!XCopy(p, to)) {
            status << "could not copy it - " << g_copy_error << "\n";
            return {};
          }
          p = to;
          copy = true;
        }
        status << "opening " << p.filename().string() << "\n";
        status.flush();
        const int got = NbaExtractPs3Pkg(p, scratch, status);
        if (copy) fs::remove(p, ec);
        if (got < 0) {
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
    status << "The mod is applied, and will not be applied again until the\n"
              ".pkg file(s) change. They can be deleted from LocalState now.\n";
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
