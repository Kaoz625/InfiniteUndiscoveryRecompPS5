#pragma once

#ifdef _WIN32
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/kernel/xboxkrnl/threading.h>

#ifdef _WIN32
#include "asset_setup.h"
#include "iu_trace.h"
#include "vblank_diag.h"
#else
// PS5 (docs/PS5.md): no Asset Setup, iu_trace or vblank diagnostics, which are
// built on Win32. Swaps log through the SDK's logger, and a disc folder is
// checked for its files rather than hashed (the player installs them).
#include <rex/logging.h>
#endif

namespace iu::disc_swap {

inline constexpr std::array<const char*, 3> kAliases{
    "game:", "d:", "\\Device\\Cdrom0"};
struct Mount {
  const char* device_path;
  std::filesystem::path host_path;
  rex::filesystem::Device* device{nullptr}; // VFS owns it until shutdown.
  rex::filesystem::Entry* root{nullptr};
  bool initialized{false};
  bool available{false};
};
struct State {
  std::mutex mutex;
  std::filesystem::path assets_root;
  std::filesystem::path disc1_path;
  std::filesystem::path active_disc_root;
  std::array<Mount, 2> mounts{{{"\\Device\\Harddisk0\\Partition1"},
                              {"\\Device\\IUDisc2"}}};
  uint32_t current_disc{1};
  bool installed{false};
};

inline State g_state;

inline uint32_t GetCurrentDisc() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  return g_state.current_disc;
}

inline void LogSwap(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

#ifdef _WIN32
  // Emit to iu_trace
  iu::trace::emit(iu::trace::kGeneral, "%s", buf);

  // Log to diagnostic.log
  vb::log("%s", buf);
#else
  REXLOG_INFO("[DISC_SWAP] {}", buf);
#endif

  // Print to console
  std::printf("[DISC_SWAP] %s\n", buf);
  std::fflush(stdout);
}

#ifdef _WIN32
inline bool PatchIAT(const char* target_dll, const char* target_func, void* new_func_ptr) {
  HMODULE hModule = GetModuleHandleW(nullptr);
  if (!hModule) return false;

  auto dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(hModule);
  if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) return false;

  auto nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(
      reinterpret_cast<uint8_t*>(hModule) + dos_header->e_lfanew);
  if (nt_headers->Signature != IMAGE_NT_SIGNATURE) return false;

  auto import_dir = nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (import_dir.VirtualAddress == 0 || import_dir.Size == 0) return false;

  auto import_desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
      reinterpret_cast<uint8_t*>(hModule) + import_dir.VirtualAddress);

  bool patched = false;
  for (; import_desc->Name != 0; ++import_desc) {
    const char* mod_name = reinterpret_cast<const char*>(
        reinterpret_cast<uint8_t*>(hModule) + import_desc->Name);
    if (_stricmp(mod_name, target_dll) != 0) continue;

    auto thunk_orig = reinterpret_cast<PIMAGE_THUNK_DATA>(
        reinterpret_cast<uint8_t*>(hModule) +
        (import_desc->OriginalFirstThunk ? import_desc->OriginalFirstThunk : import_desc->FirstThunk));
    auto thunk_iat = reinterpret_cast<PIMAGE_THUNK_DATA>(
        reinterpret_cast<uint8_t*>(hModule) + import_desc->FirstThunk);

    for (; thunk_orig->u1.AddressOfData != 0; ++thunk_orig, ++thunk_iat) {
      if (IMAGE_SNAP_BY_ORDINAL(thunk_orig->u1.Ordinal)) continue;

      auto import_by_name = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
          reinterpret_cast<uint8_t*>(hModule) + thunk_orig->u1.AddressOfData);
      if (strcmp(import_by_name->Name, target_func) == 0 ||
          _stricmp(import_by_name->Name, target_func) == 0) {
        DWORD old_protect = 0;
        if (VirtualProtect(&thunk_iat->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) {
          thunk_iat->u1.Function = reinterpret_cast<uintptr_t>(new_func_ptr);
          VirtualProtect(&thunk_iat->u1.Function, sizeof(void*), old_protect, &old_protect);
          patched = true;
        }
      }
    }
  }
  return patched;
}

#endif  // _WIN32

// ctx.r4 is the guest object pointer (from ObReferenceObjectByHandle in
// sub_82958C78) of the KEVENT the guest waits on via NtWaitForSingleObjectEx.
inline void SignalCompletion(uint32_t event_obj) {
  LogSwap("DISC_SWAP_EVENT object/handle=0x%08X", event_obj);
  if (!event_obj) return;
  auto* ks = rex::system::kernel_state();
  auto* kev = ks->memory()->TranslateVirtual<rex::system::X_KEVENT*>(event_obj);
  rex::kernel::xboxkrnl::xeKeSetEvent(kev, 1, 0);
  LogSwap("DISC_SWAP_EVENT_SIGNALED");
}

// Caller holds the global region. Snapshot exact alias presence for rollback,
// including Cdrom0, which the stock runtime does not initially register.
inline bool SelectAliases(rex::filesystem::VirtualFileSystem* vfs,
                          const Mount& mount) {
  struct Previous { bool present; std::string target; };
  std::array<Previous, 3> previous{};
  for (size_t i = 0; i < kAliases.size(); ++i)
    previous[i].present = vfs->FindSymbolicLink(kAliases[i], previous[i].target);
  try {
    for (auto alias : kAliases) {
      std::string target;
      if (vfs->FindSymbolicLink(alias, target) && target == mount.device_path) continue;
      vfs->UnregisterSymbolicLink(alias);
      if (!vfs->RegisterSymbolicLink(alias, mount.device_path))
        throw std::runtime_error("alias registration failed");
    }
    for (auto alias : kAliases) {
      std::string target;
      if (!vfs->FindSymbolicLink(alias, target) || target != mount.device_path ||
          vfs->ResolvePath(alias) != mount.root)
        throw std::runtime_error("alias target/root verification failed");
    }
    return true;
  } catch (const std::exception& e) {
    LogSwap("DISC_ALIAS_FAILED reason=\"%s\"", e.what());
    bool restored = true;
    for (size_t i = 0; i < kAliases.size(); ++i) {
      try {
        std::string target;
        const bool present = vfs->FindSymbolicLink(kAliases[i], target);
        if (present == previous[i].present &&
            (!present || target == previous[i].target)) continue;
        vfs->UnregisterSymbolicLink(kAliases[i]);
        if (previous[i].present)
          restored &= vfs->RegisterSymbolicLink(kAliases[i], previous[i].target);
        target.clear();
        const bool after = vfs->FindSymbolicLink(kAliases[i], target);
        restored &= after == previous[i].present &&
                    (!after || target == previous[i].target);
      } catch (...) { restored = false; }
    }
    LogSwap("DISC_ALIAS_ROLLBACK restored=%d", restored);
    return false;
  }
}

// Disc identity checks. Windows inspects and hashes the installed media
// (asset_setup.cpp); the PS5 checks that each disc folder holds its files.
#ifdef _WIN32
inline void ValidateDisc2(const std::filesystem::path& disc1,
                          const std::filesystem::path& disc2) {
  iu::assets::ValidatePair(iu::assets::Inspect(disc1), iu::assets::Inspect(disc2));
}
inline void ValidateDisc1(const std::filesystem::path& disc1) {
  auto disc = iu::assets::Inspect(disc1);
  if (disc.number != 1) throw std::runtime_error("target is not Disc 1");
  iu::assets::ValidatePair(disc, std::nullopt);
}
#else
inline void RequireDiscFiles(const std::filesystem::path& root) {
  for (const char* name : {"default.xex", "ud1.bin", "ud2.bin"}) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(root / name, ec))
      throw std::runtime_error(std::string("missing ") + (root / name).string());
  }
}
inline void ValidateDisc2(const std::filesystem::path& disc1,
                          const std::filesystem::path& disc2) {
  RequireDiscFiles(disc1);
  RequireDiscFiles(disc2);
}
inline void ValidateDisc1(const std::filesystem::path& disc1) { RequireDiscFiles(disc1); }
#endif

// Runs from OnPostSetup before LaunchModule. No mount is created by a swap.
inline bool PrepareMounts(rex::Runtime* runtime,
                          const std::filesystem::path& portable_root) {
  auto* vfs = runtime ? runtime->kernel_state()->file_system() : nullptr;
  if (!vfs) return false;
  g_state.assets_root = portable_root / "assets";
  g_state.disc1_path = g_state.assets_root / "disc1";
  g_state.current_disc = 1;
  g_state.active_disc_root = g_state.disc1_path;
  auto& first = g_state.mounts[0];
  first.host_path = g_state.disc1_path;
  first.root = vfs->ResolvePath(first.device_path);
  auto* first_host = dynamic_cast<rex::filesystem::HostPathEntry*>(first.root);
  if (!first_host || !first_host->device()->is_read_only() ||
      !std::filesystem::equivalent(first_host->host_path(), first.host_path)) {
    LogSwap("DISC_MOUNT_FAILED disc=1 reason=\"existing read-only Disc 1 mount unavailable\"");
    return false;
  }
  first.device = first.root->device();
  first.initialized = first.available = true;
  {
    auto global_lock = rex::thread::global_critical_region::AcquireDirect();
    if (!SelectAliases(vfs, first)) return false;
  }
  LogSwap("DISC_MOUNT_RETAIN disc=1 device=%s host=%s device_ptr=%p root_entry=%p",
          first.device_path, first.host_path.string().c_str(),
          static_cast<void*>(first.device), static_cast<void*>(first.root));
  auto& second = g_state.mounts[1];
  second.host_path = g_state.assets_root / "disc2";
  try {
    ValidateDisc2(first.host_path, second.host_path);
    auto device = std::make_unique<rex::filesystem::HostPathDevice>(
        second.device_path, second.host_path, true, false);
    if (!device->Initialize()) throw std::runtime_error("Disc 2 initialization failed");
    second.initialized = true;
    auto* device_ptr = device.get();
    auto* root = device->ResolvePath("");
    if (!root || !vfs->RegisterDevice(std::move(device)))
      throw std::runtime_error("Disc 2 registration failed");
    second.device = device_ptr;
    second.root = root;
    second.available = true;
    LogSwap("DISC_MOUNT_REGISTER disc=2 device=%s host=%s device_ptr=%p root_entry=%p",
            second.device_path, second.host_path.string().c_str(),
            static_cast<void*>(second.device), static_cast<void*>(second.root));
  } catch (const std::exception& e) {
    LogSwap("DISC_MOUNT_UNAVAILABLE disc=2 host=%s reason=\"%s\"",
            second.host_path.string().c_str(), e.what());
  }
  return true; // Missing/invalid Disc 2 never prevents Disc 1 startup.
}

// Hook replacing XamSwapDisc; the global region always precedes State::mutex.
inline void Hook_XamSwapDisc(PPCContext& ctx, uint8_t* base) {
  const uint32_t requested = ctx.r3.u32;
  const uint32_t event_obj = ctx.r4.u32;
  ctx.r3.u64 = 0xC0000001; // Failure unless selection is committed.
  if (requested < 1 || requested > 2) {
    LogSwap("DISC_SWAP_FAILED reason=\"unsupported disc number %u\"", requested);
    return;
  }

  std::filesystem::path target_path, disc1_path;
  {
    std::lock_guard lock(g_state.mutex);
    LogSwap("DISC_SWAP_REQUEST requested=%u current=%u", requested, g_state.current_disc);
    if (!g_state.mounts[requested - 1].available) {
      LogSwap("DISC_SWAP_FAILED reason=\"permanent mount unavailable\"");
      return;
    }
    target_path = g_state.mounts[requested - 1].host_path;
    disc1_path = g_state.disc1_path;
  }

  LogSwap("DISC_SWAP_VALIDATE target_disc=%u target_path=%s", requested, target_path.string().c_str());

  // Strict validation: Inspect target disc and validate pair against Disc 1
  try {
    if (!std::filesystem::exists(target_path)) {
      throw std::runtime_error("target disc folder does not exist");
    }

    if (requested == 2) {
      ValidateDisc2(disc1_path, target_path);
    } else if (requested == 1) {
      ValidateDisc1(target_path);
    }
  } catch (const std::exception& e) {
    LogSwap("DISC_SWAP_FAILED reason=\"%s\"", e.what());
    ctx.r3.u64 = 0xC0000001; // X_STATUS_UNSUCCESSFUL
    return;
  }

  // Select only names. Both devices and their cached trees remain untouched.
  auto vfs = rex::system::kernel_state()->file_system();
  if (!vfs) {
    LogSwap("DISC_SWAP_FAILED reason=\"VirtualFileSystem unavailable\"");
    ctx.r3.u64 = 0xC0000001;
    return;
  }

  const char* selected_mount = nullptr;
  {
    auto global_lock = rex::thread::global_critical_region::AcquireDirect();
    std::lock_guard lock(g_state.mutex);
    auto& mount = g_state.mounts[requested - 1];
    LogSwap("DISC_SELECT_BEGIN from=%u to=%u", g_state.current_disc, requested);
    if (!mount.available || vfs->ResolvePath(mount.device_path) != mount.root ||
        mount.root->device() != mount.device || !SelectAliases(vfs, mount)) {
      LogSwap("DISC_SWAP_FAILED reason=\"permanent mount/alias verification failed\"");
      return;
    }
    // Path swap is noexcept; publish only after all three aliases verify.
    g_state.active_disc_root.swap(target_path);
    g_state.current_disc = requested;
    selected_mount = mount.device_path;
    LogSwap("DISC_SELECT_SUCCESS current=%u active_root=%s device_ptr=%p root_entry=%p",
            requested, g_state.active_disc_root.string().c_str(),
            static_cast<void*>(mount.device), static_cast<void*>(mount.root));
  }
  for (auto alias : kAliases)
    LogSwap("DISC_ALIAS_SELECT alias=%s target=%s", alias, selected_mount);
  LogSwap("DISC_SWAP_SUCCESS current=%u", requested);
  SignalCompletion(event_obj);
  LogSwap("DISC_SWAP_RETURN status=0");
  ctx.r3.u64 = 0; // X_STATUS_SUCCESS
}

// Self-test of the swap machinery without playing to the end of Disc 1 (set
// IU_DISC_SWAP_SELFTEST=1, e.g. in the PS5 ps5-env.txt). Runs before the guest
// starts: swaps to Disc 2 through the real hook, checks that game:, d: and
// Cdrom0 now resolve into the Disc 2 folder, swaps back to Disc 1 and checks
// again. Logs DISC_SELFTEST lines; the game then starts on Disc 1 as usual.
// It doesn't exercise the game's own swap prompt or its Disc 2 loading.
inline bool SelfTest() {
  auto* vfs = rex::system::kernel_state()->file_system();
  if (!vfs) {
    LogSwap("DISC_SELFTEST_FAILED reason=\"no file system\"");
    return false;
  }
  auto check = [&](uint32_t disc) {
    bool ok = true;
    std::filesystem::path expected;
    {
      std::lock_guard lock(g_state.mutex);
      expected = g_state.mounts[disc - 1].host_path;
    }
    for (auto alias : kAliases) {
      for (const char* name : {"default.xex", "ud1.bin", "ud2.bin"}) {
        std::string path = std::string(alias) + "\\" + name;
        auto* entry = dynamic_cast<rex::filesystem::HostPathEntry*>(vfs->ResolvePath(path));
        std::error_code ec;
        const bool here = entry && std::filesystem::equivalent(entry->host_path(), expected / name, ec);
        LogSwap("DISC_SELFTEST disc=%u %s -> %s size=%llu %s", disc, path.c_str(),
                entry ? entry->host_path().string().c_str() : "(not found)",
                entry ? (unsigned long long)entry->size() : 0ull, here ? "OK" : "WRONG");
        ok = ok && here;
      }
    }
    return ok;
  };
  PPCContext ctx{};
  bool ok = check(1);
  ctx.r3.u64 = 2;
  ctx.r4.u64 = 0;  // no completion event to signal
  Hook_XamSwapDisc(ctx, nullptr);
  ok = ok && ctx.r3.u32 == 0 && GetCurrentDisc() == 2 && check(2);
  ctx = PPCContext{};
  ctx.r3.u64 = 1;
  Hook_XamSwapDisc(ctx, nullptr);
  ok = ok && ctx.r3.u32 == 0 && GetCurrentDisc() == 1 && check(1);
  LogSwap(ok ? "DISC_SELFTEST_PASSED" : "DISC_SELFTEST_FAILED");
  return ok;
}

inline bool Install(rex::Runtime* runtime, const std::filesystem::path& portable_root) {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (g_state.installed) return true;
  if (!PrepareMounts(runtime, portable_root)) return false;

  // 1. FunctionDispatcher hook (guest address 0x829687EC)
  bool fd_hooked = false;
  if (runtime && runtime->function_dispatcher()) {
    fd_hooked = runtime->function_dispatcher()->SetFunction(0x829687EC, Hook_XamSwapDisc);
  }

  // 2. Direct calls from the recompiled code. Windows patches the import in
  // the main module; a PS5 title is linked with --wrap=__imp__XamSwapDisc
  // (ps5/link-title.sh), and __wrap___imp__XamSwapDisc (src/ps5/iu_ps5_app.h)
  // forwards here, so those calls are always hooked.
#ifdef _WIN32
  bool iat_hooked_1 = PatchIAT("rexruntime.dll", "__imp__XamSwapDisc", reinterpret_cast<void*>(&Hook_XamSwapDisc));
  bool iat_hooked_2 = PatchIAT("rexruntime.dll", "XamSwapDisc", reinterpret_cast<void*>(&Hook_XamSwapDisc));
#else
  const bool iat_hooked_1 = true, iat_hooked_2 = false;
#endif

  g_state.installed = true;

  LogSwap("DISC_SWAP_INSTALLED assets_root=%s fd_hooked=%d iat_hooked=%d",
          g_state.assets_root.string().c_str(),
          fd_hooked ? 1 : 0,
          (iat_hooked_1 || iat_hooked_2) ? 1 : 0);
  return true;
}

}  // namespace iu::disc_swap
