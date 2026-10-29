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
#include <cstring>
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
using VehHandler = LONG(NTAPI*)(EXCEPTION_POINTERS*);
extern "C" __declspec(dllimport) PVOID WINAPI
AddVectoredExceptionHandler(ULONG first, VehHandler handler);
extern "C" __declspec(dllimport) ULONG WINAPI
RemoveVectoredExceptionHandler(PVOID handle);

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

    Finish(files, TestFiles(files) ? State::kPass : State::kFail);
    Finish(arena, TestArena(arena) ? State::kPass : State::kFail);
    Finish(commit, TestCommit(commit) ? State::kPass : State::kFail);
    Finish(trap, TestTrap(trap) ? State::kPass : State::kFail);
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
