// OpenJam Xbox smoke test
//
// A small UWP app that checks, on a real Xbox One in Developer Mode, the
// things the game will need before any of the ReXGlue port is worth doing:
//
//   1. The guest memory arena. ReXGlue maps the Xbox 360's 4 GB address space
//      as nine views of one 4.5 GB section at a fixed power-of-two base
//      (src/system/xmemory.cpp, Memory::Initialize and MapViews), and on UWP
//      it does that with CreateFileMappingFromApp, VirtualAlloc2FromApp and
//      MapViewOfFile3FromApp (src/core/memory_win.cpp). This repeats that
//      exactly, then checks two views of the same pages really alias.
//   2. Committing and touching 512 MB, the 360's whole physical memory.
//   3. A vectored exception handler recovering from an access violation. The
//      runtime's MMIO handler (GPU registers) is built on exactly that.
//   4. Direct3D 12, and what the GPU supports.
//   5. The controller, through XInput as the runtime reads it.
//   6. Writing to LocalState and reading files out of the package.
//   7. Which ordinary Windows DLLs exist on the console. If the ones ReXGlue
//      and its libraries already use are there, most of the SDK can stay
//      ordinary Windows code and only the window has to change.
//
// Results are drawn on the TV and written to LocalState\report.txt, which
// Device Portal's File Explorer can open.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <xinput.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "font5x7.h"

namespace fs = std::filesystem;
using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;

namespace {

// --- results ----------------------------------------------------------------

enum class State { kPending, kPass, kFail, kInfo };

struct Check {
  std::string name;
  State state = State::kPending;
  std::vector<std::string> detail;
};

std::mutex g_lock;
std::vector<Check> g_checks;

size_t AddCheck(const std::string& name) {
  std::lock_guard<std::mutex> guard(g_lock);
  g_checks.push_back({name, State::kPending, {}});
  return g_checks.size() - 1;
}

void Note(size_t i, const char* format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  std::lock_guard<std::mutex> guard(g_lock);
  g_checks[i].detail.emplace_back(buf);
}

void Finish(size_t i, State state) {
  std::lock_guard<std::mutex> guard(g_lock);
  g_checks[i].state = state;
}

const char* StateTag(State s) {
  switch (s) {
    case State::kPass: return "[ OK ]";
    case State::kFail: return "[FAIL]";
    case State::kInfo: return "[INFO]";
    default:           return "[WAIT]";
  }
}

// --- 1. the guest memory arena ----------------------------------------------

// The same table as ReXGlue v0.10.0's map_info in src/system/xmemory.cpp:
// guest range, and where in the section it lands.
struct View {
  uint64_t start, end, target;
};
constexpr View kViews[] = {
    {0x00000000, 0x3FFFFFFF, 0x0000000000000000ull},
    {0x40000000, 0x7EFFFFFF, 0x0000000040000000ull},
    {0x7F000000, 0x7FFFFFFF, 0x0000000100000000ull},
    {0x80000000, 0x8FFFFFFF, 0x0000000080000000ull},
    {0x90000000, 0x9FFFFFFF, 0x0000000080000000ull},
    {0xA0000000, 0xBFFFFFFF, 0x0000000100000000ull},
    {0xC0000000, 0xDFFFFFFF, 0x0000000100000000ull},
    {0xE0000000, 0xFFFFFFFF, 0x0000000100001000ull},
    {0x100000000, 0x11FFFFFFF, 0x0000000100000000ull},
};
constexpr size_t kViewCount = sizeof(kViews) / sizeof(kViews[0]);

HANDLE g_section = nullptr;
uint8_t* g_base = nullptr;
void* g_view_at[kViewCount] = {};

void UnmapAll() {
  for (auto& v : g_view_at) {
    if (v) UnmapViewOfFile(v);
    v = nullptr;
  }
}

// One view, as rex::memory::MapFileView does it on UWP: reserve a placeholder
// at the exact address, then replace it with the mapping.
void* MapOne(void* at, size_t length, uint64_t offset) {
  HANDLE process = GetCurrentProcess();
  void* placeholder = VirtualAlloc2FromApp(
      process, at, length, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
      nullptr, 0);
  if (!placeholder) return nullptr;
  void* mapping = MapViewOfFile3FromApp(g_section, process, placeholder, offset,
                                        length, MEM_REPLACE_PLACEHOLDER,
                                        PAGE_READWRITE, nullptr, 0);
  if (!mapping) {
    VirtualFree(placeholder, 0, MEM_RELEASE);
    return nullptr;
  }
  return mapping;
}

bool TestArena(size_t c) {
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const uint64_t gran = si.dwAllocationGranularity;
  const uint64_t size = (0x120000000ull + gran + gran - 1) / gran * gran;
  Note(c, "page %u B, granularity %llu B, section %llu MB",
       unsigned(si.dwPageSize), (unsigned long long)gran,
       (unsigned long long)(size >> 20));

  const std::wstring name =
      L"Local\\xenia_memory_" + std::to_wstring(GetTickCount64());
  g_section = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr,
                                       PAGE_READWRITE | SEC_RESERVE, size,
                                       name.c_str());
  if (!g_section) {
    Note(c, "CreateFileMappingFromApp failed, error %lu", GetLastError());
    return false;
  }

  const uint64_t mask = ~(gran - 1);
  DWORD last_error = 0;
  int last_view = -1;
  for (int n = 32; n < 64 && !g_base; ++n) {
    uint8_t* base = reinterpret_cast<uint8_t*>(1ull << n);
    bool ok = true;
    for (size_t v = 0; v < kViewCount; ++v) {
      g_view_at[v] = MapOne(base + kViews[v].start,
                            size_t(kViews[v].end - kViews[v].start + 1),
                            kViews[v].target & mask);
      if (!g_view_at[v]) {
        last_error = GetLastError();
        last_view = int(v);
        ok = false;
        break;
      }
    }
    if (ok) {
      g_base = base;
    } else {
      UnmapAll();
    }
  }
  if (!g_base) {
    Note(c, "no base worked; last failure view %d, error %lu", last_view,
         last_error);
    return false;
  }
  Note(c, "all %zu views mapped at base 0x%llX", kViewCount,
       (unsigned long long)uintptr_t(g_base));

  // Guest 0xA0001000 and guest-physical 0x00001000 are the same page of the
  // section; a value written through one must be read through the other.
  uint8_t* virt = g_base + 0xA0001000ull;
  uint8_t* phys = g_base + 0x100000000ull + 0x1000ull;
  if (!VirtualAllocFromApp(virt, 0x1000, MEM_COMMIT, PAGE_READWRITE)) {
    Note(c, "commit inside a view failed, error %lu", GetLastError());
    return false;
  }
  *reinterpret_cast<volatile uint32_t*>(virt) = 0x4A414D21u;
  const uint32_t back = *reinterpret_cast<volatile uint32_t*>(phys);
  Note(c, "alias check: wrote 4A414D21, read %08X", unsigned(back));
  return back == 0x4A414D21u;
}

// --- 2. memory the game can actually use ------------------------------------

bool TestCommit(size_t c) {
  try {
    const uint64_t limit =
        winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
    Note(c, "app memory limit %llu MB (Game mode gives far more than App)",
         (unsigned long long)(limit >> 20));
  } catch (...) {
    Note(c, "app memory limit unavailable");
  }
  if (!g_base) {
    Note(c, "skipped: no arena");
    return false;
  }
  constexpr size_t kSize = 512ull << 20;
  uint8_t* at = g_base + 0x100000000ull;  // the whole guest-physical view
  const auto t0 = std::chrono::steady_clock::now();
  if (!VirtualAllocFromApp(at, kSize, MEM_COMMIT, PAGE_READWRITE)) {
    Note(c, "committing 512 MB failed, error %lu", GetLastError());
    return false;
  }
  // Touch every page so it is really backed, not just promised.
  for (size_t off = 0; off < kSize; off += 4096) at[off] = uint8_t(off >> 12);
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  Note(c, "512 MB committed and touched in %.0f ms", ms);
  VirtualFree(at, kSize, MEM_DECOMMIT);
  return true;
}

// --- 3. recovering from an access violation ---------------------------------

// Not declared for apps: the SDK keeps these to the desktop partition. The
// functions themselves live in kernelbase on every Windows 10, so they are
// declared here to find out whether the console lets an app call them.
// ReXGlue's MMIO handler (GPU register emulation) cannot work without them.
#if WINAPI_FAMILY == WINAPI_FAMILY_APP
using VehHandler = LONG(NTAPI*)(EXCEPTION_POINTERS*);
extern "C" __declspec(dllimport) PVOID WINAPI
AddVectoredExceptionHandler(ULONG first, VehHandler handler);
extern "C" __declspec(dllimport) ULONG WINAPI
RemoveVectoredExceptionHandler(PVOID handle);
#endif

uint8_t* g_trap_page = nullptr;
std::atomic<int> g_trap_hits{0};

LONG CALLBACK TrapHandler(EXCEPTION_POINTERS* info) {
  const auto* rec = info->ExceptionRecord;
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      rec->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  auto* addr = reinterpret_cast<uint8_t*>(rec->ExceptionInformation[1]);
  if (!g_trap_page || addr < g_trap_page || addr >= g_trap_page + 4096) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  ULONG old = 0;
  VirtualProtectFromApp(g_trap_page, 4096, PAGE_READWRITE, &old);
  g_trap_hits.fetch_add(1);
  return EXCEPTION_CONTINUE_EXECUTION;
}

bool TestTrap(size_t c) {
  void* handler = AddVectoredExceptionHandler(1, TrapHandler);
  if (!handler) {
    Note(c, "AddVectoredExceptionHandler failed, error %lu", GetLastError());
    return false;
  }
  g_trap_page = static_cast<uint8_t*>(VirtualAllocFromApp(
      nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
  if (!g_trap_page) {
    Note(c, "could not allocate a guard page, error %lu", GetLastError());
    RemoveVectoredExceptionHandler(handler);
    return false;
  }
  *reinterpret_cast<volatile uint32_t*>(g_trap_page + 16) = 0x1234;
  const uint32_t v = *reinterpret_cast<volatile uint32_t*>(g_trap_page + 16);
  RemoveVectoredExceptionHandler(handler);
  Note(c, "fault caught %d time(s), write survived: %s", g_trap_hits.load(),
       v == 0x1234 ? "yes" : "no");
  return g_trap_hits.load() == 1 && v == 0x1234;
}

// --- 7. which Windows DLLs exist -------------------------------------------

// Desktop-only, like the exception handler calls above; linked from
// kernel32.lib to find out whether the console has them.
#if WINAPI_FAMILY == WINAPI_FAMILY_APP
extern "C" __declspec(dllimport) HMODULE WINAPI LoadLibraryW(LPCWSTR name);
extern "C" __declspec(dllimport) BOOL WINAPI FreeLibrary(HMODULE module);
#endif

bool TestDlls(size_t c) {
  // What ReXGlue v0.10.0, SDL3, FFmpeg and the game import between them,
  // plus the two that would replace SDL for sound and the pad.
  static const wchar_t* const kDlls[] = {
      L"kernel32.dll", L"user32.dll",   L"gdi32.dll",    L"shell32.dll",
      L"advapi32.dll", L"ole32.dll",    L"oleaut32.dll", L"dbghelp.dll",
      L"winmm.dll",    L"ws2_32.dll",   L"bcrypt.dll",   L"version.dll",
      L"setupapi.dll", L"imm32.dll",    L"hid.dll",      L"d3d12.dll",
      L"dxgi.dll",     L"D3DCompiler_47.dll", L"dxcompiler.dll",
      L"dxilconv.dll", L"xinput1_4.dll", L"xaudio2_9.dll"};
  std::string have, missing;
  for (const wchar_t* name : kDlls) {
    std::string narrow;
    for (const wchar_t* p = name; *p; ++p) narrow += char(*p);
    HMODULE m = LoadLibraryW(name);
    if (m) {
      FreeLibrary(m);
      have += (have.empty() ? "" : " ") + narrow;
    } else {
      missing += (missing.empty() ? "" : " ") + narrow + "(" +
                 std::to_string(GetLastError()) + ")";
    }
  }
  // Long lists, so wrapped to the screen.
  auto wrap = [&](const char* label, const std::string& list) {
    std::string line = label;
    for (size_t at = 0; at < list.size();) {
      size_t sp = list.find(' ', at);
      const std::string word =
          list.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
      if (line.size() + word.size() + 1 > 90) {
        Note(c, "%s", line.c_str());
        line = "  ";
      }
      line += " " + word;
      at = sp == std::string::npos ? list.size() : sp + 1;
    }
    Note(c, "%s", line.c_str());
  };
  wrap("present:", have.empty() ? "none" : have);
  wrap("missing:", missing.empty() ? "none" : missing);
  return !have.empty();
}

// --- 6. files ---------------------------------------------------------------

fs::path g_local;

fs::path PackageDir() {
  wchar_t buf[MAX_PATH]{};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return fs::path(buf).parent_path();
}

bool TestFiles(size_t c) {
  bool ok = true;
  try {
    g_local = fs::path(std::wstring(
        winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
    const fs::path probe = g_local / "probe.txt";
    { std::ofstream(probe) << "openjam"; }
    std::string got;
    { std::ifstream(probe) >> got; }
    Note(c, "LocalState write/read: %s", got == "openjam" ? "ok" : "FAILED");
    ok &= got == "openjam";
  } catch (...) {
    Note(c, "LocalState unavailable");
    ok = false;
  }
  const fs::path manifest = PackageDir() / "AppxManifest.xml";
  std::error_code ec;
  const auto bytes = fs::file_size(manifest, ec);
  Note(c, "package folder readable: %s (%llu bytes of manifest)",
       ec ? "NO" : "yes", (unsigned long long)(ec ? 0 : bytes));
  ok &= !ec;
  return ok;
}

// --- the screen -------------------------------------------------------------

constexpr UINT kWidth = 1920, kHeight = 1080, kBuffers = 2;
constexpr int kScale = 3, kCellW = 6 * kScale, kCellH = 11 * kScale;

std::vector<uint32_t> g_pixels(size_t(kWidth) * kHeight);

constexpr uint32_t Rgb(uint8_t r, uint8_t g, uint8_t b) {
  return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}

void DrawString(int col, int row, const std::string& text, uint32_t color) {
  int x0 = 40 + col * kCellW;
  const int y0 = 40 + row * kCellH;
  for (char ch : text) {
    int c = std::toupper(static_cast<unsigned char>(ch));
    if (c < 0x20 || c > 0x5F) c = '?';
    for (int x = 0; x < 5; ++x) {
      for (int y = 0; y < 7; ++y) {
        if (!(kFont5x7[c - 0x20][x] >> y & 1)) continue;
        for (int a = 0; a < kScale; ++a) {
          for (int b = 0; b < kScale; ++b) {
            const int px = x0 + x * kScale + b, py = y0 + y * kScale + a;
            if (px >= 0 && px < int(kWidth) && py >= 0 && py < int(kHeight)) {
              g_pixels[size_t(py) * kWidth + px] = color;
            }
          }
        }
      }
    }
    x0 += kCellW;
  }
}

void DrawReport() {
  std::fill(g_pixels.begin(), g_pixels.end(), Rgb(8, 8, 16));
  DrawString(0, 0, "OPENJAM XBOX SMOKE TEST", Rgb(255, 140, 0));
  DrawString(0, 1, "TAKE A PHOTO OF THIS SCREEN AND SEND IT BACK.",
           Rgb(160, 160, 160));
  int row = 3;
  std::lock_guard<std::mutex> guard(g_lock);
  for (const Check& ch : g_checks) {
    const uint32_t color = ch.state == State::kPass   ? Rgb(60, 220, 60)
                           : ch.state == State::kFail ? Rgb(240, 60, 60)
                           : ch.state == State::kInfo ? Rgb(90, 160, 255)
                                                      : Rgb(240, 200, 40);
    DrawString(0, row, StateTag(ch.state), color);
    DrawString(7, row++, ch.name, Rgb(240, 240, 240));
    for (const std::string& d : ch.detail) {
      DrawString(9, row++, d, Rgb(170, 170, 170));
    }
  }
}

void WriteReport() {
  if (g_local.empty()) return;
  std::ofstream out(g_local / "report.txt");
  std::lock_guard<std::mutex> guard(g_lock);
  for (const Check& ch : g_checks) {
    out << StateTag(ch.state) << ' ' << ch.name << '\n';
    for (const std::string& d : ch.detail) out << "         " << d << '\n';
  }
}

// --- 8. the game's own DLLs -----------------------------------------------

// Records the first fault while a DLL is loading: what it was, in which
// module, at what offset - enough, with the build's linker map, to name the
// function. It only observes; the fault goes on to do what it would have.
#if WINAPI_FAMILY == WINAPI_FAMILY_APP
// Desktop-only declarations again, from kernel32.lib.
extern "C" __declspec(dllimport) BOOL WINAPI GetModuleHandleExA(DWORD flags, LPCSTR name,
                                                                HMODULE* module);
extern "C" __declspec(dllimport) DWORD WINAPI GetModuleFileNameA(HMODULE module, LPSTR name,
                                                                 DWORD size);
#endif
#ifndef GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#endif
#ifndef GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
#endif

size_t g_load_check = SIZE_MAX;
std::atomic<int> g_load_faults{0};

// A C++ exception carries its type in the ThrowInfo the compiler emits; on
// x64 every pointer in it is an offset from the throwing module's base.
struct ThrowInfoX64 {
  uint32_t attributes;
  int32_t unwind, forward_compat, catchable_types;
};
struct CatchableTypeX64 {
  uint32_t properties;
  int32_t type;
  int32_t mdisp, pdisp, vdisp;
  int32_t size, copy;
};
struct TypeDescriptorX64 {
  const void* vftable;
  void* spare;
  char name[1];
};

// The type of a thrown C++ exception, and what() if it is a std::exception.
// Plain C, so __try can guard reading memory that may not be what it seems.
void DescribeCppException(const EXCEPTION_RECORD* rec, char* out, size_t size) {
  std::snprintf(out, size, " (C++ exception)");
  if (rec->NumberParameters < 4) return;
  __try {
    const auto base = static_cast<uintptr_t>(rec->ExceptionInformation[3]);
    const auto* info = reinterpret_cast<const ThrowInfoX64*>(rec->ExceptionInformation[2]);
    auto* object = reinterpret_cast<uint8_t*>(rec->ExceptionInformation[1]);
    if (!info || !base) return;
    const auto* types = reinterpret_cast<const int32_t*>(base + uint32_t(info->catchable_types));
    const int32_t count = types[0];
    const char* first = nullptr;
    const char* message = nullptr;
    for (int32_t i = 0; i < count && i < 16; ++i) {
      const auto* ct = reinterpret_cast<const CatchableTypeX64*>(base + uint32_t(types[1 + i]));
      const auto* td = reinterpret_cast<const TypeDescriptorX64*>(base + uint32_t(ct->type));
      if (!first) first = td->name;
      if (!message && std::strcmp(td->name, ".?AVexception@std@@") == 0 && object) {
        message = reinterpret_cast<const std::exception*>(object + ct->mdisp)->what();
      }
    }
    std::snprintf(out, size, " (C++ %s: %s)", first ? first : "?",
                  message ? message : "no message");
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

LONG CALLBACK RecordLoadFault(EXCEPTION_POINTERS* info) {
  const auto* rec = info->ExceptionRecord;
  // Faults and C++ exceptions, and only the first few (a crash can
  // cascade). A C++ exception nothing catches ends in a fast fail, which no
  // handler sees, so the throw is the only trace of it.
  const bool cpp = rec->ExceptionCode == 0xE06D7363;
  if ((!cpp && rec->ExceptionCode < 0xC0000000u) || g_load_faults.fetch_add(1) >= 6) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  char module_name[MAX_PATH] = "?";
  HMODULE module = nullptr;
  uintptr_t offset = 0;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(rec->ExceptionAddress), &module) &&
      module) {
    GetModuleFileNameA(module, module_name, MAX_PATH);
    offset = reinterpret_cast<uintptr_t>(rec->ExceptionAddress) -
             reinterpret_cast<uintptr_t>(module);
  }
  const char* base = std::strrchr(module_name, '\\');
  base = base ? base + 1 : module_name;
  char what[384] = "";
  if (cpp) {
    DescribeCppException(rec, what, sizeof(what));
  } else if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    std::snprintf(what, sizeof(what), " (%s 0x%llX)",
                  rec->ExceptionInformation[0] == 0 ? "reading" :
                  rec->ExceptionInformation[0] == 1 ? "writing" : "executing",
                  static_cast<unsigned long long>(rec->ExceptionInformation[1]));
  } else if (rec->ExceptionCode == 0xC000001D) {
    std::snprintf(what, sizeof(what), " (an instruction this CPU does not have)");
  } else if (rec->ExceptionCode == 0xC0000409 && rec->NumberParameters >= 1) {
    std::snprintf(what, sizeof(what), " (fast fail %llu)",
                  static_cast<unsigned long long>(rec->ExceptionInformation[0]));
  }
  if (g_load_check != SIZE_MAX) {
    Note(g_load_check, "FAULT %08lX at %s+0x%llX%s",
         static_cast<unsigned long>(rec->ExceptionCode), base,
         static_cast<unsigned long long>(offset), what);
    WriteReport();
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// Shipped in the game's package, this loads every DLL beside it, one at a
// time, in the order they depend on each other. Each is noted in report.txt
// before it is tried, so if loading one kills the app outright - a crash in
// its startup code - report.txt still ends with its name.
bool TestPackageDlls(size_t c) {
  std::vector<std::wstring> names;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(PackageDir(), ec)) {
    if (e.path().extension() == L".dll") names.push_back(e.path().filename().wstring());
  }
  if (names.empty()) {
    Note(c, "no DLLs in this package (run from the game's package for this)");
    return true;
  }
  // The C++ runtime first, then the ReXGlue runtime, then the GPU plugin.
  auto rank = [](const std::wstring& n) {
    std::wstring l;
    for (wchar_t ch : n) l += wchar_t(towlower(ch));
    if (l.rfind(L"vcruntime", 0) == 0 || l.rfind(L"msvcp", 0) == 0 ||
        l.rfind(L"concrt", 0) == 0 || l.rfind(L"vccorlib", 0) == 0) return 0;
    if (l.rfind(L"rexruntime", 0) == 0) return 1;
    return 2;
  };
  std::sort(names.begin(), names.end(), [&](const auto& a, const auto& b) {
    return rank(a) != rank(b) ? rank(a) < rank(b) : a < b;
  });
  g_load_check = c;
  void* recorder = AddVectoredExceptionHandler(1, RecordLoadFault);
  // The ways a DLL's startup can end the process without a fault: an
  // unhandled C++ exception, abort(), and the CRT's invalid-parameter check.
  std::set_terminate([] {
    if (g_load_check != SIZE_MAX) {
      Note(g_load_check, "TERMINATE: an unhandled C++ exception (see the line above)");
      WriteReport();
    }
    std::abort();
  });
  signal(SIGABRT, [](int) {
    if (g_load_check != SIZE_MAX) {
      Note(g_load_check, "ABORT called");
      WriteReport();
    }
  });
  _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    if (g_load_check != SIZE_MAX) {
      Note(g_load_check, "INVALID PARAMETER passed to a C runtime function");
      WriteReport();
    }
  });
  bool all = true;
  for (const std::wstring& name : names) {
    std::string narrow;
    for (wchar_t ch : name) narrow += char(ch);
    Note(c, "loading %s ...", narrow.c_str());
    WriteReport();
    HMODULE m = LoadPackagedLibrary(name.c_str(), 0);
    const DWORD err = m ? 0 : GetLastError();
    {
      std::lock_guard<std::mutex> guard(g_lock);
      g_checks[c].detail.back() =
          m ? narrow + ": loaded"
            : narrow + ": FAILED, error " + std::to_string(err) +
                  (err == 126 ? " (a DLL it needs is missing)"
                   : err == 127 ? " (a function it needs is missing)"
                                : "");
    }
    all &= m != nullptr;
    WriteReport();
  }
  if (recorder) RemoveVectoredExceptionHandler(recorder);
  g_load_check = SIZE_MAX;
  return all;
}

template <typename T>
void Release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

struct Gpu {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0;
  ID3D12Resource* upload = nullptr;
  uint8_t* upload_ptr = nullptr;
  IDXGISwapChain3* swap = nullptr;
  ID3D12Resource* back[kBuffers] = {};
};

const char* FeatureLevelName(D3D_FEATURE_LEVEL fl) {
  switch (fl) {
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    default:                     return "other";
  }
}

bool InitGpu(Gpu& g, const CoreWindow& window, size_t c) {
  HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&g.device));
  if (FAILED(hr)) {
    Note(c, "D3D12CreateDevice failed, hr %08lX", hr);
    return false;
  }
  const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1};
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels{UINT(std::size(wanted)), wanted};
  g.device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels));
  D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
  g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
  D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_0};
  const bool sm6 = SUCCEEDED(
      g.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)));
  Note(c, "feature level %s, binding tier %d, tiled tier %d",
       FeatureLevelName(levels.MaxSupportedFeatureLevel),
       int(opts.ResourceBindingTier), int(opts.TiledResourcesTier));
  Note(c, "ROVs %s, shader model %s", opts.ROVsSupported ? "yes" : "no",
       sm6 ? (sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0 ? "6.x" : "5.1")
           : "5.1");

  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(hr = g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue))) ||
      FAILED(hr = g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&g.allocator))) ||
      FAILED(hr = g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              g.allocator, nullptr,
                                              IID_PPV_ARGS(&g.list))) ||
      FAILED(hr = g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&g.fence)))) {
    Note(c, "command setup failed, hr %08lX", hr);
    return false;
  }
  g.list->Close();
  g.fence_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

  // The picture is drawn on the CPU and copied in, so no shaders are needed.
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = UINT64(kWidth) * kHeight * 4;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(hr = g.device->CreateCommittedResource(
                 &heap, D3D12_HEAP_FLAG_NONE, &rd,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(&g.upload))) ||
      FAILED(hr = g.upload->Map(0, nullptr,
                                reinterpret_cast<void**>(&g.upload_ptr)))) {
    Note(c, "upload buffer failed, hr %08lX", hr);
    return false;
  }

  IDXGIFactory2* factory = nullptr;
  if (FAILED(hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    Note(c, "CreateDXGIFactory2 failed, hr %08lX", hr);
    return false;
  }
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = kWidth;
  sd.Height = kHeight;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kBuffers;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  IDXGISwapChain1* swap1 = nullptr;
  hr = factory->CreateSwapChainForCoreWindow(
      g.queue, static_cast<::IUnknown*>(winrt::get_abi(window)), &sd, nullptr,
      &swap1);
  Release(factory);
  if (FAILED(hr)) {
    Note(c, "CreateSwapChainForCoreWindow failed, hr %08lX", hr);
    return false;
  }
  hr = swap1->QueryInterface(IID_PPV_ARGS(&g.swap));
  Release(swap1);
  if (FAILED(hr)) {
    Note(c, "IDXGISwapChain3 unavailable, hr %08lX", hr);
    return false;
  }
  for (UINT i = 0; i < kBuffers; ++i) {
    if (FAILED(hr = g.swap->GetBuffer(i, IID_PPV_ARGS(&g.back[i])))) {
      Note(c, "GetBuffer failed, hr %08lX", hr);
      return false;
    }
  }
  Note(c, "swap chain %ux%u on the CoreWindow", kWidth, kHeight);
  return true;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r,
             D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  list->ResourceBarrier(1, &b);
}

void Present(Gpu& g) {
  std::memcpy(g.upload_ptr, g_pixels.data(), g_pixels.size() * 4);
  ID3D12Resource* back = g.back[g.swap->GetCurrentBackBufferIndex()];
  g.allocator->Reset();
  g.list->Reset(g.allocator, nullptr);
  Barrier(g.list, back, D3D12_RESOURCE_STATE_PRESENT,
          D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = back;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = g.upload;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, 1,
                                   kWidth * 4};
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Barrier(g.list, back, D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_PRESENT);
  g.list->Close();
  ID3D12CommandList* lists[] = {g.list};
  g.queue->ExecuteCommandLists(1, lists);
  g.swap->Present(1, 0);
  // Wait for the frame, so the next one can overwrite the upload buffer.
  g.queue->Signal(g.fence, ++g.fence_value);
  if (g.fence->GetCompletedValue() < g.fence_value) {
    g.fence->SetEventOnCompletion(g.fence_value, g.fence_event);
    WaitForSingleObjectEx(g.fence_event, 1000, FALSE);
  }
}

// --- the app ----------------------------------------------------------------

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
  IFrameworkView CreateView() { return *this; }
  void Initialize(const CoreApplicationView&) {}
  void Load(const hstring&) {}
  void Uninitialize() {}

  void SetWindow(const CoreWindow& window) {
    window_ = window;
    window.Closed([this](auto&&, auto&&) { closed_ = true; });
  }

  void Run() {
    window_.Activate();

    // Everything that can be checked before there is a picture, in case the
    // picture is what fails: report.txt still says what happened.
    const size_t files = AddCheck("Files: LocalState and package folder");
    const size_t arena = AddCheck("Guest memory arena (ReXGlue layout)");
    const size_t commit = AddCheck("512 MB of guest memory");
    const size_t trap = AddCheck("Recovering from an access violation");
    const size_t gpu_check = AddCheck("Direct3D 12 on the CoreWindow");
    const size_t pad = AddCheck("Controller (press any button)");
    const size_t dlls = AddCheck("Windows DLLs on the console");
    const size_t game_dlls = AddCheck("The game's own DLLs");

    Finish(files, TestFiles(files) ? State::kPass : State::kFail);
    Finish(arena, TestArena(arena) ? State::kPass : State::kFail);
    Finish(commit, TestCommit(commit) ? State::kPass : State::kFail);
    Finish(trap, TestTrap(trap) ? State::kPass : State::kFail);
    Finish(dlls, TestDlls(dlls) ? State::kInfo : State::kFail);
    WriteReport();
    Finish(game_dlls, TestPackageDlls(game_dlls) ? State::kPass : State::kFail);
    WriteReport();

    Gpu gpu;
    const bool have_gpu = InitGpu(gpu, window_, gpu_check);
    Finish(gpu_check, have_gpu ? State::kPass : State::kFail);
    WriteReport();

    bool pad_seen = false;
    while (!closed_) {
      window_.Dispatcher().ProcessEvents(
          CoreProcessEventsOption::ProcessAllIfPresent);
      for (DWORD i = 0; i < XUSER_MAX_COUNT && !pad_seen; ++i) {
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) == ERROR_SUCCESS &&
            st.Gamepad.wButtons != 0) {
          pad_seen = true;
          Note(pad, "XInput pad %lu, buttons %04X", i,
               unsigned(st.Gamepad.wButtons));
          Finish(pad, State::kPass);
          WriteReport();
        }
      }
      if (have_gpu) {
        DrawReport();
        Present(gpu);
      } else {
        Sleep(16);
      }
    }
  }

  CoreWindow window_{nullptr};
  bool closed_ = false;
};

}  // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  CoreApplication::Run(make<App>());
  return 0;
}
