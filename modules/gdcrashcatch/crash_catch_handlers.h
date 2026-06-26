/**************************************************************************/
/*  crash_catch_handlers.h                                                */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#ifndef CRASH_CATCH_HANDLERS_H
#define CRASH_CATCH_HANDLERS_H

// Installs and removes the module's own crash handlers.
//
// These intentionally duplicate (and chain to) the engine's platform crash
// handlers, because the engine handlers are compiled out of template_release
// builds (CRASH_HANDLER_ENABLED requires DEBUG_ENABLED). Owning our own handlers
// is what lets a shipped game still produce a crash report.
//
// On POSIX we use sigaction for SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS and save the
// previous handlers so we can chain to them after writing our signal-safe report.
// On Windows we install an unhandled-exception filter that chains to the prior one.
class CrashCatchHandlers {
public:
	// Installs handlers. Opens a report file descriptor lazily on crash; caches
	// the target directory path now. Safe to call once at module init.
	static void install();

	// Restores previously-saved handlers. Called at module uninit.
	static void uninstall();

	static bool is_installed();
};

#endif // CRASH_CATCH_HANDLERS_H
