/**
 * @file    platform/ps5_title.cpp
 * @brief   PlayStation 5 homebrew title setup (docs/PS5.md), from Fable 2
 *          Recomp PS5's platform/ps5_title.cpp (after re:Blue PS5 and Lost Odyssey PS5).
 *
 * Everything here runs from the first static constructor, before the SDK's
 * main: standard error to klog, the IEEE floating-point state other x86-64
 * systems start in (a title starts with flush-to-zero and denormals-are-zero,
 * and the platform's pthread_create copies the creator's state to each
 * thread), the working directory set to the title folder, and the
 * ps5-env.txt switches (a title gets no environment). The SDK's Vulkan
 * backend reaches the RADV linked into the title itself
 * (vk_icdGetInstanceProcAddr), so nothing here sets Vulkan up.
 *
 * /app0/boot.txt records each step and any early crash (signal, rip, stack)
 * before the runtime's own log exists.
 *
 * @license BSD 3-Clause License
 */
#include <rex/platform.h>

#if REX_PLATFORM_PS5

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <typeinfo>

#include <fcntl.h>
#include <unistd.h>

#include <ps5platform/fp.h>
#include <ps5platform/klog.h>

extern "C" int sceSystemServiceHideSplashScreen(void);
extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *argv);
extern "C" int sceKernelUsleep(uint32_t microseconds);
extern "C" int sceKernelDebugOutText(int channel, const char *text);

namespace iu::ps5 {

// One line to /app0/boot.txt (opened afresh by the first line of a run,
// written through and closed each time, so it survives a crash) and to klog.
// Safe before main and before the runtime's log exists.
void Breadcrumb(const char *line) {
  static bool started = false;
  const int flags = O_WRONLY | O_CREAT | (started ? O_APPEND : O_TRUNC);
  started = true;
  const int fd = open("/app0/boot.txt", flags, 0666);
  if (fd >= 0) {
    (void)write(fd, line, std::strlen(line));
    (void)write(fd, "\n", 1);
    (void)close(fd);
  }
  char text[512];
  std::snprintf(text, sizeof(text), "[IU] %s\n", line);
  (void)sceKernelDebugOutText(0, text);
}

[[noreturn]] void ExitTitle(int status) {
  char line[96];
  std::snprintf(line, sizeof(line), "exit: status %d, asking the shell to close the title", status);
  Breadcrumb(line);
  std::fprintf(stderr, "exit status %d: asking the shell to close the title\n", status);
  std::fflush(nullptr);
  (void)sceSystemServiceLoadExec("exit", nullptr);
  for (;;)
    sceKernelUsleep(100000);
}

namespace {

// KEY=VALUE lines from /app0/ps5-env.txt become environment variables, so
// REX_* cvar overrides work on the console (ps5-env.example.txt).
void ReadEnvFile() {
  FILE *env = std::fopen("/app0/ps5-env.txt", "r");
  if (!env) {
    Breadcrumb("main: no ps5-env.txt");
    return;
  }
  char line[512];
  while (std::fgets(line, sizeof(line), env)) {
    line[std::strcspn(line, "\r\n")] = '\0';
    char *equals = std::strchr(line, '=');
    if (line[0] == '#' || !equals)
      continue;
    *equals = '\0';
    setenv(line, equals + 1, 1);
    char crumb[600];
    std::snprintf(crumb, sizeof(crumb), "main: ps5-env.txt %s=%s", line, equals + 1);
    Breadcrumb(crumb);
  }
  std::fclose(env);
}

void InitializeTitle() {
  Breadcrumb("title: started");
  (void)ps5_klog_capture_stderr("[IU] ");
  Breadcrumb("title: stderr to klog");
  ps5_fp_ieee();
  {
    // A title may not chdir on the console (EPERM); every path the title
    // opens is absolute under /app0, so only a different failure is news.
    const int rc = chdir("/app0");
    if (rc != 0 && errno != EPERM) {
      char line[96];
      std::snprintf(line, sizeof(line), "title: chdir /app0 failed, errno=%d", errno);
      Breadcrumb(line);
    }
  }
  (void)sceSystemServiceHideSplashScreen();
  Breadcrumb("title: splash hidden");
  // Infinite Undiscovery's title folder is its portable root: assets/disc1,
  // assets/disc2, saves, shaders, cache and logs all live in /app0
  // (src/ps5/iu_ps5_app.h). HOME points there too for anything that asks.
  setenv("HOME", "/app0", 0);
  ReadEnvFile();
  // The SDK's PS5-only hot-page uploads (shared_memory_hot_pages) leave stale
  // data in IU's title-screen HDR image: for a frame, part of the right half
  // shows the left half (runs 14-18, 2026-11-14). Off unless ps5-env.txt
  // sets REX_SHARED_MEMORY_HOT_PAGES itself.
  setenv("REX_SHARED_MEMORY_HOT_PAGES", "false", 0);
  // The debug overlay (L3 + R3) opens as a small frame-rate readout;
  // REX_DEBUG_OVERLAY_COMPACT=false in ps5-env.txt brings back the full panel.
  setenv("REX_DEBUG_OVERLAY_COMPACT", "true", 0);
}

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGSYS, SIGABRT, SIGTRAP};

// Before the runtime installs its own handlers (guest MMIO and SEH), and as
// the fallback after them: where the title died, in boot.txt.
void EarlyCrash(int signal, siginfo_t *info, void *context) {
  const auto *uc = static_cast<const ucontext_t *>(context);
  const uintptr_t anchor = reinterpret_cast<uintptr_t>(&Breadcrumb);
  char line[256];
  std::snprintf(line, sizeof(line),
                "CRASH signal=%d code=%d addr=%p rip=0x%lx rsp=0x%lx anchor(Breadcrumb)=0x%lx", signal,
                info ? info->si_code : 0, info ? info->si_addr : nullptr,
                (unsigned long)uc->uc_mcontext.mc_rip, (unsigned long)uc->uc_mcontext.mc_rsp,
                (unsigned long)anchor);
  Breadcrumb(line);
  const auto *stack = reinterpret_cast<const uintptr_t *>(uc->uc_mcontext.mc_rsp);
  int found = 0;
  for (int i = 0; i < 512 && found < 24; ++i) {
    const uintptr_t value = stack[i];
    if (value >= 0x400000 && value < 0x10400000) {
      std::snprintf(line, sizeof(line), "  stack[%d]=0x%lx", i, (unsigned long)value);
      Breadcrumb(line);
      ++found;
    }
  }
  struct sigaction fallback{};
  fallback.sa_handler = SIG_DFL;
  sigaction(signal, &fallback, nullptr);
}

// An uncaught exception ends in abort(); name it first.
[[noreturn]] void Terminate() {
  char line[400];
  const char *what = "no exception";
  const char *type = "";
  if (auto current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception &e) {
      what = e.what();
      type = typeid(e).name();
    } catch (...) {
      what = "non-std exception";
    }
  }
  std::snprintf(line, sizeof(line), "TERMINATE: uncaught %s: %s", type, what);
  Breadcrumb(line);
  std::abort();
}

} // namespace
} // namespace iu::ps5

__attribute__((constructor(101))) static void IuFirstConstructor() {
  iu::ps5::Breadcrumb("title loaded: static constructors running");
  std::set_terminate(iu::ps5::Terminate);
  struct sigaction action{};
  action.sa_sigaction = iu::ps5::EarlyCrash;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  for (int signal : iu::ps5::kFatalSignals)
    sigaction(signal, &action, nullptr);
  iu::ps5::InitializeTitle();
}

// The CRT (PS5_Vulkan's app_crt.cpp) calls this after main returns.
extern "C" void catchReturnFromMain(int status) {
  iu::ps5::ExitTitle(status);
}

// ps5/link-title.sh links with --wrap=exit --wrap=_exit --wrap=_Exit, so a quit
// from deep inside the runtime (or SDL, or a library) takes the same way out.
extern "C" [[noreturn]] void __wrap_exit(int status) {
  iu::ps5::ExitTitle(status);
}
extern "C" [[noreturn]] void __wrap__exit(int status) {
  iu::ps5::ExitTitle(status);
}
extern "C" [[noreturn]] void __wrap__Exit(int status) {
  iu::ps5::ExitTitle(status);
}

#endif // REX_PLATFORM_PS5
