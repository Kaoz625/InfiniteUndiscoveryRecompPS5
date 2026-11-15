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
