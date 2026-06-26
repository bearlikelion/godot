/**************************************************************************/
/*  crash_catch_project_settings.cpp                                      */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#include "crash_catch_project_settings.h"

#include "core/config/project_settings.h"

void CrashCatchProjectSettings::register_settings() {
	GLOBAL_DEF(PropertyInfo(Variant::BOOL, "crash_catch/enabled"), true);

	GLOBAL_DEF(PropertyInfo(Variant::BOOL, "crash_catch/upload/enabled"), false);
	GLOBAL_DEF(PropertyInfo(Variant::BOOL, "crash_catch/upload/require_consent"), true);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "crash_catch/upload/url"), "");
	// Shared secret sent as a header to the private analyzer service.
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "crash_catch/upload/secret"), "");

	GLOBAL_DEF(PropertyInfo(Variant::STRING, "crash_catch/report/app_version"), "");
	GLOBAL_DEF(PropertyInfo(Variant::INT, "crash_catch/report/log_tail_lines", PROPERTY_HINT_RANGE, "0,2000,1"), 200);

	// Manual-send path: bundle each promoted report into a zip the user can send.
	GLOBAL_DEF(PropertyInfo(Variant::BOOL, "crash_catch/manual/zip_reports"), true);
	// Contact (email or URL) surfaced to the player so they know where to send the zip.
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "crash_catch/manual/contact"), "");

	// Off by default so a shipped game cannot be trivially crashed through the
	// test API. force_write_test_report() stays available regardless.
	GLOBAL_DEF(PropertyInfo(Variant::BOOL, "crash_catch/debug/enable_test_api"), false);
}

bool CrashCatchProjectSettings::get_enabled() {
	return GLOBAL_GET("crash_catch/enabled");
}

bool CrashCatchProjectSettings::get_upload_enabled() {
	return GLOBAL_GET("crash_catch/upload/enabled");
}

bool CrashCatchProjectSettings::get_require_consent() {
	return GLOBAL_GET("crash_catch/upload/require_consent");
}

String CrashCatchProjectSettings::get_upload_url() {
	return GLOBAL_GET("crash_catch/upload/url");
}

String CrashCatchProjectSettings::get_upload_secret() {
	return GLOBAL_GET("crash_catch/upload/secret");
}

String CrashCatchProjectSettings::get_app_version() {
	return GLOBAL_GET("crash_catch/report/app_version");
}

int CrashCatchProjectSettings::get_log_tail_lines() {
	return GLOBAL_GET("crash_catch/report/log_tail_lines");
}

bool CrashCatchProjectSettings::get_test_api_enabled() {
	return GLOBAL_GET("crash_catch/debug/enable_test_api");
}

bool CrashCatchProjectSettings::get_zip_reports() {
	return GLOBAL_GET("crash_catch/manual/zip_reports");
}

String CrashCatchProjectSettings::get_contact() {
	return GLOBAL_GET("crash_catch/manual/contact");
}
