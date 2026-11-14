#!/usr/bin/env bash
# Infinite Undiscovery Recomp - build the PS5 homebrew title (docs/PS5.md),
# from Fable 2 Recomp PS5's ps5/build.sh.
#
#   ps5/build.sh [--sdk DIR] [--vulkan DIR] [--game DIR] [--title-id PPSAxxxxx] [--copy-to DIR]
#
# --game DIR (or game=DIR in ps5/build.local.sh): where your Disc 1
# default.xex is, as DIR/Disc 1/default.xex, DIR/disc1/default.xex or
# DIR/default.xex (default: game/ in this checkout). Only default.xex is read,
# by the code generator; the disc files go to the console (docs/PS5.md).
#
# --copy-to DIR (or copy_to=DIR in ps5/build.local.sh, which git ignores):
# also copy the finished title folder to DIR/<TITLE_ID>/, e.g. a Windows
# folder under /mnt/c, and the unstripped ELF to DIR/symbols/iu.elf.
#
# Needs: your own Infinite Undiscovery default.xex (USA or Europe), the SDK
# submodule (thirdparty/rexglue-sdk, branch iu-ps5), a PS5 payload SDK and
# PS5_Vulkan with its RADV release archive built, and clang/lld 20 on the host.
# Writes out/build/ps5/dist/<TITLE_ID>/.
#
# The SDK's code generator (rexglue) runs on this machine, never on the
# console. The game's shaders are translated at run time by the SDK's Xenos
# GPU, so there is no host shader step.
#
# Paths default to the work folder this checkout sits in, then to
# ~/bluedragon-ps5 (the re:Blue PS5 toolchain), or PS5_PAYLOAD_SDK and
# PS5_VULKAN_DIR from the environment.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
parent=$(dirname "$root")
first_dir() { # the first argument that is a directory
    local dir
    for dir in "$@"; do
        [[ -n $dir && -d $dir ]] && { echo "$dir"; return; }
    done
    echo "${1:-}"
}
# re:Blue's default first (the built SDK PS5_VulkanTemplate's bootstrap makes).
sdk=${PS5_PAYLOAD_SDK:-$(first_dir "$parent/PS5_VulkanTemplate/.deps/native/ps5-payload-sdk" \
    "$HOME/bluedragon-ps5/PS5_VulkanTemplate/.deps/native/ps5-payload-sdk" \
    "$parent/PS5_PayloadSDK/host" "$HOME/bluedragon-ps5/PS5_PayloadSDK/host")}
vulkan=${PS5_VULKAN_DIR:-$(first_dir "$parent/PS5_Vulkan" "$HOME/bluedragon-ps5/PS5_Vulkan")}
rexsdk=$root/thirdparty/rexglue-sdk
title_id=PPSA99215
copy_to=${IU_PS5_COPY_TO:-}
game=${IU_GAME_DIR:-$root/game}
# Personal settings (copy_to=..., game=..., PS5_PAYLOAD_SDK=..., PS5_VULKAN_DIR=...).
if [[ -f $root/ps5/build.local.sh ]]; then
    # shellcheck source=/dev/null
    source "$root/ps5/build.local.sh"
    sdk=${PS5_PAYLOAD_SDK:-$sdk}
    vulkan=${PS5_VULKAN_DIR:-$vulkan}
fi
while (($#)); do
    case $1 in
        --sdk) sdk=$2; shift 2 ;;
        --vulkan) vulkan=$2; shift 2 ;;
        --title-id) title_id=$2; shift 2 ;;
        --copy-to) copy_to=$2; shift 2 ;;
        --game) game=$2; shift 2 ;;
        *) echo "usage: ${0##*/} [--sdk DIR] [--vulkan DIR] [--game DIR] [--title-id PPSAxxxxx] [--copy-to DIR]" >&2; exit 2 ;;
    esac
done
echo "payload SDK: $sdk${PS5_PAYLOAD_SDK:+ (from PS5_PAYLOAD_SDK)}"
echo "PS5_Vulkan:  $vulkan${PS5_VULKAN_DIR:+ (from PS5_VULKAN_DIR)}"
[[ -f $sdk/toolchain/prospero.cmake && -x $sdk/bin/prospero-lld ]] ||
    { echo "no built PS5 payload SDK at $sdk (--sdk; needs toolchain/ and bin/prospero-lld)" >&2; exit 2; }
[[ -f $vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a ]] ||
    { echo "no RADV release archive in $vulkan (tools/build-radv.sh release)" >&2; exit 2; }
if [[ ! -f $rexsdk/include/rex/platform.h ]]; then
    git -C "$root" submodule update --init thirdparty/rexglue-sdk
fi
grep -q REX_PLATFORM_PS5 "$rexsdk/include/rex/platform.h" ||
    { echo "thirdparty/rexglue-sdk is not the PS5 SDK (rexglue-sdk-PS5, branch iu-ps5)" >&2; exit 2; }
git -C "$rexsdk" submodule update --init --recursive --depth 1 > /dev/null

# Codegen reads assets/default.xex (infinite_undiscovery_manifest.toml): Disc
# 1's executable. The USA and Europe executables decrypt to the same image, so
# either works (ASSET_SETUP.md); a copy is made so codegen sees a plain file.
xex=
for candidate in "$game/Disc 1/default.xex" "$game/disc1/default.xex" "$game/default.xex" \
        "$game/Infinite Undiscovery/Disc 1/default.xex"; do
    [[ -f $candidate ]] && { xex=$candidate; break; }
done
mkdir -p "$root/assets"
if [[ -n $xex ]]; then
    cmp -s "$xex" "$root/assets/default.xex" || cp "$xex" "$root/assets/default.xex"
fi
[[ -f $root/assets/default.xex ]] ||
    { echo "no Disc 1 default.xex under $game (--game DIR; docs/PS5.md)" >&2; exit 2; }
case $(sha256sum "$root/assets/default.xex" | cut -d' ' -f1) in
    22893bb8d96a1440ecbdbcae543baeaf89d26588c89c99a2c96fecf611475325) echo "default.xex: Europe (PAL) Disc 1" ;;
    9523b45e6a724d4988ce9cf70d55b672e461b89303b02dc2f0f9c84329c25555) echo "default.xex: USA Disc 1" ;;
    *) echo "warning: assets/default.xex is not a validated USA or Europe Disc 1 executable (ASSET_SETUP.md)" >&2 ;;
esac

# Submodule changes the PS5 needs, kept as patches because the submodules
# point at upstream: SDL and FFmpeg in the SDK. Each is applied once; an
# already-applied patch is left alone.
apply_patch() { # repo-dir patch
    if git -C "$1" apply --check -R "$2" 2> /dev/null; then
        return
    fi
    git -C "$1" apply --check "$2" ||
        { echo "${2##*/} neither applies nor is applied in $1" >&2; exit 1; }
    git -C "$1" apply --whitespace=nowarn "$2"
    echo "applied ${2##*/} in $1"
}
apply_patch "$rexsdk/thirdparty/sdl3" "$rexsdk/tools/patches/sdl-ps5.patch"
apply_patch "$rexsdk/thirdparty/FFmpeg" "$rexsdk/tools/patches/ffmpeg-ps5.patch"

host=$root/out/build/ps5-host
mkdir -p "$host"

# 1. The SDK's code generator, for this machine.
rexglue=$host/rexglue-sdk/bin/rexglue
if [[ ! -x $rexglue ]]; then
    cmake -S "$rexsdk" -B "$host/rexglue-sdk" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_LINKER_TYPE=LLD \
        -DREXGLUE_ENABLE_TRACY=OFF > "$host/rexglue-sdk.configure.log"
    cmake --build "$host/rexglue-sdk" --target rexglue
    mkdir -p "$(dirname "$rexglue")"
    # The SDK writes its binaries to <sdk>/out/<platform>/.
    built=$(find "$rexsdk/out" -maxdepth 2 -type f -name rexglue -perm -u+x | head -1)
    [[ -n $built ]] || { echo "rexglue was not built" >&2; exit 1; }
    ln -sf "$built" "$rexglue"
else
    # Keep it current with the SDK sources (a no-op when nothing changed).
    cmake --build "$host/rexglue-sdk" --target rexglue > "$host/rexglue-sdk.build.log"
fi
# The code templates are compiled into rexglue but are not part of codegen's
# input fingerprint, so a rebuilt generator regenerates the game code.
codegen_stamp=$root/generated/default/codegen.build.stamp
if [[ -f $codegen_stamp && $(readlink -f "$rexglue") -nt $codegen_stamp ]]; then
    echo "rexglue changed: regenerating the recompiled code"
    rm -f "$codegen_stamp" "$root/generated/default/codegen.stamp" "$root/generated/default/recomp_patches.stamp"
fi

# 2. The title.
build=$root/out/build/ps5
# A build tree is tied to its toolchain: start afresh when it changes.
# (Recorded in our own file: the PS5 toolchain file rewrites CMake's cached
# CMAKE_TOOLCHAIN_FILE, so the cache can't be compared.)
toolchain=$(readlink -f "$sdk/toolchain/prospero.cmake")
toolchain_record=$build/ps5-toolchain.txt
if [[ -d $build && -f $toolchain_record && $(cat "$toolchain_record") != "$toolchain" ]]; then
    echo "toolchain changed: starting $build afresh"
    rm -rf "$build"
fi
mkdir -p "$build"
echo "$toolchain" > "$toolchain_record"
configure() {
    cmake -S "$root" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
        -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DREXSDK_DIR="$rexsdk" -DREXGLUE_HOST_TOOL="$rexglue" \
        -DREXGLUE_ENABLE_TRACY=OFF \
        -DIU_PS5_VULKAN_DIR="$vulkan" -DIU_PS5_TITLE_ID="$title_id" \
        -DIU_PS5_DEBUG_HEAP="${IU_PS5_DEBUG_HEAP:-OFF}"
}
configure
# Codegen writes generated/default/sources.cmake; configure again to compile it.
cmake --build "$build" --target infinite_undiscovery_codegen
configure
cmake --build "$build" --target infinite_undiscovery ${BUILD_JOBS:+-j "$BUILD_JOBS"}
echo "title folder: $build/dist/$title_id"
if [[ -n $copy_to ]]; then
    dest=$copy_to/$title_id
    mkdir -p "$dest"
    (cd "$build/dist/$title_id" && tar -cf - .) | tar -xf - -C "$dest"
    # The unstripped link output, for symbolizing faults and profiles
    # (kept beside the title folder, not inside it, so it never goes to the console).
    mkdir -p "$copy_to/symbols"
    cp "$build/ps5-link/link/llvm-pie.elf" "$copy_to/symbols/iu.elf"
    echo "copied to: $dest (symbols: $copy_to/symbols/iu.elf)"
fi
echo "copy it to /data/homebrew/$title_id/, with Disc 1 and Disc 2 (default.xex, ud1.bin, ud2.bin) in its assets/disc1/ and assets/disc2/ (docs/PS5.md)"
