// NBA JAM: On Fire Edition - files outside the app's own storage, for
// everything in the process
//
// An app on the console reaches a USB drive or D:\DevelopmentFiles under
// broadFileSystemAccess, but only through the FromApp versions of the Win32
// file calls; the plain ones are refused there. The runtime, the C++ library
// (std::filesystem, fstream) and the C library (fopen) all use the plain
// ones, and the game data has to be read by all of them.
//
// So the plain calls are pointed at the FromApp ones, in the import tables of
// the modules that make them: this executable, the runtime and its plugin,
// and the C and C++ runtime libraries. A FromApp call tries the plain one
// first and brokers only what that refuses, so inside the app's own storage
// nothing changes. Nothing in the system's own libraries is touched, which
// includes the one the FromApp calls live in, so they cannot end up calling
// themselves.

#include "uwp_fromapp.h"

#include <windows.h>
#include <fileapifromapp.h>

#include <cstring>
#include <string>

#if WINAPI_FAMILY == WINAPI_FAMILY_APP
extern "C" __declspec(dllimport) BOOL WINAPI GetModuleHandleExW(DWORD flags, LPCWSTR name,
                                                                HMODULE* module);
#endif
#ifndef GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#endif

namespace {

HANDLE WINAPI HookCreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
                              LPCREATEFILE2_EXTENDED_PARAMETERS params) {
  return CreateFile2FromAppW(name, access, share, disposition, params);
}

HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share,
                              LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags,
                              HANDLE templ) {
  CREATEFILE2_EXTENDED_PARAMETERS p{};
  p.dwSize = sizeof(p);
  p.dwFileAttributes = flags & 0x0000FFFF;
  p.dwFileFlags = flags & 0xFFF00000;
  p.dwSecurityQosFlags = flags & 0x000F0000;
  p.lpSecurityAttributes = security;
  p.hTemplateFile = templ;
  return CreateFile2FromAppW(name, access, share, disposition, &p);
}

HANDLE WINAPI HookFindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data,
                                   FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
  return FindFirstFileExFromAppW(name, level, data, op, filter, flags);
}

HANDLE WINAPI HookFindFirstFileW(LPCWSTR name, LPWIN32_FIND_DATAW data) {
  return FindFirstFileExFromAppW(name, FindExInfoStandard, data, FindExSearchNameMatch, nullptr,
                                 0);
}

BOOL WINAPI HookGetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
  return GetFileAttributesExFromAppW(name, level, info);
}

DWORD WINAPI HookGetFileAttributesW(LPCWSTR name) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExFromAppW(name, GetFileExInfoStandard, &d)) {
    return INVALID_FILE_ATTRIBUTES;
  }
  return d.dwFileAttributes;
}

BOOL WINAPI HookCreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES security) {
  return CreateDirectoryFromAppW(name, security);
}

BOOL WINAPI HookRemoveDirectoryW(LPCWSTR name) { return RemoveDirectoryFromAppW(name); }

BOOL WINAPI HookDeleteFileW(LPCWSTR name) { return DeleteFileFromAppW(name); }

BOOL WINAPI HookSetFileAttributesW(LPCWSTR name, DWORD attributes) {
  return SetFileAttributesFromAppW(name, attributes);
}

BOOL WINAPI HookMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
  // MoveFileFromApp has no flags. Replacing what is there is the one most
  // callers ask for (std::filesystem::rename does), so do that by hand.
  if ((flags & MOVEFILE_REPLACE_EXISTING) && to) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (GetFileAttributesExFromAppW(to, GetFileExInfoStandard, &d) &&
        !(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      DeleteFileFromAppW(to);
    }
  }
  return MoveFileFromAppW(from, to);
}

BOOL WINAPI HookCopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists) {
  return CopyFileFromAppW(from, to, fail_if_exists);
}

// std::filesystem::copy_file's call.
HRESULT WINAPI HookCopyFile2(PCWSTR from, PCWSTR to, COPYFILE2_EXTENDED_PARAMETERS* params) {
  const BOOL fail_if_exists = params && (params->dwCopyFlags & COPY_FILE_FAIL_IF_EXISTS);
  return CopyFileFromAppW(from, to, fail_if_exists) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

struct Hook {
  const char* name;
  void* to;
};

const Hook kHooks[] = {
    {"CreateFile2", reinterpret_cast<void*>(&HookCreateFile2)},
    {"CreateFileW", reinterpret_cast<void*>(&HookCreateFileW)},
    {"FindFirstFileExW", reinterpret_cast<void*>(&HookFindFirstFileExW)},
    {"FindFirstFileW", reinterpret_cast<void*>(&HookFindFirstFileW)},
    {"GetFileAttributesExW", reinterpret_cast<void*>(&HookGetFileAttributesExW)},
    {"GetFileAttributesW", reinterpret_cast<void*>(&HookGetFileAttributesW)},
    {"CreateDirectoryW", reinterpret_cast<void*>(&HookCreateDirectoryW)},
    {"RemoveDirectoryW", reinterpret_cast<void*>(&HookRemoveDirectoryW)},
    {"DeleteFileW", reinterpret_cast<void*>(&HookDeleteFileW)},
    {"SetFileAttributesW", reinterpret_cast<void*>(&HookSetFileAttributesW)},
    {"MoveFileExW", reinterpret_cast<void*>(&HookMoveFileExW)},
    {"CopyFileW", reinterpret_cast<void*>(&HookCopyFileW)},
    {"CopyFile2", reinterpret_cast<void*>(&HookCopyFile2)},
};

// Points the named imports of one module at the hooks. Returns how many.
int PatchModule(HMODULE module) {
  auto* base = reinterpret_cast<uint8_t*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress || !dir.Size) return 0;
  int patched = 0;
  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       imp->Name; ++imp) {
    if (!imp->OriginalFirstThunk || !imp->FirstThunk) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      const char* fn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
      for (const Hook& h : kHooks) {
        if (std::strcmp(fn, h.name) != 0) continue;
        void** slot = reinterpret_cast<void**>(&slots->u1.Function);
        if (*slot == h.to) break;
        ULONG old = 0;
        if (VirtualProtectFromApp(slot, sizeof(void*), PAGE_READWRITE, &old)) {
          *slot = h.to;
          VirtualProtectFromApp(slot, sizeof(void*), old, &old);
          ++patched;
        }
        break;
      }
    }
  }
  return patched;
}

}  // namespace

int NbaRouteFilesThroughFromApp() {
  // The modules that open the game's files. A name that is not loaded is
  // skipped; calling this again later picks up what has loaded since.
  static const wchar_t* const kModules[] = {
      nullptr,  // this executable
      L"rexruntime.dll",       L"rexgpu-xenos.dll",    L"ucrtbase.dll",
      L"msvcp140_app.dll",     L"msvcp140_1_app.dll",  L"msvcp140_2_app.dll",
      L"vcruntime140_app.dll", L"vcruntime140_1_app.dll",
  };
  int total = 0;
  for (const wchar_t* name : kModules) {
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, name, &m) && m) {
      total += PatchModule(m);
    }
  }
  return total;
}
