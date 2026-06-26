/**************************************************************************/
/*  crash_catch_project_settings.h                                        */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#ifndef CRASH_CATCH_PROJECT_SETTINGS_H
#define CRASH_CATCH_PROJECT_SETTINGS_H

#include "core/string/ustring.h"

// Static accessor wrapper around the module's project settings. Mirrors the
// pattern used by GodotSteam's SteamProjectSettings: register defaults once at
// module init, then read them back through typed getters.
class CrashCatchProjectSettings {
public:
	static void register_settings();

	static bool get_enabled();
	static bool get_upload_enabled();
	static bool get_require_consent();
	static String get_upload_url();
	static String get_upload_secret();
	static String get_app_version();
	static int get_log_tail_lines();
	static bool get_test_api_enabled();
	static bool get_zip_reports();
	static String get_contact();
};

#endif // CRASH_CATCH_PROJECT_SETTINGS_H
