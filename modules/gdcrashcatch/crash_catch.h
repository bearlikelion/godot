/**************************************************************************/
/*  crash_catch.h                                                         */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#ifndef CRASH_CATCH_H
#define CRASH_CATCH_H

#include "core/object/object.h"
#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

// CrashCatch singleton, exposed to GDScript as the global `CrashCatch`.
//
// Responsibilities:
//   - own the engine's crash signal/exception handlers (see CrashCatchHandlers),
//   - promote and upload any reports left over from a previous crashed run,
//   - expose a configuration + test API to GDScript.
//
// The heavy lifting lives in the helper translation units (handlers / report /
// uploader); this class is the bound facade and policy layer.
class CrashCatch : public Object {
	GDCLASS(CrashCatch, Object);

	static CrashCatch *singleton;

	bool initialized = false;
	bool handlers_active = false;

protected:
	static void _bind_methods();

public:
	static CrashCatch *get_singleton();

	// Wires up settings-driven state, installs handlers, and kicks off deferred
	// upload of any reports from a previous crash. Called from register_types.
	void initialize();
	void shutdown();

	// Invoked by the MainLoop NOTIFICATION_CRASH bridge (richer, non-signal-safe
	// capture path). Internal; not bound.
	void _on_engine_crash_notification();

	// ----- Bound configuration / control API -----
	void set_enabled(bool p_enabled);
	bool is_enabled() const;

	void set_upload_url(const String &p_url);
	String get_upload_url() const;

	void set_app_version(const String &p_version);
	String get_app_version() const;

	void set_user_note(const String &p_note);
	String get_user_note() const;

	String get_crash_report_dir() const;
	PackedStringArray get_pending_reports() const;
	int flush_pending_uploads();

	// Manual-send support.
	String get_last_crash_zip() const;
	PackedStringArray get_pending_zips() const;
	// Zip a specific report .json on demand; returns the .zip path (or empty).
	String zip_report(const String &p_report_json_path);

	// ----- Bound test API (guarded by crash_catch/debug/enable_test_api) -----
	void crash_here();
	void crash_with_signal(int p_signal);
	void crash_via_abort();
	// Always available: exercises the writer + upload path without crashing.
	String force_write_test_report();

	// Emitted by the uploader / capture paths.
	void _emit_crash_captured(const String &p_path);
	void _emit_report_uploaded(const String &p_path, int p_code);
	void _emit_upload_failed(const String &p_path, const String &p_error);
	void _emit_crash_zip_ready(const String &p_zip_path);

	CrashCatch();
	~CrashCatch();
};

#endif // CRASH_CATCH_H
