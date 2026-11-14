# Infinite Undiscovery Recomp on the PS5 (homebrew)

This branch (`ps5-port`) builds Infinite Undiscovery Recomp as a homebrew title
for a jailbroken PS5 (title id `PPSA99215`). Like the Fable 2 PS5 port, it
renders through the ReXGlue SDK's Xenos GPU emulation on its Vulkan backend,
with Mesa's RADV linked into the title, and runs on the SDK's PS5 port
(github.com/dragonscales123/rexglue-sdk-PS5, branch `fable2-ps5`: the PS5
port with the Xenos GPU working on the console).

Status: **work in progress, not yet booted on a console.**

You need your own copy of Infinite Undiscovery (USA or Europe). Nothing from
the game is in this repository, and nothing derived from it should be
published.

## What you need

- Ubuntu 24.04 (WSL works; keep the work tree in the Linux filesystem, not
  under `/mnt/c`).
- clang/lld 20 (`clang-20`, `clang++-20`).
- mihawk-99's PS5 toolchain, as for re:Blue PS5 and Fable 2 PS5: the payload
  SDK, and PS5_Vulkan with its RADV release archive built. `ps5/build.sh`
  looks next to this checkout, then in `~/bluedragon-ps5`; `--sdk` and
  `--vulkan` point it elsewhere.
- This repository with its SDK submodule:

  ```
  git clone -b ps5-port <this repository> infinite-undiscovery-recompPS5
  cd infinite-undiscovery-recompPS5
  git submodule update --init thirdparty/rexglue-sdk
  ```

## Build

```
ps5/build.sh --game "/mnt/c/.../Infinite Undiscovery"
```

`--game` is the folder with your extracted discs, as `Disc 1/` and `Disc 2/`
(or `disc1/`, `disc2/`). The build only reads Disc 1's `default.xex`, which the
code generator needs; it copies it to `assets/default.xex` (ignored by git).
The USA and Europe Disc 1 executables decrypt to the same image
(ASSET_SETUP.md), so the same generated code serves both.

Other options: `--sdk`, `--vulkan`, `--title-id` (default `PPSA99215`),
`--copy-to DIR` (also copies the title folder to `DIR/PPSA99215/` and the
unstripped ELF to `DIR/symbols/iu.elf`). Settings can live in
`ps5/build.local.sh` (ignored by git):

```
game="/mnt/c/Users/<you>/Desktop/Infinite Undiscovery Recomp/game/Infinite Undiscovery"
copy_to="/mnt/c/Users/<you>/Desktop/Infinite Undiscovery Recomp/ps5-work/ps5-dist"
```

`IU_PS5_DEBUG_HEAP=ON ps5/build.sh` builds the checking allocator
(`src/platform/ps5_debug_heap.cpp`) for chasing heap damage.

The result is `out/build/ps5/dist/PPSA99215/`: `eboot.bin`, `sce_sys/`,
`sce_module/`, empty `assets/disc1/` and `assets/disc2/`, and
`ps5-env.example.txt`.

## Install

The title folder is the game's portable root, laid out as the Windows Asset
Setup wizard lays out a content profile:

```
/data/homebrew/PPSA99215/
  eboot.bin, sce_sys/, sce_module/     from the build
  assets/disc1/   default.xex, ud1.bin, ud2.bin    (Disc 1)
  assets/disc2/   default.xex, ud1.bin, ud2.bin    (Disc 2)
  saves/  shaders/  cache/  logs/                  written by the game
```

There is no setup wizard on the console, and the disc files are not hashed
there: copy the files as they are on your discs.

Logs go to `logs/runtime.log`; `boot.txt` records start-up and any early
crash. `ps5-env.txt` (from `ps5-env.example.txt`) sets SDK options.

## What the PS5 port changes

- `cmake/IuPs5.cmake`, `ps5/`: PS5 build, link and packaging (from the Fable 2
  PS5 port).
- `src/platform/`: title start-up, crash breadcrumbs and exit to the shell
  (`ps5_title.cpp`), libc functions the payload SDK lacks (`ps5_libc.cpp`),
  shorter RADV GPU waits (`ps5_radv_wait.cpp`), the optional checking
  allocator.
- `src/ps5/iu_ps5_app.h`: the console app. Paths are pinned under `/app0`;
  the Asset Setup wizard, profile manager, Win32 dialogs, community debug
  overlay and D3D12/PSO diagnostics are left out.
- Two hooks over SDK functions that Windows reaches through
  `rexruntime.dll` are link-time wraps on the PS5
  (`--wrap=rexcrt_SwitchToFiber --wrap=__imp__XamSwapDisc`):
  - the fiber switch keeps each continuation's 64-bit LR (`fiber_lr.h`);
  - XamSwapDisc selects Disc 1 or Disc 2 (`disc_swap.h`), which the console
    build shares with Windows. Both discs are mounted before the guest
    starts; a swap only repoints `game:`, `d:` and `\Device\Cdrom0`.

## Not yet on the PS5

- DLC (A Voucher, B Voucher) installation.
- Save Anywhere and the recovery-move debug actions.
- Controller-only menus: the Windows build's extra actions are on F-keys.
- The in-game Disc 1 to Disc 2 change is untested on Windows too.
