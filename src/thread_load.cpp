// NBA JAM: On Fire Edition - how busy each thread is
//
// The frame rate says how slow the game is; this says where. Every thread
// the runtime knows - the game's own and the runtime's, such as the one
// that turns the game's GPU commands into the host's - is asked how much
// CPU time it has used, and the difference since last time, over the wall
// clock, is how much of a core it kept busy. One that sits near 100% is
// what the rest are waiting on.

#include "thread_load.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

namespace {

rex::Runtime* g_runtime = nullptr;
std::mutex g_mutex;
std::map<void*, uint64_t> g_last;   // native handle -> CPU time, 100 ns
std::chrono::steady_clock::time_point g_last_at;

uint64_t CpuTime(void* handle) {
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(static_cast<HANDLE>(handle), &created, &exited, &kernel, &user)) return 0;
  auto u64 = [](const FILETIME& f) { return uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime; };
  return u64(kernel) + u64(user);
#else
  (void)handle;
  return 0;
#endif
}

}  // namespace

void NbaThreadLoadUse(rex::Runtime* runtime) {
  std::lock_guard lock(g_mutex);
  g_runtime = runtime;
  g_last_at = std::chrono::steady_clock::now();
}

std::string NbaThreadLoad() {
  std::lock_guard lock(g_mutex);
  if (!g_runtime || !g_runtime->kernel_state()) return {};
  const auto now = std::chrono::steady_clock::now();
  const double wall =
      std::chrono::duration<double>(now - g_last_at).count() * 1e7;  // in 100 ns
  g_last_at = now;
  struct Load {
    std::string name;
    double share;
  };
  std::vector<Load> loads;
  std::map<void*, uint64_t> seen;
  auto threads = g_runtime->kernel_state()
                     ->object_table()
                     ->GetObjectsByType<rex::system::XThread>();
  for (auto& t : threads) {
    rex::thread::Thread* native = t->thread();
    if (!native) continue;
    void* handle = native->native_handle();
    const uint64_t cpu = CpuTime(handle);
    seen[handle] = cpu;
    auto it = g_last.find(handle);
    if (it == g_last.end() || wall <= 0) continue;
    const double share = double(cpu - it->second) / wall;
    if (share >= 0.05) loads.push_back({native->name(), share});
  }
  g_last = std::move(seen);
  std::sort(loads.begin(), loads.end(), [](const Load& a, const Load& b) { return a.share > b.share; });
  std::string out;
  for (size_t i = 0; i < loads.size() && i < 5; ++i) {
    char part[96];
    std::snprintf(part, sizeof(part), "%s%s %.0f%%", i ? ", " : "", loads[i].name.c_str(),
                  loads[i].share * 100.0);
    out += part;
  }
  return out;
}
