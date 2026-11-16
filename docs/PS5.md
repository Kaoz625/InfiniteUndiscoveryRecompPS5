# Infinite Undiscovery Recomp on the PS5 (homebrew)

This branch (`ps5-port`) builds Infinite Undiscovery Recomp as a homebrew title
for a jailbroken PS5 (title id `PPSA99215`). Like the Fable 2 PS5 port, it
renders through the ReXGlue SDK's Xenos GPU emulation on its Vulkan backend,
with Mesa's RADV linked into the title, and runs on the SDK's PS5 port
(github.com/dragonscales123/rexglue-sdk-PS5, branch `iu-ps5`: the `fable2-ps5`
branch, where the Xenos GPU works on the console, plus fixes this game needs).

Status: **playable, work in progress.** Tested on the console so far: the
opening logos and intro movie, the title screen, a new game, field
exploration, battles, chests, cutscenes, saving and loading. It runs at
**60 fps at normal game speed** in the areas tested, rendered at twice the
game's width with FXAA and sharpening (see "Frame rate and picture" below).
A full playthrough, the in-game Disc 1 to Disc 2 change and DLC are still to
do.

Fixed so far on the PS5:

- A crash when the opening credits start: the game switches to the fiber
  that is already running, and the PS5 fiber switch now returns at once
  instead of resuming from a stale stack pointer.
- Everything far too dark: the Vulkan shader translator read a texture's
  exponent bias from the wrong fetch-constant word, so the game's HDR scene
  reached the tone mapper 32 times too dark.
- A flickering half-screen copy on the title screen: the SDK's PS5-only
  "hot page" uploads left stale data in the HDR scene image. They are off by
  default for this game (`src/platform/ps5_title.cpp`);
  `REX_SHARED_MEMORY_HOT_PAGES=true` in `ps5-env.txt` turns them back on.
- Gameplay stuck at 18–25 fps (the Windows build dips the same way): threads
  queued on the SDK's global lock, taken by the game's interrupt-disable
  sections, kernel object lookups and the write-fault handler. The lock now
  spins briefly before sleeping (`REX_GLOBAL_LOCK_SPIN`, default 100 on the
  PS5), which gives a locked 30.
- 60 fps at the right speed: the game counts time in vblanks and its frame
  limiter waits 60 / target vblanks per frame (target 30).
  `src/frame_rate.h` raises the target to `IU_TARGET_FPS`, so each frame
  waits one vblank and the game still advances 1/60 s per frame.

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
crash, and the SDK writes faults to `faults.txt`. `ps5-env.txt` (from
`ps5-env.example.txt`) sets SDK options, one `REX_<SETTING>=value` per line.
Diagnostic options in the `iu-ps5` SDK branch: `REX_GPU_LOG_SHADER_DRAWS`
(pixel shader hashes: log their constants and texture fetches),
`REX_GPU_LOG_RESOLVE_SUMMARY` (every N resolves, list each distinct resolve,
rarest first), `REX_GPU_GAMMA_RAMP_BYPASS` and `REX_DUMP_SHADERS` (a folder).

## Frame rate and picture

Defaults (set in `src/platform/ps5_title.cpp`; any of them can be changed in
`ps5-env.txt`):

| Setting | Default | What it does |
|---|---|---|
| `IU_TARGET_FPS` | `60` | Game frame-rate target. `30` (or `0`) is the original. |
| `REX_RESOLUTION_SCALE` | `1` | Render scale for both axes. |
| `REX_DRAW_RESOLUTION_SCALE_X` | `2` | Twice the game's width. |
| `REX_SWAP_POST_EFFECT` | `fxaa` | FXAA on the final image (`none` to turn off). |
| `REX_SWAP_SHARPNESS` | `0.6` | Contrast-adaptive sharpening, 0 (off) to 1. With FXAA, edges get FXAA and the rest is sharpened. |
| `REX_ANISOTROPIC_OVERRIDE` | `5` | 16x anisotropic texture filtering. |

GPU time per frame in a busy area: about 7 ms at the original resolution,
12 ms at twice the width (the default), and 18–20 ms at twice the width and
height, which is over the 16.7 ms a 60 fps frame allows. For full 2x at a
locked 30 instead, put this in `ps5-env.txt`:

```
REX_RESOLUTION_SCALE=2
IU_TARGET_FPS=30
```

Note that `REX_RESOLUTION_SCALE` overrides the per-axis scales unless they
are set to something other than 1.

L3 + R3 toggles an on-screen readout of FPS, frame time, GPU-emulation load
and the build number; `REX_SHOW_DEBUG_OVERLAY=true` opens it at start and
`REX_DEBUG_OVERLAY_COMPACT=false` shows the full panel. `REX_PROFILE=1`
writes a sampling profile to `profile.txt` every `REX_PROFILE_INTERVAL`
seconds, including frames per second and real PS5 GPU time
(`REX_GPU_PHASES=1` breaks GPU time down by kind of work).

## Fibers

The game runs a pool of over 500 guest fibers. On the PS5 each one's host
stack is a plain heap block (`REX_FIBER_HOST_STACK_KB`, default 256) with
nothing below it; a fiber that needs more overwrites the block beneath it,
often another fiber's saved frame, and that fiber then crashes on resume at a
guest address. The `iu-ps5` SDK branch makes the size a setting and logs
`Fiber N: host stack overflow` when a fiber's guard pattern is damaged.
The default has been enough so far; the opening-credits crash turned out to be
a switch to the running fiber, not an overflow.

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
- The in-game Disc 1 to Disc 2 change hasn't been played to on the PS5 yet
  (it works on Windows). Disc 2 is mounted at start, and
  `IU_DISC_SWAP_SELFTEST=1` checks the swap path at boot (it passes).
- A full playthrough.
