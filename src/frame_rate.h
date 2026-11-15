#pragma once
// 60 fps experiments (PS5 build). Infinite Undiscovery asks the Xbox 360 D3D
// runtime to show each frame after 2 vblanks (30 fps at 60 Hz). The swap
// scheduler, guest sub_827E6CF8 (D3D's swap queue: r3 = frontbuffer page bits
// | presentation interval << 8 | flags), queues the frame for vblank
// last_swap + interval. Overriding the weak recompiled symbol lets us log the
// interval the game asks for and, with IU_PRESENT_INTERVAL=1 in ps5-env.txt,
// ask for every vblank instead (up to 60 fps at the normal vblank rate).
// Whether the game then runs at the right speed depends on how it steps its
// own time: from vblanks counted (right speed) or a fixed step per frame
// (double speed).

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <rex/logging.h>
#include <rex/ppc/context.h>

namespace iu::frame_rate {

inline int ForcedInterval() {
  static const int value = [] {
    const char* env = std::getenv("IU_PRESENT_INTERVAL");
    return env && *env ? std::atoi(env) : 0;
  }();
  return value;
}

inline int TargetFps() {
  static const int value = [] {
    const char* env = std::getenv("IU_TARGET_FPS");
    return env && *env ? std::atoi(env) : 0;
  }();
  return value;
}

inline uint32_t* GuestWord(uint8_t* base, uint32_t address) {
  return reinterpret_cast<uint32_t*>(base + address + (address >= 0xE0000000u ? 0x1000u : 0u));
}

inline std::atomic<uint32_t> g_limiter_logs{0};
inline std::atomic<uint32_t> g_calls{0};
inline std::atomic<int> g_last_interval{-1};

}  // namespace iu::frame_rate

extern "C" void __imp__sub_827E6CF8(PPCContext& ctx, uint8_t* base);

extern "C" void sub_827E6CF8(PPCContext& ctx, uint8_t* base) {
  using namespace iu::frame_rate;
  const uint32_t arg = ctx.r3.u32;
  const int interval = int((arg >> 8) & 0xF);
  const uint32_t call = g_calls.fetch_add(1);
  if (call < 4 || g_last_interval.exchange(interval) != interval) {
    REXLOG_INFO("[frame-rate] swap #{} arg=0x{:08X} interval={} (forced {})", call, arg, interval,
                ForcedInterval());
  }
  if (const int forced = ForcedInterval(); forced > 0 && interval > forced) {
    ctx.r3.u64 = (arg & ~0xF00u) | (uint32_t(forced & 0xF) << 8);
  }
  __imp__sub_827E6CF8(ctx, base);
}

// The game's frame limiter (guest sub_821B8240, the main loop's "wait for the
// next frame"): r3 = frame clock object, r4 = vblanks to wait (0 = use the
// clock's target rate), r5 = timer slot. With r4 = 0 it waits until the vblank
// counter (bumped by the game's vblank callback sub_821FC010) reaches the
// slot's last frame + 60 / target_fps (u32 at object + 196: 30), then stores
// delta = elapsed vblanks / 60 for the game to step by. Time is counted in
// vblanks, so a target of 60 waits 1 vblank and steps 1/60 s per frame: 60 fps
// at the right speed. IU_TARGET_FPS=60 in ps5-env.txt sets it.
extern "C" void __imp__sub_821B8240(PPCContext& ctx, uint8_t* base);

extern "C" void sub_821B8240(PPCContext& ctx, uint8_t* base) {
  using namespace iu::frame_rate;
  if (ctx.r4.s32 == 0) {
    uint32_t* target = GuestWord(base, ctx.r3.u32 + 196);
    const uint32_t current = __builtin_bswap32(*target);
    const int forced = TargetFps();
    if (g_limiter_logs.fetch_add(1) < 4) {
      REXLOG_INFO("[frame-rate] limiter clock=0x{:08X} slot={} target_fps={} (forced {})",
                  ctx.r3.u32, ctx.r5.s32, current, forced);
    }
    if (forced > 0 && forced <= 60 && current != uint32_t(forced) && current != 0) {
      *target = __builtin_bswap32(uint32_t(forced));
    }
  }
  __imp__sub_821B8240(ctx, base);
}
