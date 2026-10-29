# Xbox One (UWP, Developer Mode)

Three apps for an Xbox One in Developer Mode, built with clang-cl:

| App | Identity | Plays |
| --- | --- | --- |
| NBA JAM: On Fire Edition | `OpenJam.NBAJamOFE` | the base game |
| NBA JAM 2020s On Fire Edition | `OpenJam.NBAJamOFE.2020s` | johnz1's 2020s mod |
| NBA JAM Legends On Fire Edition | `OpenJam.NBAJamOFE.Legends` | johnz1's Legends mod |

All three use the same executable. On a PC, picking a mod relaunches the game
against that mod's folder. A UWP app can't start a process, so on the
console each mod is its own app, with its own tile and its own saves.

## Status: this does not run yet

The port's own code builds for the app container. **The ReXGlue SDK it links
against does not**, and until it does, the game will not start on a console.
This is what the SDK (v0.10.0) still needs. Each item is SDK work, not work in
this repository:

| What | Where in the SDK | Why |
| --- | --- | --- |
| A CoreWindow window and app loop | `src/ui/window_sdl.cpp`, `windowed_app_context_sdl.cpp`, `windowed_app_main_sdl.cpp` | Windowing is SDL3, and SDL3 dropped UWP support. A UWP app enters through `CoreApplication::Run`. |
| A CoreWindow swap chain | `src/ui/d3d12/d3d12_presenter.cpp` (around line 420) | Only `CreateSwapChainForHwnd` is implemented, under a desktop-only `#if`. |
| Packaged library loading | `src/ui/d3d12/d3d12_provider.cpp` | `LoadLibraryW` of `D3D12.dll`, `dxgi.dll`, `D3DCompiler_47.dll` and `dxcompiler.dll` is desktop-only. An app uses `LoadPackagedLibrary`, or links directly. |
| Logs in app storage | logging setup | Logs are written beside the executable, and an installed package is read-only. |
| Third-party libraries | `thirdparty/` | FFmpeg, Tracy, spdlog and the rest have to be built with `WINAPI_FAMILY=WINAPI_FAMILY_APP`. |

The guest memory layer is already done: `src/core/memory_win.cpp` and
`mapped_memory_win.cpp` have `VirtualAllocFromApp` / `MapViewOfFile3FromApp`
paths for the app partition.

## What this repository does for it

- **`NBAJAM_UWP` in `CMakeLists.txt`.** Builds for `WINAPI_FAMILY_APP`,
  links `/APPCONTAINER` against `WindowsApp.lib` and MSVC's app CRT, and
  leaves out the crash reporter and profiler, which need DbgHelp.
- **`xbox-uwp-release` / `xbox-uwp-relwithdebinfo` presets.** Ninja and
  clang-cl.
- **`src/uwp_platform.cpp`.** Replaces `mod_swap.cpp` and `watchdog.cpp`. With
  no mod to swap to and no process to restart, the in-game chooser finds an
  empty list. Saves go to the app's LocalState folder.
- **Small guards in existing code.** `null_page.cpp` uses the `FromApp` memory
  calls, `host_pad.cpp` links `xinputuap.lib`, `apt_trace.cpp` skips its
  DbgHelp stack dump, and `--unpack` is off.
- **`packaging/xbox/`.** The manifest template and tile images.
- **`tools/package_xbox.ps1`.** Builds and signs the three `.appx` files.

## Building

You need Windows with:

- Visual Studio 2022 with the **Universal Windows Platform development**
  workload. That workload provides MSVC's `lib\x64\store` app CRT and the
  VCLibs framework package.
- LLVM, for `clang-cl`, plus CMake 3.25 or later and Ninja.
- A **UWP build of the ReXGlue SDK** (see Status above). Set
  `REXGLUE_UWP_SDK` to its install prefix.
- The staged PC game with both mods installed (`python tools/dlc.py stage`;
  see RELEASE.md).

From an **x64 Native Tools Command Prompt for VS 2022**, which sets
`VCToolsInstallDir`:

```
:: Codegen once with your normal desktop preset. generated/ sits in the
:: source tree and both builds share it.
cmake --build --preset local-relwithdebinfo --target nbajam_ofe_codegen

set REXGLUE_UWP_SDK=C:\path\to\rexglue-uwp
cmake --preset xbox-uwp-release
cmake --build --preset xbox-uwp-release

powershell -ExecutionPolicy Bypass -File tools\package_xbox.ps1 ^
    -GameFolder "D:\...\NBA JAM On Fire Edition PC"
```

The script finds the two mods under `mods\` by name. If it can't, name the
folders with `-Mod2020Id` and `-ModLegendsId`. Use `-Variant base`
(or `2020`, or `legends`) to build only one package. The output goes to
`out\appx\`:

```
NBAJamOFE_1.0.0.0_x64.appx
NBAJamOFE-2020s_1.0.0.0_x64.appx
NBAJamOFE-Legends_1.0.0.0_x64.appx
OpenJam.cer / OpenJam.pfx           self-signed, made on first run and reused
Microsoft.VCLibs.x64.14.00.appx     the CRT framework the apps depend on
```

Each package holds a whole game, about 850 MB, taken from your own copy.
**They are for your own console. Do not share them.**

## Installing on the console

1. Put the console in Developer Mode (Dev Mode Activation app plus a
   Partner Center developer account).
2. In Dev Home, turn on **Remote Access** (Device Portal) and note the address.
3. Open `https://<console address>:11443` in a browser. Under
   **Add**, choose an `.appx` and add `Microsoft.VCLibs.x64.14.00.appx` as its
   dependency. Repeat for each of the three.
4. In Dev Home, select each app, open **View details**, and set
   **App type** to **Game**. As an "App" it gets a small share of memory and
   the GPU, which this game will not fit in.

To update an app, raise `-Version` and install again.

## Known differences from the PC build

- The main menu's mod panel still lists every mod staged on the PC, because
  the package carries that rewritten front end. LT+Y opens an empty chooser.
  To switch mods, go back to the console's home screen and pick the other app.
- There is no watchdog. A game that hangs is closed from the console's own
  menu.
- Saves are kept per app, in that app's LocalState. Uninstalling an app
  deletes its saves.
