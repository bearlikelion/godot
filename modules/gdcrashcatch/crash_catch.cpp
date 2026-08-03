/**************************************************************************/
/*  crash_catch.cpp                                                       */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#include "crash_catch.h"

#include "crash_catch_handlers.h"
#include "crash_catch_project_settings.h"
#include "crash_catch_report.h"
#include "crash_catch_uploader.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/string/print_string.h"

#include <csignal>
#include <cstdlib>

CrashCatch *CrashCatch::singleton = nullptr;

// Most recently produced sendable crash zip (global user:// path).
static String s_last_zip;

CrashCatch *CrashCatch::get_singleton() {
	return singleton;
}

CrashCatch::CrashCatch() {
	ERR_FAIL_COND_MSG(singleton != nullptr, "CrashCatch singleton already exists.");
	singleton = this;
}

CrashCatch::~CrashCatch() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void CrashCatch::initialize() {
	if (initialized) {
		return;
	}
	initialized = true;

	if (!CrashCatchProjectSettings::get_enabled()) {
		return;
	}

	// Cache everything the signal handler will need while it is still safe to
	// allocate / call into the engine.
	CrashCatchReport::ensure_report_dir();
	CrashCatchReport::cache_static_metadata();

	CrashCatchHandlers::install();
	handlers_active = CrashCatchHandlers::is_installed();

	// Promote any reports left behind by a previous crashed run, then try to
	// upload them. This is the deferred-upload path; it never runs in a handler.
	Vector<String> promoted = CrashCatchReport::promote_partials();
	for (const String &p : promoted) {
		_emit_crash_captured(p);
		// promote_partials() already wrote a sibling .zip when zip_reports is on.
		if (CrashCatchProjectSettings::get_zip_reports()) {
			String zip = p.trim_suffix(".json") + ".zip";
			if (FileAccess::exists(zip)) {
				s_last_zip = zip;
				_emit_crash_zip_ready(zip);
			}
		}
	}

	if (CrashCatchProjectSettings::get_upload_enabled()) {
		CrashCatchUploader::flush(this);
	}
}

void CrashCatch::shutdown() {
	if (handlers_active) {
		CrashCatchHandlers::uninstall();
		handlers_active = false;
	}
	initialized = false;
}

void CrashCatch::_on_engine_crash_notification() {
	// Richer, non-signal-safe capture: fold the GDScript backtrace into the
	// report that the (already-fired) signal handler will/has written. We stash
	// it for the next-launch promotion to pick up.
	// (Defined in crash_catch_report.cpp; kept out of the raw signal path.)
	CrashCatchReport::set_pending_script_backtrace(String());
	// Copy the log now: by the next launch the engine's logger has rotated it.
	CrashCatchReport::snapshot_log();
}

// ----------------------------------------------------------------------------
// Configuration / control
// ----------------------------------------------------------------------------

void CrashCatch::set_enabled(bool p_enabled) {
	if (p_enabled == is_enabled()) {
		return;
	}
	ProjectSettings::get_singleton()->set_setting("crash_catch/enabled", p_enabled);
	if (p_enabled && !handlers_active) {
		CrashCatchHandlers::install();
		handlers_active = CrashCatchHandlers::is_installed();
	} else if (!p_enabled && handlers_active) {
		CrashCatchHandlers::uninstall();
		handlers_active = false;
	}
}

bool CrashCatch::is_enabled() const {
	return CrashCatchProjectSettings::get_enabled();
}

void CrashCatch::set_upload_url(const String &p_url) {
	ProjectSettings::get_singleton()->set_setting("crash_catch/upload/url", p_url);
}

String CrashCatch::get_upload_url() const {
	return CrashCatchProjectSettings::get_upload_url();
}

void CrashCatch::set_app_version(const String &p_version) {
	ProjectSettings::get_singleton()->set_setting("crash_catch/report/app_version", p_version);
}

String CrashCatch::get_app_version() const {
	return CrashCatchProjectSettings::get_app_version();
}

static String s_user_note;

void CrashCatch::set_user_note(const String &p_note) {
	s_user_note = p_note;
}

String CrashCatch::get_user_note() const {
	return s_user_note;
}

String CrashCatch::get_crash_report_dir() const {
	return CrashCatchReport::get_report_dir_global();
}

PackedStringArray CrashCatch::get_pending_reports() const {
	PackedStringArray out;
	for (const String &p : CrashCatchUploader::list_pending()) {
		out.push_back(p);
	}
	return out;
}

int CrashCatch::flush_pending_uploads() {
	return CrashCatchUploader::flush(this);
}

String CrashCatch::get_last_crash_zip() const {
	return s_last_zip;
}

PackedStringArray CrashCatch::get_pending_zips() const {
	PackedStringArray out;
	Ref<DirAccess> da = DirAccess::open(CrashCatchReport::get_report_dir_global());
	if (da.is_null()) {
		return out;
	}
	da->list_dir_begin();
	String name = da->get_next();
	while (!name.is_empty()) {
		if (!da->current_is_dir() && name.ends_with(".zip")) {
			out.push_back(CrashCatchReport::get_report_dir_global().path_join(name));
		}
		name = da->get_next();
	}
	da->list_dir_end();
	return out;
}

String CrashCatch::zip_report(const String &p_report_json_path) {
	String zip = CrashCatchReport::zip_report(p_report_json_path);
	if (!zip.is_empty()) {
		s_last_zip = zip;
		_emit_crash_zip_ready(zip);
	}
	return zip;
}

// ----------------------------------------------------------------------------
// Test API
// ----------------------------------------------------------------------------

void CrashCatch::crash_here() {
	if (!CrashCatchProjectSettings::get_test_api_enabled()) {
		ERR_FAIL_MSG("CrashCatch.crash_here() is disabled. Enable 'crash_catch/debug/enable_test_api' to use it.");
	}
	WARN_PRINT("CrashCatch: intentionally dereferencing a null pointer (test API).");
	volatile int *p = nullptr;
	*p = 0xDEAD; // Triggers SIGSEGV.
}

void CrashCatch::crash_with_signal(int p_signal) {
	if (!CrashCatchProjectSettings::get_test_api_enabled()) {
		ERR_FAIL_MSG("CrashCatch.crash_with_signal() is disabled. Enable 'crash_catch/debug/enable_test_api' to use it.");
	}
	WARN_PRINT(vformat("CrashCatch: raising signal %d (test API).", p_signal));
	raise(p_signal);
}

void CrashCatch::crash_via_abort() {
	if (!CrashCatchProjectSettings::get_test_api_enabled()) {
		ERR_FAIL_MSG("CrashCatch.crash_via_abort() is disabled. Enable 'crash_catch/debug/enable_test_api' to use it.");
	}
	WARN_PRINT("CrashCatch: calling abort() (test API).");
	abort();
}

String CrashCatch::force_write_test_report() {
	// Always available: writes a complete synthetic report so the upload pipeline
	// can be exercised end-to-end without crashing the process.
	String path = CrashCatchReport::write_test_report(get_user_note());
	if (!path.is_empty()) {
		_emit_crash_captured(path);
		if (CrashCatchProjectSettings::get_zip_reports()) {
			(void)zip_report(path);
		}
		if (CrashCatchProjectSettings::get_upload_enabled()) {
			CrashCatchUploader::flush(this);
		}
	}
	return path;
}

// ----------------------------------------------------------------------------
// Signal emit helpers
// ----------------------------------------------------------------------------

void CrashCatch::_emit_crash_captured(const String &p_path) {
	emit_signal("crash_captured", p_path);
}

void CrashCatch::_emit_report_uploaded(const String &p_path, int p_code) {
	emit_signal("report_uploaded", p_path, p_code);
}

void CrashCatch::_emit_upload_failed(const String &p_path, const String &p_error) {
	emit_signal("upload_failed", p_path, p_error);
}

void CrashCatch::_emit_crash_zip_ready(const String &p_zip_path) {
	emit_signal("crash_zip_ready", p_zip_path);
}

// ----------------------------------------------------------------------------
// Bindings
// ----------------------------------------------------------------------------

void CrashCatch::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &CrashCatch::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &CrashCatch::is_enabled);

	ClassDB::bind_method(D_METHOD("set_upload_url", "url"), &CrashCatch::set_upload_url);
	ClassDB::bind_method(D_METHOD("get_upload_url"), &CrashCatch::get_upload_url);

	ClassDB::bind_method(D_METHOD("set_app_version", "version"), &CrashCatch::set_app_version);
	ClassDB::bind_method(D_METHOD("get_app_version"), &CrashCatch::get_app_version);

	ClassDB::bind_method(D_METHOD("set_user_note", "note"), &CrashCatch::set_user_note);
	ClassDB::bind_method(D_METHOD("get_user_note"), &CrashCatch::get_user_note);

	ClassDB::bind_method(D_METHOD("get_crash_report_dir"), &CrashCatch::get_crash_report_dir);
	ClassDB::bind_method(D_METHOD("get_pending_reports"), &CrashCatch::get_pending_reports);
	ClassDB::bind_method(D_METHOD("flush_pending_uploads"), &CrashCatch::flush_pending_uploads);

	ClassDB::bind_method(D_METHOD("get_last_crash_zip"), &CrashCatch::get_last_crash_zip);
	ClassDB::bind_method(D_METHOD("get_pending_zips"), &CrashCatch::get_pending_zips);
	ClassDB::bind_method(D_METHOD("zip_report", "report_json_path"), &CrashCatch::zip_report);

	ClassDB::bind_method(D_METHOD("crash_here"), &CrashCatch::crash_here);
	ClassDB::bind_method(D_METHOD("crash_with_signal", "signal"), &CrashCatch::crash_with_signal);
	ClassDB::bind_method(D_METHOD("crash_via_abort"), &CrashCatch::crash_via_abort);
	ClassDB::bind_method(D_METHOD("force_write_test_report"), &CrashCatch::force_write_test_report);

	ADD_SIGNAL(MethodInfo("crash_captured", PropertyInfo(Variant::STRING, "report_path")));
	ADD_SIGNAL(MethodInfo("report_uploaded", PropertyInfo(Variant::STRING, "report_path"), PropertyInfo(Variant::INT, "response_code")));
	ADD_SIGNAL(MethodInfo("upload_failed", PropertyInfo(Variant::STRING, "report_path"), PropertyInfo(Variant::STRING, "error")));
	ADD_SIGNAL(MethodInfo("crash_zip_ready", PropertyInfo(Variant::STRING, "zip_path")));
}
