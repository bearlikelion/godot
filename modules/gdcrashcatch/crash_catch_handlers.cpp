/**************************************************************************/
/*  crash_catch_handlers.cpp                                              */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#include "crash_catch_handlers.h"

#include "crash_catch_report.h"

#include "core/os/os.h"
#include "core/string/print_string.h"

// The path to write the in-handler ".partial" report to. Computed at install()
// time and stored as a raw NUL-terminated C string so the signal handler never
// has to touch String/heap. Sized generously for a user data path.
static char s_partial_path_template[1024] = { 0 };
static bool s_installed = false;

// Builds "<report_dir>/crash-<unixtime>-<pid>.partial" into dst using only
// async-signal-safe operations. Returns false if it would not fit.
static bool build_partial_path_signal_safe(char *dst, size_t dst_size);

#if defined(UNIX_ENABLED) || defined(__linux__) || defined(__APPLE__)

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

static const int CC_SIGNALS[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };
static const int CC_SIGNAL_COUNT = sizeof(CC_SIGNALS) / sizeof(CC_SIGNALS[0]);
static struct sigaction s_old_actions[CC_SIGNAL_COUNT];

// Async-signal-safe handler. Captures the backtrace, writes a minimal partial
// report, then chains to the previously-installed handler so the engine's own
// crash handler (in debug builds) and the default disposition still run.
static void cc_signal_handler(int p_sig, siginfo_t *p_info, void *p_ucontext) {
	void *frames[128];
	int frame_count = backtrace(frames, 128);

	char path[1024];
	if (build_partial_path_signal_safe(path, sizeof(path))) {
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0) {
			CrashCatchReport::write_partial_signal_safe(fd, p_sig, frames, frame_count);
			close(fd);
		}
	}

	// Chain to the previous handler for this signal.
	int idx = -1;
	for (int i = 0; i < CC_SIGNAL_COUNT; i++) {
		if (CC_SIGNALS[i] == p_sig) {
			idx = i;
			break;
		}
	}
	if (idx >= 0) {
		struct sigaction &old = s_old_actions[idx];
		if ((old.sa_flags & SA_SIGINFO) && old.sa_sigaction) {
			old.sa_sigaction(p_sig, p_info, p_ucontext);
			return;
		}
		if (old.sa_handler == SIG_IGN) {
			return;
		}
		if (old.sa_handler && old.sa_handler != SIG_DFL) {
			old.sa_handler(p_sig);
			return;
		}
	}

	// No prior handler: restore default and re-raise so the OS produces its
	// normal crash (core dump, exit code) after we have written our report.
	signal(p_sig, SIG_DFL);
	raise(p_sig);
}

void CrashCatchHandlers::install() {
	if (s_installed) {
		return;
	}
	if (!build_partial_path_signal_safe(s_partial_path_template, sizeof(s_partial_path_template))) {
		// Path won't fit; refuse rather than risk a buffer overrun in-handler.
		WARN_PRINT("CrashCatch: report path too long; native handlers not installed.");
		return;
	}

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = cc_signal_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_SIGINFO | SA_RESTART;

	for (int i = 0; i < CC_SIGNAL_COUNT; i++) {
		sigaction(CC_SIGNALS[i], &action, &s_old_actions[i]);
	}
	s_installed = true;
}

void CrashCatchHandlers::uninstall() {
	if (!s_installed) {
		return;
	}
	for (int i = 0; i < CC_SIGNAL_COUNT; i++) {
		sigaction(CC_SIGNALS[i], &s_old_actions[i], nullptr);
	}
	s_installed = false;
}

#elif defined(WINDOWS_ENABLED) || defined(_WIN32)

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>

static LPTOP_LEVEL_EXCEPTION_FILTER s_old_filter = nullptr;

static LONG WINAPI cc_exception_filter(EXCEPTION_POINTERS *p_exception) {
	void *frames[128];
	// RtlCaptureStackBackTrace is safe to call here and uses the current context.
	USHORT frame_count = RtlCaptureStackBackTrace(0, 128, frames, nullptr);

	char path[1024];
	if (build_partial_path_signal_safe(path, sizeof(path))) {
		// Use a CRT fd so the report writer's write()-based path works unchanged.
		int fd = _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
		if (fd >= 0) {
			CrashCatchReport::write_partial_signal_safe(fd, (int)p_exception->ExceptionRecord->ExceptionCode, frames, (int)frame_count);
			_close(fd);
		}
	}

	if (s_old_filter) {
		return s_old_filter(p_exception);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

void CrashCatchHandlers::install() {
	if (s_installed) {
		return;
	}
	if (!build_partial_path_signal_safe(s_partial_path_template, sizeof(s_partial_path_template))) {
		WARN_PRINT("CrashCatch: report path too long; native handlers not installed.");
		return;
	}
	s_old_filter = SetUnhandledExceptionFilter(cc_exception_filter);
	s_installed = true;
}

void CrashCatchHandlers::uninstall() {
	if (!s_installed) {
		return;
	}
	SetUnhandledExceptionFilter(s_old_filter);
	s_old_filter = nullptr;
	s_installed = false;
}

#else // Unsupported platform: no-op handlers.

void CrashCatchHandlers::install() {}
void CrashCatchHandlers::uninstall() {}

#endif

bool CrashCatchHandlers::is_installed() {
	return s_installed;
}

// ----------------------------------------------------------------------------
// Shared path builder (signal-safe: only integer formatting + memcpy).
// ----------------------------------------------------------------------------

// Cached report directory as a raw C string, populated on first install().
static char s_report_dir_cstr[768] = { 0 };

static void itoa_unsigned(uint64_t v, char *buf, int &len) {
	char tmp[24];
	int n = 0;
	if (v == 0) {
		tmp[n++] = '0';
	}
	while (v > 0) {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	for (int i = 0; i < n; i++) {
		buf[i] = tmp[n - 1 - i];
	}
	len = n;
}

static bool build_partial_path_signal_safe(char *dst, size_t dst_size) {
	// Populate the cached dir once, here, from the (safe) engine call. After the
	// first call this is just integer formatting and memcpy, so it stays usable
	// from a signal handler.
	if (s_report_dir_cstr[0] == 0) {
		String dir = CrashCatchReport::get_report_dir();
		CharString cs = dir.utf8();
		if ((size_t)cs.length() + 1 > sizeof(s_report_dir_cstr)) {
			return false;
		}
		memcpy(s_report_dir_cstr, cs.get_data(), cs.length() + 1);
	}

	size_t dir_len = strlen(s_report_dir_cstr);
	// dir + "/crash-" + time + "-" + pid + ".partial"
	const char *prefix = "/crash-";
	const char *suffix = ".partial";

#if defined(UNIX_ENABLED) || defined(__linux__) || defined(__APPLE__)
	uint64_t now = (uint64_t)time(nullptr);
	uint64_t pid = (uint64_t)getpid();
#elif defined(WINDOWS_ENABLED) || defined(_WIN32)
	uint64_t now = (uint64_t)GetTickCount64();
	uint64_t pid = (uint64_t)GetCurrentProcessId();
#else
	uint64_t now = 0;
	uint64_t pid = 0;
#endif

	char num_time[24];
	char num_pid[24];
	int lt = 0, lp = 0;
	itoa_unsigned(now, num_time, lt);
	itoa_unsigned(pid, num_pid, lp);

	size_t needed = dir_len + strlen(prefix) + lt + 1 /*'-'*/ + lp + strlen(suffix) + 1;
	if (needed > dst_size) {
		return false;
	}

	char *w = dst;
	memcpy(w, s_report_dir_cstr, dir_len);
	w += dir_len;
	memcpy(w, prefix, strlen(prefix));
	w += strlen(prefix);
	memcpy(w, num_time, lt);
	w += lt;
	*w++ = '-';
	memcpy(w, num_pid, lp);
	w += lp;
	memcpy(w, suffix, strlen(suffix));
	w += strlen(suffix);
	*w = 0;
	return true;
}
