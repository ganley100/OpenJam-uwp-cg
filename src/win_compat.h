// NBA JAM: On Fire Edition - the few Windows calls, away from Windows
//
// The game's own code was written for Windows and leans on a handful of its
// calls: environment variables, the executable's path, a millisecond clock,
// and asking whether guest memory is there before reading it. On Windows this
// is just <windows.h>. On iOS each of those has a plain POSIX equivalent,
// given the Windows name here so the files that use them stay as they are.

#pragma once

#if defined(_WIN32)
#include <windows.h>
#else

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <string>

#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/mman.h>

using DWORD = uint32_t;
using ULONG = unsigned long;
using USHORT = unsigned short;
using SIZE_T = size_t;
using ULONGLONG = unsigned long long;
using LPCVOID = const void*;

#ifndef MAX_PATH
#define MAX_PATH 1024
#endif
#define _TRUNCATE (static_cast<size_t>(-1))

// The values Windows uses, so masks written against them still mean the same.
constexpr DWORD PAGE_NOACCESS = 0x01;
constexpr DWORD PAGE_READONLY = 0x02;
constexpr DWORD PAGE_READWRITE = 0x04;
constexpr DWORD PAGE_WRITECOPY = 0x08;
constexpr DWORD PAGE_EXECUTE_READ = 0x20;
constexpr DWORD PAGE_EXECUTE_READWRITE = 0x40;
constexpr DWORD PAGE_EXECUTE_WRITECOPY = 0x80;
constexpr DWORD PAGE_GUARD = 0x100;
constexpr DWORD MEM_COMMIT = 0x1000;
constexpr DWORD MEM_RESERVE = 0x2000;
constexpr DWORD MEM_FREE = 0x10000;

struct MEMORY_BASIC_INFORMATION {
  void* BaseAddress;
  void* AllocationBase;
  DWORD AllocationProtect;
  SIZE_T RegionSize;
  DWORD State;
  DWORD Protect;
  DWORD Type;
};

inline DWORD GetLastError() { return static_cast<DWORD>(errno); }

inline ULONGLONG GetTickCount64() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ULONGLONG(ts.tv_sec) * 1000u + ULONGLONG(ts.tv_nsec) / 1000000u;
}

// Its length when it fits, the size it would need when it does not, 0 when
// unset - as Windows answers.
inline DWORD GetEnvironmentVariableA(const char* name, char* buf, DWORD size) {
  const char* v = std::getenv(name);
  if (!v) return 0;
  const size_t n = std::strlen(v);
  if (n + 1 > size) return DWORD(n + 1);
  std::memcpy(buf, v, n + 1);
  return DWORD(n);
}

inline DWORD GetModuleFileNameA(void*, char* buf, DWORD size) {
  uint32_t n = size;
  if (_NSGetExecutablePath(buf, &n) != 0) return 0;
  return DWORD(std::strlen(buf));
}

inline DWORD GetModuleFileNameW(void*, wchar_t* buf, DWORD size) {
  char path[MAX_PATH]{};
  if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) return 0;
  const size_t n = std::mbstowcs(buf, path, size);
  return n == static_cast<size_t>(-1) ? 0 : DWORD(n);
}

// Darwin reports a region at or above the address asked about; one that
// starts above it means the address itself is in a hole. A reserved range
// that nothing has committed yet is mapped with no access, which is what the
// runtime does for the guest's arena.
inline SIZE_T VirtualQuery(LPCVOID p, MEMORY_BASIC_INFORMATION* m, SIZE_T) {
  vm_address_t address = reinterpret_cast<vm_address_t>(p);
  vm_size_t size = 0;
  vm_region_basic_info_data_64_t info{};
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  if (vm_region_64(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                   reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS) {
    return 0;
  }
  *m = {};
  if (address > reinterpret_cast<vm_address_t>(p)) {
    m->BaseAddress = const_cast<void*>(p);
    m->RegionSize = address - reinterpret_cast<vm_address_t>(p);
    m->State = MEM_FREE;
    m->Protect = PAGE_NOACCESS;
    return sizeof(*m);
  }
  m->BaseAddress = reinterpret_cast<void*>(address);
  m->AllocationBase = m->BaseAddress;
  m->RegionSize = size;
  const vm_prot_t prot = info.protection;
  if (!(prot & VM_PROT_READ)) {
    m->State = MEM_RESERVE;
    m->Protect = PAGE_NOACCESS;
  } else {
    m->State = MEM_COMMIT;
    const bool w = prot & VM_PROT_WRITE, x = prot & VM_PROT_EXECUTE;
    m->Protect = x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                   : (w ? PAGE_READWRITE : PAGE_READONLY);
  }
  return sizeof(*m);
}

// Committing inside the runtime's reservation is opening its pages.
inline void* VirtualAlloc(void* at, SIZE_T size, DWORD, DWORD) {
  return mprotect(at, size, PROT_READ | PROT_WRITE) == 0 ? at : nullptr;
}

inline int VirtualProtect(void* at, SIZE_T size, DWORD, DWORD* was) {
  *was = 0;
  return mprotect(at, size, PROT_READ | PROT_WRITE) == 0;
}

inline int fopen_s(FILE** f, const char* name, const char* mode) {
  *f = std::fopen(name, mode);
  return *f ? 0 : errno;
}

// std::filesystem::path::c_str() is narrow here; only the mode is wide.
inline int _wfopen_s(FILE** f, const char* name, const wchar_t* mode) {
  char m[8]{};
  std::wcstombs(m, mode, sizeof(m) - 1);
  return fopen_s(f, name, m);
}

template <size_t N>
inline int strncpy_s(char (&dst)[N], const char* src, size_t) {
  std::strncpy(dst, src, N - 1);
  dst[N - 1] = 0;
  return 0;
}

#endif
