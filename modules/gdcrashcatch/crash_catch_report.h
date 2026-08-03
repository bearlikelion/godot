/**************************************************************************/
/*  crash_catch_report.h                                                  */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#ifndef CRASH_CATCH_REPORT_H
#define CRASH_CATCH_REPORT_H

#include "core/string/ustring.h"
#include "core/templates/vector.h"

// Two-phase crash report handling.
//
// Phase 1 (in a signal handler): only async-signal-safe primitives may run, so
// CrashCatchReport::write_partial_signal_safe() writes a tiny fixed-format text
// record using cached, pre-formatted bytes and raw write() calls. No Variant,
// String, or heap allocation happens on the crashing path.
//
// Phase 2 (next launch, normal context): promote_partials() reads each .partial
// file and rewrites it as a complete JSON report, enriching it with data that
// could not be gathered safely during the crash (log tail, system info, and any
// GDScript backtrace captured through NOTIFICATION_CRASH).
class CrashCatchReport {
public:
	// Directory (expanded, OS path) where reports live, e.g. user://crash_reports.
	static String get_report_dir();
	static String get_report_dir_global(); // The "user://crash_reports" form.

	// Ensures the report directory exists. Returns false on failure.
	static bool ensure_report_dir();

	// Extra fault detail the platform handler can supply. All-zero when the
	// platform gives us nothing (POSIX signals other than SIGSEGV/SIGBUS).
	struct FaultInfo {
		uint64_t fault_pc = 0; // Faulting instruction pointer.
		uint64_t fault_address = 0; // Data address that was accessed.
		int access_type = -1; // 0 read, 1 write, 8 execute (DEP); -1 unknown.
	};

	// Phase 1: called from the crash handler. p_fd is a file descriptor opened at
	// init time. Writes raw addresses + cached metadata. Must stay signal-safe.
	static void write_partial_signal_safe(int p_fd, int p_signal, void *const *p_frames, int p_frame_count, const FaultInfo *p_fault = nullptr);

	// Phase 2: scan the report dir for *.partial, build full JSON *.json reports,
	// then delete the consumed .partial files. Returns the list of json paths
	// (global user:// form) that are now ready to upload.
	static Vector<String> promote_partials();

	// Build a complete synthetic report immediately (used by force_write_test_report).
	// Returns the global user:// path of the written .json, or empty on failure.
	static String write_test_report(const String &p_note);

	// Bundle a single report .json (global user:// path) plus a copy of the log
	// file into a sibling .zip the user can send manually. Returns the .zip path
	// (global user:// form) or empty on failure.
	static String zip_report(const String &p_report_json_path);

	// Cache process metadata that the signal handler will need, while it is still
	// safe to call into the engine. Must be called from initialize().
	static void cache_static_metadata();

	// Stash a GDScript backtrace string captured during NOTIFICATION_CRASH so the
	// next-launch promotion can fold it into the matching report. Safe-ish: called
	// from the notification path, not a raw signal.
	static void set_pending_script_backtrace(const String &p_text);

	// Copy the current log to user://crash_reports/pending.log while the crashing
	// process is still alive. Promotion runs on the NEXT launch, by which point
	// the engine's RotatedFileLogger has already truncated or rotated the live
	// log, so reading it then yields nothing. Called from NOTIFICATION_CRASH.
	static void snapshot_log();
};

#endif // CRASH_CATCH_REPORT_H
