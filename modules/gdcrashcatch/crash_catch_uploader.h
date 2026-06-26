/**************************************************************************/
/*  crash_catch_uploader.h                                                */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#ifndef CRASH_CATCH_UPLOADER_H
#define CRASH_CATCH_UPLOADER_H

#include "core/string/ustring.h"
#include "core/templates/vector.h"

class CrashCatch;

// Deferred uploader. Never runs inside a signal handler: it is driven from the
// scene/main-loop context on the next launch after a crash. POSTs each completed
// JSON report to the configured analyzer endpoint using the low-level HTTPClient
// (usable off the scene tree), then renames sent files to *.sent.
class CrashCatchUploader {
public:
	// Returns the list of completed (not-yet-sent) report paths in global form.
	static Vector<String> list_pending();

	// Attempts to upload every pending report. Emits report_uploaded /
	// upload_failed signals through the supplied singleton. Returns the count
	// successfully uploaded. Respects upload-enabled / consent settings.
	static int flush(CrashCatch *p_owner);

	// Upload a single report file (global user:// path). Returns the HTTP
	// response code, or a negative Error on transport failure.
	static int upload_one(const String &p_report_path, String *r_error = nullptr);

	// Cheap build_id field extraction from a JSON report body (for the header).
	static String _extract_build_id(const String &p_json_body);
};

#endif // CRASH_CATCH_UPLOADER_H
