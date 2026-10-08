/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
** SDL3Main.cpp
**
** Entry point for Linux builds using SDL3 windowing and DXVK graphics.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Entry point replaces WinMain() for Linux builds.
** Instantiates SDL3GameEngine and calls GameMain().
*/

#ifndef _WIN32

// SYSTEM INCLUDES
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
// GeneralsX @build FadiLabib 06/07/2026 On iOS and Android, SDL owns the app
// lifecycle: SDL_main.h renames main() to SDL_main, which SDL's bootstrap
// (UIApplicationMain / SDLActivity JNI) invokes.
#include <SDL3/SDL_main.h>
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#elif defined(__ANDROID__)
// GeneralsX @build FadiLabib 06/07/2026 Android bootstrap needs cerrno for strerror(errno) in chdir() diagnostics.
#include <cerrno>
// GeneralsX @feature FadiLabib 06/07/2026 stderr/stdout -> logcat pump (see main()).
#include <android/log.h>
#include <pthread.h>
#include <cstdint>
// GeneralsX @feature FadiLabib 07/07/2026 Turnip-via-adrenotools driver staging.
// dladdr() locates the APK nativeLibraryDir; sys/stat + fcntl copy the driver.
#include <dlfcn.h>
#include <sys/stat.h>
#include <fcntl.h>
// GeneralsX @feature S25 08/10/2026 On-device session log + crash report.
#include <signal.h>
#include <ucontext.h>
#include <unwind.h>
#include <sys/prctl.h>
#include <sys/system_properties.h>
#endif
#endif
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <unistd.h>   // _exit()
#include <glob.h>     // glob() for Vulkan ICD discovery

// USER INCLUDES (match WinMain.cpp pattern)
#include "Lib/BaseType.h"
#include "Common/CommandLine.h"
#include "Common/CriticalSection.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "Common/GameMemory.h"
#include "Common/Debug.h"
#include "Common/version.h"  // GeneralsX @bugfix BenderAI 14/02/2026 Version class + TheVersion extern
#include "SDL3GameEngine.h"

// DXVK WSI
#define DXVK_WSI_SDL3 1
#include <wsi/native_wsi.h>

// CRITICAL SECTIONS (Linux needs these too)
static CriticalSection critSec1;
static CriticalSection critSec2;
static CriticalSection critSec3;
static CriticalSection critSec4;
static CriticalSection critSec5;

// GLOBAL COMMAND LINE ARGUMENTS
// TheSuperHackers @build felipebraz 13/02/2026
// Store argc/argv from main() for use by CommandLine.cpp parseCommandLine() on Linux
// Windows provides these automatically; Linux needs explicit globals
int __argc = 0;          ///< global argument count
char** __argv = nullptr; ///< global argument vector

// GLOBAL WINDOW HANDLE
// TheSuperHackers @build felipebraz 13/02/2026
// ApplicationHWnd is declared extern in GeneralsMD/Code/Main/WinMain.h
// On Linux, we cast SDL_Window* to HWND type for compatibility
HWND ApplicationHWnd = nullptr;  ///< our application window handle

// GLOBAL SDL3 WINDOW
// GeneralsX @feature felipebraz 16/02/2026
// SDL3 window created in main() before GameMain(), stored globally for engine access
SDL_Window* TheSDL3Window = nullptr;

// GAME TEXT FILE PATHS
// TheSuperHackers @build felipebraz 13/02/2026
// GameText.cpp uses these paths to load CSF and STR files (game localization)
// Format %s is replaced with language code in GameTextManager::init()
// GeneralsX @bugfix BenderAI 13/02/2026 - Fix case-sensitivity on Linux (generals.csf vs Generals.csf)
const Char *g_csfFile = "data/%s/generals.csf";  ///< CSF file path (lowercase for Linux compatibility)
const Char *g_strFile = "data/Generals.str";     ///< STR file path

// Extern declarations (from GameMain.cpp)
extern Int GameMain();

#if defined(__ANDROID__)
// GeneralsX @feature FadiLabib 06/07/2026
// On Android an app's stdout/stderr are discarded by the runtime — SDL does NOT
// route them to logcat. The engine's completion marker ("GameMain() returned
// with code N", stderr) and the headless replay progress ("Simulating Replay",
// "Elapsed/Game Time", "Simulation of all replays completed", stdout) would all
// vanish, leaving on-device runs (headless replay harness now, renderer
// bring-up later) blind. This pump reads the write end of a pipe that stdout and
// stderr are dup2'd onto and forwards each line to __android_log_write under a
// stable tag, so `adb logcat -s GeneralsX` shows the full engine output.
// GeneralsX @feature FadiLabib 07/07/2026 Filter boot-time trace spam from logcat (GENERALSX_VERBOSE disables).
// The [INI]/[SUBSYS]/[GX-ISSUE144] fprintf traces flood logcat for ~40 s per boot.
// Returns true when a line is this high-volume boot trace and should be dropped.
// Error-ish lines (err:/warn:/ERROR/FATAL, including DXVK's own) are ALWAYS kept so
// real diagnostics can never be swallowed by the filter. Cheap, allocation-free:
// skip leading whitespace, then a couple of prefix compares on the per-line path.
static bool gxIsBootTraceSpam(const char *line)
{
	while (*line == ' ' || *line == '\t') ++line;
	// Safety first: never drop error/warn lines even if a prefix somehow matches.
	if (strncmp(line, "err:", 4) == 0 ||
	    strncmp(line, "warn:", 5) == 0 ||
	    strncmp(line, "ERROR", 5) == 0 ||
	    strncmp(line, "FATAL", 5) == 0) {
		return false;
	}
	return strncmp(line, "[INI]", 5) == 0
	    || strncmp(line, "[SUBSYS]", 8) == 0
	    || strncmp(line, "[GX-ISSUE144]", 13) == 0;
}

// GeneralsX @feature S25 08/10/2026 On-device session log + crash report.
// Reading logcat needs adb and a computer. So the pump below also mirrors every
// line it forwards into /sdcard/GeneralsZH/generals-log.txt (the previous session
// is kept as generals-log-prev.txt), and gxInstallCrashHandler() appends the
// signal, fault address, thread and a backtrace when the process dies. A player
// can then send the file straight from the phone. Per-frame spam is dropped and
// the file is capped at 8 MB (error lines and the crash report still land past
// the cap, since the tail of a dying session is the point of the file).
static const char *GX_LOG_PATH = "/sdcard/GeneralsZH/generals-log.txt";
static const char *GX_LOG_PREV_PATH = "/sdcard/GeneralsZH/generals-log-prev.txt";
static const size_t GX_LOG_CAP = 8u * 1024u * 1024u;
static int s_gxLogFd = -1;
static int s_gxPumpReadFd = -1;
static size_t s_gxLogWritten = 0;

static bool gxIsErrorLine(const char *line)
{
	while (*line == ' ' || *line == '\t') ++line;
	return strncmp(line, "err:", 4) == 0
	    || strncmp(line, "ERROR", 5) == 0
	    || strncmp(line, "FATAL", 5) == 0;
}

// Append raw bytes to the session log. important=true bypasses the size cap.
static void gxLogFileWrite(const char *buf, size_t len, bool important)
{
	if (s_gxLogFd < 0 || len == 0) {
		return;
	}
	if (s_gxLogWritten >= GX_LOG_CAP && !important) {
		static bool s_capMarked = false;
		if (!s_capMarked) {
			s_capMarked = true;
			static const char mark[] = "[log capped: only errors are recorded from here]\n";
			write(s_gxLogFd, mark, sizeof(mark) - 1);
		}
		return;
	}
	const ssize_t w = write(s_gxLogFd, buf, len);
	if (w > 0) {
		s_gxLogWritten += (size_t)w;
	}
}

static void gxLogFileWriteLine(const char *line)
{
	const char *p = line;
	while (*p == ' ' || *p == '\t') ++p;
	// Same per-frame spam the iOS session log drops (DXVK's D3D8 per-call warns).
	if (strncmp(p, "warn:  D3D8De", 13) == 0) {
		return;
	}
	// One write per line, newline included, so a crash report written from
	// another thread can't land between a line and its newline.
	char buf[1100];
	size_t len = strlen(line);
	if (len > sizeof(buf) - 2) {
		len = sizeof(buf) - 2;
	}
	memcpy(buf, line, len);
	buf[len++] = '\n';
	gxLogFileWrite(buf, len, gxIsErrorLine(line));
}

static void gxLogFileOpen()
{
	rename(GX_LOG_PATH, GX_LOG_PREV_PATH);
	s_gxLogFd = open(GX_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
	if (s_gxLogFd < 0) {
		return;  // no storage permission yet / folder missing: logcat only
	}
	char model[PROP_VALUE_MAX] = "?";
	char release[PROP_VALUE_MAX] = "?";
	char sdk[PROP_VALUE_MAX] = "?";
	__system_property_get("ro.product.model", model);
	__system_property_get("ro.build.version.release", release);
	__system_property_get("ro.build.version.sdk", sdk);
	char header[256];
	const int n = snprintf(header, sizeof(header),
	                       "Generals ZH session log - %s, Android %s (API %s)\n", model, release, sdk);
	if (n > 0) {
		gxLogFileWrite(header, (size_t)n, true);
	}
}

static void *gxLogcatPump(void *arg)
{
	const int readFd = (int)(intptr_t)arg;
	char line[1024];
	size_t len = 0;
	char chunk[512];
	ssize_t n;
	// GeneralsX @feature FadiLabib 07/07/2026 Read the verbose gate ONCE. Unset
	// (default) = filter boot-trace spam; set = full firehose for debugging.
	const bool verbose = (getenv("GENERALSX_VERBOSE") != nullptr);
	while ((n = read(readFd, chunk, sizeof(chunk))) > 0) {
		for (ssize_t i = 0; i < n; ++i) {
			const char c = chunk[i];
			if (c == '\n' || len == sizeof(line) - 1) {
				line[len] = '\0';
				if (verbose || !gxIsBootTraceSpam(line)) {
					__android_log_write(ANDROID_LOG_INFO, "GeneralsX", line);
					gxLogFileWriteLine(line);
				}
				len = 0;
				if (c != '\n') {
					line[len++] = c;  // carry the char that overflowed the buffer
				}
			} else {
				line[len++] = c;
			}
		}
	}
	if (len > 0) {
		line[len] = '\0';
		if (verbose || !gxIsBootTraceSpam(line)) {
			__android_log_write(ANDROID_LOG_INFO, "GeneralsX", line);
			gxLogFileWriteLine(line);
		}
	}
	return nullptr;
}

// GeneralsX @feature FadiLabib 06/07/2026 Redirect stdout+stderr to logcat.
// Must run before any fprintf/printf in main() so the bootstrap diagnostics are
// captured too. Idempotent-safe: called once from main().
static void gxRedirectStdioToLogcat()
{
	gxLogFileOpen();

	int pipeFd[2];
	if (pipe(pipeFd) != 0) {
		return;  // best-effort; leave stdio as-is if the pipe can't be made
	}

	// GeneralsX @bugfix FadiLabib 06/07/2026 Fail-safe redirect.
	// If the pump thread never starts (or a dup2 fails), nothing drains the read
	// end while stdout/stderr point at the write end — the 64KB pipe fills and the
	// FIRST printf/fprintf past that blocks forever, hanging the whole engine.
	// Save the real stdout/stderr first so we can undo the redirect and fall back
	// to (Android-dropped, but non-blocking) default stdio on any failure.
	const int savedStdout = dup(STDOUT_FILENO);
	const int savedStderr = dup(STDERR_FILENO);

	// stdout line-buffered, stderr unbuffered so a crash still flushes its tail.
	setvbuf(stdout, nullptr, _IOLBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);

	bool ok = (dup2(pipeFd[1], STDOUT_FILENO) != -1)
	       && (dup2(pipeFd[1], STDERR_FILENO) != -1);

	pthread_t pumpThread;
	if (ok && pthread_create(&pumpThread, nullptr, gxLogcatPump, (void *)(intptr_t)pipeFd[0]) == 0) {
		pthread_detach(pumpThread);
		s_gxPumpReadFd = pipeFd[0];  // the crash handler drains what the pump hasn't read yet
		close(pipeFd[1]);            // pump owns the read end; writer fd stays live via dup2'd 1/2
		if (savedStdout != -1) close(savedStdout);
		if (savedStderr != -1) close(savedStderr);
	} else {
		// No drainer: restore the original fds so writes never block on a full pipe,
		// then close both pipe ends. Logging silently reverts to default; engine runs.
		if (savedStdout != -1) { dup2(savedStdout, STDOUT_FILENO); close(savedStdout); }
		if (savedStderr != -1) { dup2(savedStderr, STDERR_FILENO); close(savedStderr); }
		close(pipeFd[0]);
		close(pipeFd[1]);
	}
}

// GeneralsX @feature S25 08/10/2026 Crash report into the session log.
// On a fatal signal, append to generals-log.txt: the log lines still sitting in
// the pipe, the signal and fault address, the crashing thread, its registers'
// pc/lr, a frame-pointer walk from the faulting context and an unwinder
// backtrace (library + offset + symbol, symbolizable against the APK's libs).
// Then restore the previous handler (Android's debuggerd, which still writes
// its tombstone) and let the signal reach it. Not strictly async-signal-safe
// (snprintf/dladdr/unwinder), which is acceptable for a last-gasp report.
static const int s_gxCrashSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
static const int GX_CRASH_SIGNAL_COUNT = (int)(sizeof(s_gxCrashSignals) / sizeof(s_gxCrashSignals[0]));
static struct sigaction s_gxOldSigActions[sizeof(s_gxCrashSignals) / sizeof(s_gxCrashSignals[0])];
static volatile sig_atomic_t s_gxInCrash = 0;

static void gxCrashWrite(const char *s)
{
	gxLogFileWrite(s, strlen(s), true);
}

static void gxCrashWriteFrame(int idx, const char *tag, uintptr_t pc)
{
	char out[512];
	Dl_info info;
	if (pc != 0 && dladdr(reinterpret_cast<void *>(pc), &info) != 0 && info.dli_fname != nullptr) {
		const char *base = strrchr(info.dli_fname, '/');
		base = base ? base + 1 : info.dli_fname;
		const unsigned long off = (unsigned long)(pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
		if (info.dli_sname != nullptr) {
			snprintf(out, sizeof(out), "  #%02d %s %08lx  %s (%s+%lu)\n", idx, tag, off, base,
			         info.dli_sname, (unsigned long)(pc - reinterpret_cast<uintptr_t>(info.dli_saddr)));
		} else {
			snprintf(out, sizeof(out), "  #%02d %s %08lx  %s\n", idx, tag, off, base);
		}
	} else {
		snprintf(out, sizeof(out), "  #%02d %s %016lx  ???\n", idx, tag, (unsigned long)pc);
	}
	gxCrashWrite(out);
}

struct GxUnwindState {
	int depth;
};

static _Unwind_Reason_Code gxUnwindFrame(struct _Unwind_Context *ctx, void *arg)
{
	GxUnwindState *st = static_cast<GxUnwindState *>(arg);
	const uintptr_t pc = _Unwind_GetIP(ctx);
	if (pc == 0) {
		return _URC_END_OF_STACK;
	}
	gxCrashWriteFrame(st->depth++, "pc", pc);
	return st->depth >= 48 ? _URC_END_OF_STACK : _URC_NO_REASON;
}

static const char *gxSignalName(int sig)
{
	switch (sig) {
	case SIGSEGV: return "SIGSEGV";
	case SIGBUS:  return "SIGBUS";
	case SIGILL:  return "SIGILL";
	case SIGFPE:  return "SIGFPE";
	case SIGABRT: return "SIGABRT";
	case SIGTRAP: return "SIGTRAP";
	default:      return "signal";
	}
}

static void gxCrashHandler(int sig, siginfo_t *info, void *ucv)
{
	if (!s_gxInCrash) {
		s_gxInCrash = 1;

		// 1) Log lines the pump thread hasn't consumed yet (the last words).
		if (s_gxPumpReadFd >= 0) {
			const int fl = fcntl(s_gxPumpReadFd, F_GETFL);
			if (fl != -1 && fcntl(s_gxPumpReadFd, F_SETFL, fl | O_NONBLOCK) != -1) {
				char buf[4096];
				ssize_t n;
				int rounds = 0;
				while (rounds++ < 64 && (n = read(s_gxPumpReadFd, buf, sizeof(buf))) > 0) {
					gxLogFileWrite(buf, (size_t)n, true);
				}
			}
		}

		// 2) What, where, which thread.
		char out[512];
		char thread[17] = "?";
		prctl(PR_GET_NAME, thread, 0, 0, 0);
		if (info != nullptr && info->si_code > 0) {
			snprintf(out, sizeof(out),
			         "\n*** CRASH: %s (signal %d, code %d), fault addr %p, thread %d '%s'\n",
			         gxSignalName(sig), sig, info->si_code, info->si_addr, (int)gettid(), thread);
		} else {
			snprintf(out, sizeof(out),
			         "\n*** CRASH: %s (signal %d, raised by pid %d), thread %d '%s'\n",
			         gxSignalName(sig), sig, info ? (int)info->si_pid : 0, (int)gettid(), thread);
		}
		gxCrashWrite(out);

		// 3) Faulting pc/lr, then walk the frame-pointer chain from the faulting
		// context (works without unwinding through the signal trampoline).
		if (ucv != nullptr) {
			const ucontext_t *uc = static_cast<const ucontext_t *>(ucv);
			const uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
			const uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
			const uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
			uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
			gxCrashWrite("backtrace (frame pointers):\n");
			gxCrashWriteFrame(0, "pc", pc);
			gxCrashWriteFrame(1, "lr", lr);
			int idx = 2;
			uintptr_t prev = sp;
			while (idx < 40 && fp != 0 && (fp & 0xF) == 0 && fp >= prev && fp - sp < 8u * 1024u * 1024u) {
				const uintptr_t *frame = reinterpret_cast<const uintptr_t *>(fp);
				const uintptr_t ret = frame[1];
				if (ret == 0) {
					break;
				}
				gxCrashWriteFrame(idx++, "fp", ret);
				prev = fp + 16;
				fp = frame[0];
			}
		}

		// 4) Unwinder backtrace (CFI-based: covers code built without frame pointers).
		gxCrashWrite("backtrace (unwinder, starts inside the crash handler):\n");
		GxUnwindState st = { 0 };
		_Unwind_Backtrace(gxUnwindFrame, &st);
		gxCrashWrite("*** end of crash report ***\n");
	}

	// Hand the signal to the previous handler (debuggerd -> tombstone + system dialog).
	for (int i = 0; i < GX_CRASH_SIGNAL_COUNT; ++i) {
		if (s_gxCrashSignals[i] == sig) {
			sigaction(sig, &s_gxOldSigActions[i], nullptr);
			break;
		}
	}
	if (info == nullptr || info->si_code <= 0) {
		raise(sig);  // sent by kill/abort: re-send; a hardware fault re-triggers on return
	}
}

static void gxInstallCrashHandler()
{
	if (s_gxLogFd < 0) {
		return;  // nowhere to write the report
	}
	static char s_altStack[64 * 1024];
	stack_t ss = {};
	ss.ss_sp = s_altStack;
	ss.ss_size = sizeof(s_altStack);
	sigaltstack(&ss, nullptr);  // main thread: survives a stack-overflow SIGSEGV

	struct sigaction sa = {};
	sa.sa_sigaction = gxCrashHandler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	for (int i = 0; i < GX_CRASH_SIGNAL_COUNT; ++i) {
		sigaction(s_gxCrashSignals[i], &sa, &s_gxOldSigActions[i]);
	}
	fprintf(stderr, "INFO: crash reports go to %s\n", GX_LOG_PATH);
}

// GeneralsX @feature FadiLabib 07/07/2026 Stage the bundled Mesa Turnip driver and
// point DXVK's Vulkan loader at it via adrenotools. WHY: the stock Qualcomm driver
// on the Adreno 650 only exposes Vulkan 1.1, which DXVK 2.6 rejects ("Skipping
// Vulkan 1.1 adapter" -> "No adapters found. A Vulkan 1.3 capable driver is
// required"). Turnip (Mesa, freedreno) implements Vulkan 1.3+ for Adreno 6xx.
//
// adrenotools requires: (a) its hook libs (libhook_impl.so / libmain_hook.so) in
// the APK nativeLibraryDir, and (b) the custom driver in a PRIVATE, non-sdcard dir
// that dlopen trusts (not world-writable). We bundle the driver as the jniLib
// libvulkan_freedreno.so (so Android extracts it to nativeLibraryDir), then copy it
// once into the app's internal files dir under its real name, and export the three
// env vars the DXVK loader (vulkan_loader.cpp adrenotools branch) reads.
//
// nativeLibraryDir is discovered with dladdr() on this function — it lives in
// libmain.so, which sits in nativeLibraryDir alongside the hook libs.
static bool gxCopyFile(const char *src, const char *dst)
{
	int in = open(src, O_RDONLY);
	if (in < 0) {
		fprintf(stderr, "WARNING: Turnip: open(%s) failed: %s\n", src, strerror(errno));
		return false;
	}
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out < 0) {
		fprintf(stderr, "WARNING: Turnip: open(%s) for write failed: %s\n", dst, strerror(errno));
		close(in);
		return false;
	}
	char buf[65536];
	ssize_t n;
	bool ok = true;
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < n) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w <= 0) { ok = false; break; }
			off += w;
		}
		if (!ok) break;
	}
	if (n < 0) ok = false;
	close(in);
	close(out);
	return ok;
}

static void gxSetupTurnipDriver()
{
	// Driver soname as it ships in the adrenotools package meta.json. adrenotools
	// dlopens this exact filename from GENERALSX_TURNIP_DRIVER_DIR.
	static const char *DRIVER_NAME = "vulkan.ad07xx.so";
	// The jniLib we ship the Turnip .so as (Android only extracts lib*.so names).
	static const char *BUNDLED_SONAME = "libvulkan_freedreno.so";

	// 1) nativeLibraryDir: the directory containing this very .so (libmain.so).
	Dl_info info;
	if (dladdr(reinterpret_cast<void *>(&gxSetupTurnipDriver), &info) == 0 || info.dli_fname == nullptr) {
		fprintf(stderr, "WARNING: Turnip: dladdr failed; cannot locate nativeLibraryDir — stock driver will be used\n");
		return;
	}
	char nativeLibDir[1024];
	snprintf(nativeLibDir, sizeof(nativeLibDir), "%s", info.dli_fname);
	if (char *slash = strrchr(nativeLibDir, '/')) {
		*slash = '\0';
	}

	// 2) Private files dir (dlopen-trusted, not sdcard) to hold the copied driver.
	const char *filesDir = SDL_GetAndroidInternalStoragePath();
	if (filesDir == nullptr) {
		fprintf(stderr, "WARNING: Turnip: internal storage path unavailable — stock driver will be used\n");
		return;
	}

	char srcPath[1024];
	char dstPath[1024];
	snprintf(srcPath, sizeof(srcPath), "%s/%s", nativeLibDir, BUNDLED_SONAME);
	snprintf(dstPath, sizeof(dstPath), "%s/%s", filesDir, DRIVER_NAME);

	if (access(srcPath, R_OK) != 0) {
		fprintf(stderr, "WARNING: Turnip: bundled driver %s not found (%s) — stock driver will be used\n",
		        BUNDLED_SONAME, strerror(errno));
		return;
	}

	// Copy once; refresh if the bundled driver changed size (e.g. after an update).
	struct stat srcStat{}, dstStat{};
	const bool haveDst = (stat(dstPath, &dstStat) == 0);
	const bool haveSrc = (stat(srcPath, &srcStat) == 0);
	if (!haveDst || !haveSrc || srcStat.st_size != dstStat.st_size) {
		if (!gxCopyFile(srcPath, dstPath)) {
			fprintf(stderr, "WARNING: Turnip: failed to stage driver to %s — stock driver will be used\n", dstPath);
			return;
		}
		fprintf(stderr, "INFO: Turnip: staged driver -> %s (%lld bytes)\n",
		        dstPath, (long long)srcStat.st_size);
	} else {
		fprintf(stderr, "INFO: Turnip: driver already staged at %s\n", dstPath);
	}

	// 3) Export the loader contract. DRIVER_DIR MUST end with '/': adrenotools
	// stat()s customDriverDir + customDriverName by string concatenation.
	char driverDir[1024];
	snprintf(driverDir, sizeof(driverDir), "%s/", filesDir);
	setenv("GENERALSX_ADRENOTOOLS_HOOK_DIR", nativeLibDir, 1);
	setenv("GENERALSX_TURNIP_DRIVER_DIR", driverDir, 1);
	setenv("GENERALSX_TURNIP_DRIVER_NAME", DRIVER_NAME, 1);
	fprintf(stderr, "INFO: Turnip: adrenotools env set (hookLibDir=%s driverDir=%s name=%s)\n",
	        nativeLibDir, driverDir, DRIVER_NAME);

	// GeneralsX @feature S25 07/10/2026 Adreno 830 (Snapdragon 8 Elite, Galaxy S25):
	// the A8xx Turnip build renders glitchy frames in GMEM (tiled) mode on A830 —
	// write page faults, per the driver's release notes — so render in sysmem mode.
	// Turnip reads TU_DEBUG from the process environment at instance creation, which
	// happens after this point. overwrite=0: a user-provided TU_DEBUG still wins.
	setenv("TU_DEBUG", "sysmem", 0);
	fprintf(stderr, "INFO: Turnip: TU_DEBUG=%s\n", getenv("TU_DEBUG"));
}
#endif // __ANDROID__

/**
 * FilterSoftwareVulkanICDs
 *
 * Sets VK_DRIVER_FILES to only hardware Vulkan ICDs, excluding LLVMpipe/lavapipe.
 *
 * Workaround for Mesa/LLVM 20.x bug: libvulkan_lvp.so (LLVMpipe Vulkan ICD) crashes
 * during dlopen() static initialization with a null-ptr deref in llvm::Regex::Regex().
 * The Vulkan loader loads ALL ICDs found in the ICD directories when
 * vkEnumerateInstanceExtensionProperties() is called, which triggers the crash.
 * Filtering hardware-only ICDs via VK_DRIVER_FILES prevents loading libvulkan_lvp.so.
 *
 * Only applied when neither VK_DRIVER_FILES nor VK_ICD_FILENAMES is already set,
 * so the user can always override by setting those variables externally.
 *
 * GeneralsX @bugfix BenderAI 06/03/2026
 */
static void FilterSoftwareVulkanICDs()
{
	if (getenv("VK_DRIVER_FILES") || getenv("VK_ICD_FILENAMES")) {
		return;
	}

	auto icd_is_software = [](const char *name) -> bool {
		char low[256] = "";
		for (int i = 0; name[i] && i < 255; ++i) {
			low[i] = (char)tolower((unsigned char)name[i]);
		}
		return strstr(low, "lvp") || strstr(low, "lavapipe") || strstr(low, "softpipe") || strstr(low, "llvmpipe");
	};

	static char hw_icds[4096] = "";
	const char *patterns[] = {
		"/usr/share/vulkan/icd.d/*.json",
		"/etc/vulkan/icd.d/*.json",
		nullptr
	};

	glob_t gl = {};
	int gflags = 0;
	for (int i = 0; patterns[i]; ++i) {
		if (glob(patterns[i], gflags, nullptr, &gl) == 0) {
			gflags = GLOB_APPEND;
		}
	}

	bool found_hw = false;
	for (size_t i = 0; i < gl.gl_pathc; ++i) {
		const char *path = gl.gl_pathv[i];
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		if (icd_is_software(base)) {
			fprintf(stderr, "INFO: Vulkan ICD filter: skipping software ICD '%s'\n", base);
			continue;
		}
		if (found_hw) {
			strncat(hw_icds, ":", sizeof(hw_icds) - strlen(hw_icds) - 1);
		}
		strncat(hw_icds, path, sizeof(hw_icds) - strlen(hw_icds) - 1);
		found_hw = true;
	}
	globfree(&gl);

	if (found_hw) {
		setenv("VK_DRIVER_FILES", hw_icds, 1);
		fprintf(stderr, "INFO: Vulkan ICD filter: VK_DRIVER_FILES=%s\n", hw_icds);
	} else {
		fprintf(stderr, "WARNING: Vulkan ICD filter: no hardware ICDs found, LLVMpipe exclusion skipped\n");
		fprintf(stderr, "WARNING: If startup crashes in libvulkan_lvp.so, set VK_DRIVER_FILES manually\n");
	}
}

/**
 * FilterPipeWireOpenAL
 *
 * Sets ALSOFT_DRIVERS to skip PipeWire, falling back to pulse/alsa.
 *
 * Workaround for openal-soft PipeWire backend crash: alcOpenDevice() segfaults
 * inside the PipeWire backend while opening the default playback device.
 * The crash occurs in PipeWire's stream/context internals and is unrecoverable
 * from userspace. Excluding PipeWire via ALSOFT_DRIVERS causes openal-soft to
 * fall back to the PulseAudio backend, which works correctly on PipeWire systems
 * via the PulseAudio compatibility layer.
 *
 * NOTE: openal-soft reads ALSOFT_DRIVERS from a static global constructor when
 * libopenal.so is loaded by the dynamic linker, which is before main() runs.
 * This function is therefore only effective for builds that use lazy
 * initialization. The authoritative fix is in the launch scripts (run-linux-zh.sh
 * etc.), which set ALSOFT_DRIVERS before the binary starts.
 *
 * Only applied when ALSOFT_DRIVERS is not already set by the user.
 *
 * GeneralsX @bugfix 09/03/2026
 */
static void FilterPipeWireOpenAL()
{
	// GeneralsX @bugfix Copilot 24/03/2026 PipeWire/OpenAL workaround is Linux-only; keep macOS CoreAudio backend selection untouched.
	// GeneralsX @android FadiLabib 07/07/2026 - __ANDROID__ also defines __linux__, but the
	// desktop driver list (pulse,alsa,oss,jack,...) contains no backend that exists on
	// Android, so openal-soft fell through to 'null'/'wave' and the game was silent.
	// Android's libopenal.so ships the OpenSL backend; leave default selection intact.
	// The movaps CPU-ext workaround is x86-only anyway (arm64 NEON has no such fault).
	#if defined(__linux__) && !defined(__ANDROID__)
	// Crash: alcOpenDevice() hits 'movaps %xmm1,0x26260(%rbx)' — SSE movaps requires
	// 16-byte alignment; a misaligned ALCdevice struct faults regardless of backend.
	// Disabling CPU extensions forces openal-soft to use scalar code that has no
	// alignment requirements. Also exclude pipewire which has its own crash at
	// device-open time on PipeWire 1.4.x.
	// NOTE: these env vars are authoritative only when set before the binary loads
	// (openal-soft reads them from a static constructor). The launch scripts set them
	// first; this is a best-effort fallback for lazy-init builds.
	if (!getenv("ALSOFT_DISABLE_CPU_EXTS")) {
		setenv("ALSOFT_DISABLE_CPU_EXTS", "all", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DISABLE_CPU_EXTS=all (movaps alignment crash workaround)\n");
	}
	if (!getenv("ALSOFT_DRIVERS")) {
		setenv("ALSOFT_DRIVERS", "pulse,alsa,oss,jack,null,wave", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DRIVERS=pulse,alsa,oss,jack,null,wave (pipewire excluded)\n");
	}
	#else
	fprintf(stderr, "INFO: OpenAL: keeping default driver selection on non-Linux platform\n");
	#endif
}

/**
 * CreateGameEngine
 *
 * Factory function for SDL3GameEngine on Linux.
 * Called by GameMain() to instantiate platform-specific engine.
 *
 * @return SDL3GameEngine instance
 */
GameEngine *CreateGameEngine(void)
{
	fprintf(stderr, "INFO: CreateGameEngine() - Creating SDL3GameEngine for Linux\n");
	SDL3GameEngine *engine = NEW SDL3GameEngine();
	return engine;
}

/**
 * main
 *
 * Linux entry point (replaces WinMain on Windows).
 * Initializes subsystems and calls GameMain().
 *
 * @param argc Command line argument count
 * @param argv Command line arguments
 * @return Exit code (0 = success)
 */
int main(int argc, char* argv[])
{
	int exitcode = 1;

	// TheSuperHackers @build felipebraz 13/02/2026
	// Store command line arguments in globals for CommandLine.cpp parser
	__argc = argc;
	__argv = argv;

#if defined(__ANDROID__)
	// GeneralsX @feature FadiLabib 06/07/2026 Route stdout/stderr to logcat FIRST,
	// before any diagnostic below, so the whole on-device run is visible via
	// `adb logcat -s GeneralsX` (Android otherwise discards app stdio).
	gxRedirectStdioToLogcat();
	// GeneralsX @feature S25 08/10/2026 ...and into /sdcard/GeneralsZH/generals-log.txt,
	// with a crash report appended on a fatal signal (see gxInstallCrashHandler).
	gxInstallCrashHandler();

	// GeneralsX @feature FadiLabib 06/07/2026 Android bootstrap.
	// The engine resolves ALL game data relative to the working directory
	// (see StdLocalFileSystem); assets are pushed to /sdcard/GeneralsZH by
	// scripts/build/android/push-assets-android.sh (targetSdk 29 +
	// requestLegacyExternalStorage keeps that path readable).
	// HOME must exist because GlobalData's user-data path and the registry
	// shim derive from it on the POSIX branch; Android doesn't set it.
	{
		static const char *GX_ANDROID_ASSET_DIR = "/sdcard/GeneralsZH";
		const char *internal = SDL_GetAndroidInternalStoragePath();
		if (internal != nullptr) {
			setenv("HOME", internal, 0);
			setenv("XDG_DATA_HOME", internal, 0);
		}
		if (chdir(GX_ANDROID_ASSET_DIR) != 0) {
			fprintf(stderr, "FATAL: chdir(%s) failed: %s — push assets first "
			        "(scripts/build/android/push-assets-android.sh)\n",
			        GX_ANDROID_ASSET_DIR, strerror(errno));
			return 1;
		}
		fprintf(stderr, "INFO: Android working directory: %s, HOME=%s\n",
		        GX_ANDROID_ASSET_DIR, internal ? internal : "<unset>");
	}

	// GeneralsX @feature FadiLabib 07/07/2026 Stage Turnip + set the adrenotools env
	// BEFORE any Vulkan load (SDL_Vulkan_LoadLibrary below, and DXVK's own loader when
	// the d3d8 device is created). Gives DXVK a Vulkan 1.3 adapter instead of the
	// stock Adreno 1.1 driver it rejects.
	gxSetupTurnipDriver();
#endif

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Diagnostic capture: an icon-launched app's stderr goes nowhere we can read,
	// so mirror it to a file in Library/Caches (purgeable, not user-visible). This
	// lets us pull a full engine log after an on-device session — essential for
	// debugging mode-specific issues (e.g. Generals Challenge radar/scripts) that
	// only the user can reproduce. Pull with: devicectl ... copy from
	// Library/Caches/generals-stderr.log. Remove once the relevant bugs are fixed.
	{
		// Quiet DXVK at the source: the d3d8 layer's per-call warns (e.g. an
		// unimplemented render state set every frame) wrote hundreds of MB per
		// long session. The shipped dxvk.conf also sets logLevel=none; the env
		// covers modules that read it before the config.
		setenv("DXVK_LOG_LEVEL", "none", 0);
		const char *diagHome = getenv("HOME");
		if (diagHome != nullptr) {
			char diagPath[1024];
			char prevPath[1024];
			// Documents, not Library/Caches: Caches is purgeable (a device restart or
			// storage pressure can empty it), and Documents is user-reachable via the
			// Files app since the bundle enables UIFileSharingEnabled.
			snprintf(diagPath, sizeof(diagPath), "%s/Documents/generals-stderr.log", diagHome);
			// Keep the previous session's log: a session that ends in a memory kill
			// leaves no OS crash report, so the prior log is often the only evidence.
			snprintf(prevPath, sizeof(prevPath), "%s/Documents/generals-stderr-prev.log", diagHome);
			rename(diagPath, prevPath);
			// Filtered + capped sink instead of a raw freopen: per-frame debug spam
			// (upstream [GX-ISSUE144] font traces, [INI] loader traces, residual DXVK
			// warns) is dropped, and the file stops growing at 8 MB so a marathon
			// session cannot eat device storage. funopen() is fine here: this is
			// Darwin-only code.
			static int s_logFd = -1;
			s_logFd = open(diagPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (s_logFd >= 0) {
				static size_t s_logWritten = 0;
				FILE *sink = funopen(nullptr,
					nullptr,
					[](void *, const char *buf, int len) -> int {
						static const size_t kLogCap = 8u * 1024u * 1024u;
						if (s_logFd < 0) return len;
						if (len > 13 &&
						    (memcmp(buf, "[GX-ISSUE144]", 13) == 0 ||
						     memcmp(buf, "[INI] ", 6) == 0 ||
						     memcmp(buf, "warn:  D3D8De", 13) == 0)) {
							return len;  // drop known per-frame spam, report consumed
						}
						if (s_logWritten >= kLogCap) {
							// past the cap, still record errors — the tail of a dying
							// session is this log's whole reason to exist
							static bool s_capMarked = false;
							if (!s_capMarked) {
								s_capMarked = true;
								const char *mark = "[log capped: non-error lines dropped from here]\n";
								write(s_logFd, mark, strlen(mark));
							}
							if (len > 4 && (memcmp(buf, "err:", 4) == 0 ||
							                memcmp(buf, "ERROR", 5) == 0 ||
							                memcmp(buf, "FATAL", 5) == 0)) {
								write(s_logFd, buf, (size_t)len);
							}
							return len;
						}
						ssize_t w = write(s_logFd, buf, (size_t)len);
						if (w > 0) s_logWritten += (size_t)w;
						return len;
					},
					nullptr, nullptr);
				if (sink != nullptr) {
					*stderr = *sink;  // classic Darwin stderr swap; stderr is a FILE, not a macro here
					setvbuf(stderr, nullptr, _IOLBF, 0);  // line-buffered so a crash still flushes recent lines
				}
			}
		}
	}

	// The engine resolves all game data relative to the working directory.
	// Preferred layout: assets ship read-only INSIDE the signed app bundle
	// (<bundle>/GameData), the iOS-sanctioned home for app resources — the
	// install is then fully self-contained. Dev builds packaged without
	// assets fall back to the Documents folder (Files-app accessible).
	// User data (saves, Options.ini) always lives in Library/Application
	// Support via the engine's user-data path; never in the bundle.
	{
		const char *home = getenv("HOME");

		// <bundle>/GameData, derived from the executable path (argv[0])
		char bundleData[1024] = {0};
		if (argc > 0 && argv[0] != nullptr) {
			const char *slash = strrchr(argv[0], '/');
			if (slash != nullptr) {
				const size_t dirLen = (size_t)(slash - argv[0]);
				if (dirLen < sizeof(bundleData) - 16) {
					memcpy(bundleData, argv[0], dirLen);
					snprintf(bundleData + dirLen, sizeof(bundleData) - dirLen, "/GameData");
				}
			}
		}

		bool usingBundleData = false;
		if (bundleData[0] != '\0' && access(bundleData, R_OK) == 0) {
			if (chdir(bundleData) == 0) {
				usingBundleData = true;
				fprintf(stderr, "INFO: iOS working directory (bundle): %s\n", bundleData);
			}
		}
		if (!usingBundleData && home != nullptr) {
			char docs[1024];
			snprintf(docs, sizeof(docs), "%s/Documents", home);
			if (chdir(docs) != 0) {
				fprintf(stderr, "WARNING: chdir(%s) failed: %s\n", docs, strerror(errno));
			} else {
				fprintf(stderr, "INFO: iOS working directory (Documents): %s\n", docs);
			}
		}

		if (home != nullptr) {
			// Keep DXVK's shader cache in Library/Caches: purgeable under
			// storage pressure, excluded from iCloud backup, invisible in the
			// Files app. Must be set before the d3d8 dylib loads.
			char cacheDir[1024];
			snprintf(cacheDir, sizeof(cacheDir), "%s/Library/Caches", home);
			mkdir(cacheDir, 0755);
			setenv("DXVK_STATE_CACHE_PATH", cacheDir, 0);

			if (usingBundleData) {
				// Seed default settings on first run (full detail instead of the
				// 2003 auto-detect, which drops unknown GPUs to Low).
				char userDataDir[1024], optionsPath[1024];
				snprintf(userDataDir, sizeof(userDataDir),
				         "%s/Library/Application Support/GeneralsX/GeneralsZH", home);
				snprintf(optionsPath, sizeof(optionsPath), "%s/Options.ini", userDataDir);
				if (access(optionsPath, F_OK) != 0 && access("DefaultOptions.ini", R_OK) == 0) {
					std::error_code fsError;
					std::filesystem::create_directories(userDataDir, fsError);
					std::filesystem::copy_file("DefaultOptions.ini", optionsPath, fsError);
					if (!fsError) {
						fprintf(stderr, "INFO: Seeded default Options.ini\n");
					}
				}

				// One-time tidy-up: remove asset copies from Documents now that
				// the bundle carries them. Guarded by a sentinel so it truly runs
				// once — Documents is exposed via the Files app, and anything the
				// user places there later (mods, custom maps) must never be touched.
				// "Maps" is deliberately NOT in the list: it is where user maps live.
				char docs[1024];
				snprintf(docs, sizeof(docs), "%s/Documents", home);
				char sentinel[1024];
				snprintf(sentinel, sizeof(sentinel), "%s/.bundle-assets-tidied", docs);
				if (access(sentinel, F_OK) != 0) {
					std::error_code fsError;
					for (const auto &entry : std::filesystem::directory_iterator(docs, fsError)) {
						const std::string name = entry.path().filename().string();
						const bool isShippedAsset =
							(name.size() > 4 && name.compare(name.size() - 4, 4, ".big") == 0) ||
							name == "Data" || name == "Window" || name == "ZH_Generals" ||
							name == "fonts" || name == "_CommonRedist" ||
							name == "dxvk.conf" || name == "GeneralsXZH.dxvk-cache" ||
							name == "GeneralsXZH_d3d9.log";
						if (isShippedAsset) {
							fprintf(stderr, "INFO: tidy-up removing shipped asset copy: %s\n", name.c_str());
							std::error_code removeError;
							std::filesystem::remove_all(entry.path(), removeError);
						}
					}
					if (!fsError) {  // a failed scan must retry next launch, not fail closed forever
						FILE *s = fopen(sentinel, "w");
						if (s) fclose(s);
					}
				}
			}
		}
	}
#endif

	fprintf(stderr, "=================================================\n");
	fprintf(stderr, " Command & Conquer Generals: Zero Hour (Linux)\n");
	fprintf(stderr, " SDL3 + DXVK Build\n");
	fprintf(stderr, "=================================================\n\n");

	try {
		// Initialize critical sections (required by game engine)
		TheAsciiStringCriticalSection = &critSec1;
		TheUnicodeStringCriticalSection = &critSec2;
		TheDmaCriticalSection = &critSec3;
		TheMemoryPoolCriticalSection = &critSec4;
		TheDebugLogCriticalSection = &critSec5;

		// Initialize memory manager early (required by NEW operator)
		initMemoryManager();

		// GeneralsX @bugfix BenderAI 14/02/2026 Initialize Version singleton
		// GameEngine::init() calls updateWindowTitle() which uses TheVersion
		// Must be created before GameMain() to avoid nullptr dereference
		TheVersion = NEW Version;

		// Parse command line (CommandLine class handles argc/argv internally)
		// TheSuperHackers @build felipebraz 10/02/2026 Phase 1.5
		// Store argc/argv for CommandLine parser to access via _NSGetArgc/_NSGetArgv or /proc/self/cmdline
		// For now, let CommandLine::parseCommandLineForStartup() handle this
		CommandLine::parseCommandLineForStartup();

		// GeneralsX @bugfix Copilot 17/05/2026 Skip SDL3 window bootstrap for CLI/headless replay execution.
		const bool isHeadlessMode = (TheGlobalData != nullptr && TheGlobalData->m_headless);
		if (isHeadlessMode) {
			fprintf(stderr, "INFO: Headless mode detected, skipping SDL3 video/Vulkan window initialization\n");
		} else {

		// GeneralsX @bugfix felipebraz 16/02/2026
		// Initialize SDL3 and Vulkan BEFORE creating GameEngine (fighter19 pattern)
		// This prevents LLVM SIGSEGV crash during Vulkan driver enumeration
		// Must be done here, not in SDL3GameEngine::init() which is too late
		fprintf(stderr, "INFO: Initializing SDL3 video subsystem...\n");
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
		// All mouse events are synthesized by the gesture translator in
		// SDL3GameEngine.cpp; SDL's automatic touch->mouse synthesis would
		// double-deliver finger 1 and fight the two-finger pan logic.
		// GeneralsX @android FadiLabib 07/07/2026 - Android uses the same translator.
		SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
		if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
			fprintf(stderr, "FATAL: Failed to initialize SDL3: %s\n", SDL_GetError());
			return 1;
		}

		// Set DXVK WSI driver before loading Vulkan
		setenv("DXVK_WSI_DRIVER", "SDL3", 1);

#if defined(__ANDROID__)
		// GeneralsX @android FadiLabib 07/07/2026 - Defer DXVK surface creation until
		// first Present. An ANativeWindow allows exactly ONE producer connection, and
		// W3D's device-creation retry loop (W3DDisplay::init / dx8wrapper) can build a
		// doomed first device (e.g. D3DFMT_D32 depth, unsupported by the driver) whose
		// implicit swapchain claims the window before failing; every later device then
		// dies with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR and the screen stays black. With
		// deferSurfaceCreation only the device that actually presents touches the
		// window. overwrite=0 so a user-set DXVK_CONFIG still wins.
		// samplerAnisotropy=16: the DX8 engine never requests anisotropic
		// filtering, so terrain smears under the tilted RTS camera. Mobile GPUs
		// (Adreno/Turnip and Xclipse alike) support it; force it for sharp ground
		// textures. A user-supplied dxvk.conf can still override via the file.
		setenv("DXVK_CONFIG", "d3d9.deferSurfaceCreation = True;d3d9.samplerAnisotropy = 16", 0);
#endif

		// GeneralsX @bugfix BenderAI 06/03/2026 - Exclude LLVMpipe Vulkan ICD before loading Vulkan.
		// libvulkan_lvp.so crashes during static initialization with LLVM 20.x when the Vulkan
		// loader enumerates all ICDs. Restrict to hardware ICDs first.
		FilterSoftwareVulkanICDs();
		FilterPipeWireOpenAL();

		// Load Vulkan library for DXVK DirectX8→Vulkan translation
		fprintf(stderr, "INFO: Loading Vulkan library...\n");
		if (!SDL_Vulkan_LoadLibrary(nullptr)) {
			fprintf(stderr, "WARNING: Failed to load Vulkan: %s\n", SDL_GetError());
			fprintf(stderr, "WARNING: Continuing without Vulkan (may use software rendering)\n");
		}

		// Create SDL3 window with Vulkan support
		fprintf(stderr, "INFO: Creating SDL3 Vulkan window...\n");
		Uint32 windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;  // Start hidden, show after D3D init
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// Request a native-resolution Metal drawable (e.g. 2868x1320 instead of the
		// 956x440 point size). Without this the swapchain renders at point size and
		// the display upscales 3x, visibly blurring textures and terrain.
		windowFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
		TheSDL3Window = SDL_CreateWindow(
			"Command & Conquer Generals: Zero Hour",
			1024, 768,  // Default resolution
			windowFlags
		);

		if (!TheSDL3Window) {
			fprintf(stderr, "FATAL: Failed to create SDL3 window: %s\n", SDL_GetError());
			SDL_Quit();
			return 1;
		}

		// Store window handle globally (cast SDL_Window* to HWND for compatibility)
		ApplicationHWnd = (HWND)TheSDL3Window;
		fprintf(stderr, "INFO: SDL3 window created successfully\n");

#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
		// Match the game's internal resolution to the phone screen's aspect ratio.
		// Without this the engine runs its 4:3 default inside the 19.5:9 display:
		// pillarboxed picture and a skewed window->game coordinate mapping. Height
		// stays at the engine's 600px design baseline (UI layouts assume >= 600);
		// width follows the real aspect. Injected as -xres/-yres argv entries so
		// the normal command-line path applies them (user-passed flags still win
		// because the parser lets later arguments override earlier ones... ours go
		// last, so only add them if the user didn't pass explicit -xres/-yres).
		{
			bool userSetRes = false;
			for (int i = 1; i < __argc; ++i) {
				if (strcmp(__argv[i], "-xres") == 0 || strcmp(__argv[i], "-yres") == 0) {
					userSetRes = true;
					break;
				}
			}
			// Use the pixel size of the high-density drawable: the game renders
			// 1:1 into the native-resolution swapchain, and fonts/UI rescale via
			// the engine's resolution-aware font scaling (GlobalLanguage).
			int winW = 0, winH = 0;
#if defined(__ANDROID__)
			// GeneralsX @android FadiLabib 07/07/2026 - The fullscreen window's size
			// isn't laid out yet at this point on Android (surface creation is
			// deferred), so query the panel's native mode directly — always valid.
			// Force landscape dims (the app is orientation-locked) so a portrait-
			// reported mode still yields xres>yres. Measured 8.3% black pillarbox
			// bars each side without this (4:3 default in a 16:10 panel).
			if (const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay())) {
				winW = dm->w > dm->h ? dm->w : dm->h;
				winH = dm->w > dm->h ? dm->h : dm->w;
			}
#else
			SDL_GetWindowSizeInPixels(TheSDL3Window, &winW, &winH);
#endif
			if (!userSetRes && winW > 0 && winH > 0 && winW > winH) {
				static char xresVal[16], yresVal[16];
				static char xresFlag[] = "-xres";
				static char yresFlag[] = "-yres";
				const int yres = winH;
				int xres = winW;
				xres &= ~1;  // keep it even
				snprintf(xresVal, sizeof(xresVal), "%d", xres);
				snprintf(yresVal, sizeof(yresVal), "%d", yres);

				static char* newArgv[64];
				int n = 0;
				for (int i = 0; i < __argc && n < 59; ++i) {
					newArgv[n++] = __argv[i];
				}
				newArgv[n++] = xresFlag;
				newArgv[n++] = xresVal;
				newArgv[n++] = yresFlag;
				newArgv[n++] = yresVal;
				newArgv[n] = nullptr;
				__argv = newArgv;
				__argc = n;
				fprintf(stderr, "INFO: internal resolution set to %sx%s (screen %dx%d)\n",
				        xresVal, yresVal, winW, winH);
			}
		}
#endif
		}

		// Call cross-platform game entry point
		exitcode = GameMain();

		fprintf(stderr, "INFO: GameMain() returned with code %d\n", exitcode);

	} catch (const std::exception& e) {
		fprintf(stderr, "FATAL: Unhandled exception in main(): %s\n", e.what());
		exitcode = 1;
	} catch (...) {
		fprintf(stderr, "FATAL: Unknown exception in main()\n");
		exitcode = 1;
	}

	// Cleanup SDL3 resources
	if (TheSDL3Window) {
		SDL_DestroyWindow(TheSDL3Window);
		TheSDL3Window = nullptr;
		ApplicationHWnd = nullptr;
	}
	SDL_Quit();

	// GeneralsX @bugfix BenderAI 14/02/2026 Cleanup Version singleton
	if (TheVersion) {
		delete TheVersion;
		TheVersion = nullptr;
	}

	// GeneralsX @bugfix BenderAI 19/02/2026 Shutdown memory manager BEFORE nulling critical
	// sections. Without this, global pool destructors (ObjectPoolClass) crash during atexit()
	// because they call ::operator delete after the memory manager is already gone (SIGSEGV).
	// Matches WinMain.cpp cleanup order: TheVersion -> shutdownMemoryManager -> null critSecs.
	shutdownMemoryManager();

	// Cleanup critical sections (after memory manager, which may use them during shutdown)
	TheAsciiStringCriticalSection = nullptr;
	TheUnicodeStringCriticalSection = nullptr;
	TheDmaCriticalSection = nullptr;
	TheMemoryPoolCriticalSection = nullptr;
	TheDebugLogCriticalSection = nullptr;

	fprintf(stderr, "\nExiting with code %d\n", exitcode);

#if defined(__ANDROID__)
	// GeneralsX @feature FadiLabib 06/07/2026 Robust completion signal for the
	// headless-replay harness: logcat can be racy to poll (buffer wrap, tag
	// filtering), so also drop the final exit code into a file the harness reads
	// back with a single `adb shell cat`. Placed here (after the try/catch) so it
	// captures the exception path too. Written next to the assets, which the
	// process already chdir'd into and can write (legacy external storage).
	{
		FILE *rc = fopen("/sdcard/GeneralsZH/last-run-exitcode.txt", "w");
		if (rc != nullptr) {
			fprintf(rc, "%d\n", exitcode);
			fclose(rc);
		}
	}
#endif

	// GeneralsX @bugfix BenderAI 25/02/2026 — use _exit() to skip C++ global destructors.
	// On macOS, __cxa_finalize_ranges runs ObjectPoolClass<X,256> global dtors after main() returns.
	// Those dtors crash with a corrupted BlockListHead (SIGSEGV at 0x4ade32ec4ade0018) because
	// pool block memory was already reused/overwritten during game shutdown.
	// Windows never had this problem — ExitProcess() terminates without running C++ global dtors.
	// _exit() matches that behavior. Explicit cleanup already done above (SDL_Quit, shutdownMemoryManager).
	_exit(exitcode);
}

#endif // !_WIN32
