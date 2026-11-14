// infinite_undiscovery - ReXGlue Recompiled Project

#include "generated/default/infinite_undiscovery_init.h"

#include <rex/platform.h>

#if REX_PLATFORM_PS5
// The console app: no Asset Setup wizard, Win32 dialogs or D3D12 diagnostics
// (docs/PS5.md). It also carries the PS5 versions of fiber_lr.h's fiber
// switch wrapper and disc_swap.h's import hook.
#include "ps5/iu_ps5_app.h"
#else
#include "infinite_undiscovery_app.h"
#include "fiber_lr.h"
#include "vesplume_diag.h"
#include "iu_trace_points.h"
#endif

REXCVAR_DEFINE_BOOL(asset_setup, false, "Content",
                   "Show Asset Setup Wizard even when assets are configured")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(profile_manager, false, "Content",
                   "Open Content Profile Manager")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if !REX_PLATFORM_PS5
REXCVAR_DEFINE_BOOL(community_debug, true, "Debug", "Enable Infinite Undiscovery Community Debug overlay for recovery tools");

REXCVAR_DEFINE_BOOL(pso_prewarm, true, "GPU", "Wait for in-flight PSO creations at startup before resuming guest");
REXCVAR_DEFINE_BOOL(pso_telemetry, false, "Debug", "Log runtime PSO creation and frame miss telemetry");

#endif

REX_DEFINE_APP(infinite_undiscovery, InfiniteUndiscoveryApp::Create)
