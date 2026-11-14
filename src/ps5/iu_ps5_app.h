// infinite_undiscovery - PS5 (homebrew) app. See docs/PS5.md.
//
// The Windows app (infinite_undiscovery_app.h) is built around the Asset
// Setup wizard, Win32 dialogs and D3D12 diagnostics. None of those exist on
// the console. Here the title folder (/app0) is the portable root, laid out
// as the wizard lays out a content profile on Windows:
//
//   /app0/assets/disc1/   default.xex, ud1.bin, ud2.bin   (Disc 1)
//   /app0/assets/disc2/   default.xex, ud1.bin, ud2.bin   (Disc 2, optional)
//   /app0/saves/          user data (saves, profile)
//   /app0/shaders/        shader and pipeline caches
//   /app0/cache/          runtime.toml (settings), metadata
//   /app0/logs/           runtime.log
//
// Disc swapping (disc_swap.h) is the same code as on Windows: both discs are
// mounted up front and XamSwapDisc only selects which one the game's aliases
// point at. DLC packages are not installed on the PS5 yet.

#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_PS5

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/rex_app.h>
#include <rex/system/kernel_state.h>

#include "disc_swap.h"

namespace iu::ps5 {
// src/platform/ps5_title.cpp
void Breadcrumb(const char* line);
[[noreturn]] void ExitTitle(int status);

inline const std::filesystem::path kRoot{"/app0"};
}  // namespace iu::ps5

// The SDK's fiber switch, called by name from the recompiled code (the
// manifest's [entrypoint.rexcrt] SwitchToFiber). Windows wraps it in
// fiber_lr.h by looking the original up in rexruntime.dll; a PS5 title is one
// static executable, so ps5/link-title.sh links with
// --wrap=rexcrt_SwitchToFiber and the original is __real_rexcrt_SwitchToFiber.
//
// The suspended activation's saved LR belongs to this continuation (all 64
// bits), not to the shared PPCContext, so it is put back after the switch
// returns. (The Windows wrapper also polls the Save Anywhere and recovery
// move debug actions here; those are not in the PS5 build.)
extern "C" void __real_rexcrt_SwitchToFiber(PPCContext& ctx, uint8_t* base);
extern "C" void __wrap_rexcrt_SwitchToFiber(PPCContext& ctx, uint8_t* base) {
  const uint64_t continuation_lr = ctx.lr;
  __real_rexcrt_SwitchToFiber(ctx, base);
  ctx.lr = continuation_lr;
}

// Direct calls to XamSwapDisc from the recompiled code (Windows patches the
// import table instead; see disc_swap.h).
extern "C" void __wrap___imp__XamSwapDisc(PPCContext& ctx, uint8_t* base) {
  iu::disc_swap::Hook_XamSwapDisc(ctx, base);
}

class InfiniteUndiscoveryApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<InfiniteUndiscoveryApp>(
        new InfiniteUndiscoveryApp(ctx, "infinite_undiscovery", PPCImageConfig));
  }

 protected:
  bool SetupEnvironment() override {
    namespace fs = std::filesystem;
    const auto& root = iu::ps5::kRoot;
    const auto disc1 = root / "assets" / "disc1";
    std::error_code ec;
    if (!fs::is_regular_file(disc1 / "default.xex", ec)) {
      iu::ps5::Breadcrumb(
          "setup: no assets/disc1/default.xex in the title folder; copy Disc 1's "
          "default.xex, ud1.bin and ud2.bin to /app0/assets/disc1/ (docs/PS5.md)");
      return false;
    }
    for (const char* dir : {"saves", "shaders", "cache", "cache/metadata", "logs"})
      fs::create_directories(root / dir, ec);

    // Pin before the base SetupEnvironment, as the Windows app does: no
    // user-profile defaults or legacy config.
    rex::cvar::SetFlagByName("user_data_root", (root / "saves").string());
    rex::cvar::SetFlagByName("game_data_root", disc1.string());
    rex::cvar::SetFlagByName("cache_root", (root / "shaders").string());
    rex::cvar::SetFlagByName("log_file", (root / "logs" / "runtime.log").string());
    iu::ps5::Breadcrumb("setup: paths pinned under /app0");
    const bool ok = rex::ReXApp::SetupEnvironment();
    iu::ps5::Breadcrumb(ok ? "setup: environment ready" : "setup: SDK SetupEnvironment failed");
    return ok;
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto& root = iu::ps5::kRoot;
    paths.game_data_root = root / "assets" / "disc1";
    paths.user_data_root = root / "saves";
    paths.update_data_root = root / "assets" / "updates";
    paths.cache_root = root / "shaders";
    paths.metadata_root = root / "cache" / "metadata";
    paths.config_path = root / "cache" / "runtime.toml";
  }

  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)>) override {
    return defaults;
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The SDK's Xenos GPU, linked into the title (GPU_PLUGINS xenos); on the
    // PS5 its backend is Vulkan on the RADV linked in beside it.
    config.gpu_plugin = "xenos";
    iu::ps5::Breadcrumb("setup: gpu_plugin=xenos");
  }

  void OnPostSetup() override {
    const auto assets = iu::ps5::kRoot / "assets";
    REXLOG_INFO("CONTENT_PROFILE=ps5");
    REXLOG_INFO("DISC1_ROOT={}", (assets / "disc1").string());
    REXLOG_INFO("DISC2_ROOT={}", (assets / "disc2").string());
    // Base ConstructRuntime calls OnPostSetup before LaunchModule: mount
    // Disc 2 permanently and hook XamSwapDisc before the guest runs.
    if (!iu::disc_swap::Install(runtime(), iu::ps5::kRoot)) {
      iu::ps5::Breadcrumb("setup: disc mounts could not be prepared");
      throw std::runtime_error("IU permanent disc mounts could not be prepared");
    }
    iu::ps5::Breadcrumb("setup: disc mounts ready");
  }

  void OnPreLaunchModule() override { iu::ps5::Breadcrumb("launch: starting the guest"); }

  void OnClosing(rex::ui::UIEvent& e) override {
    (void)e;
    REXLOG_INFO("[Shutdown] closing, terminating title...");
    if (runtime() && runtime()->kernel_state()) runtime()->kernel_state()->TerminateTitle();
    rex::FlushLogging();
    // The desktop teardown crashes on the console: flush and leave.
    iu::ps5::ExitTitle(0);
  }
};

#endif  // REX_PLATFORM_PS5
