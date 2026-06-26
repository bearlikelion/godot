/**************************************************************************/
/*  crash_catch_uploader.cpp                                              */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#include "crash_catch_uploader.h"

#include "crash_catch.h"
#include "crash_catch_project_settings.h"
#include "crash_catch_report.h"

#include "core/crypto/crypto.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/http_client.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/print_string.h"

Vector<String> CrashCatchUploader::list_pending() {
	Vector<String> pending;
	String dir = CrashCatchReport::get_report_dir_global();
	Ref<DirAccess> da = DirAccess::open(dir);
	if (da.is_null()) {
		return pending;
	}
	da->list_dir_begin();
	String name = da->get_next();
	while (!name.is_empty()) {
		if (!da->current_is_dir() && name.ends_with(".json")) {
			pending.push_back(dir.path_join(name));
		}
		name = da->get_next();
	}
	da->list_dir_end();
	return pending;
}

int CrashCatchUploader::upload_one(const String &p_report_path, String *r_error) {
	String url = CrashCatchProjectSettings::get_upload_url();
	if (url.is_empty()) {
		if (r_error) {
			*r_error = "No upload URL configured.";
		}
		return -1;
	}

	Ref<FileAccess> f = FileAccess::open(p_report_path, FileAccess::READ);
	if (f.is_null()) {
		if (r_error) {
			*r_error = "Cannot read report file.";
		}
		return -1;
	}
	String body = f->get_as_text();
	f.unref();

	// Parse the URL into host / port / path / scheme.
	String scheme = "http://";
	bool use_tls = false;
	String rest = url;
	if (url.begins_with("https://")) {
		scheme = "https://";
		use_tls = true;
		rest = url.substr(8);
	} else if (url.begins_with("http://")) {
		rest = url.substr(7);
	}
	int slash = rest.find_char('/');
	String host_port = slash >= 0 ? rest.substr(0, slash) : rest;
	String req_path = slash >= 0 ? rest.substr(slash) : "/";
	int port = use_tls ? 443 : 80;
	String host = host_port;
	int colon = host_port.find_char(':');
	if (colon >= 0) {
		host = host_port.substr(0, colon);
		port = host_port.substr(colon + 1).to_int();
	}

	Ref<HTTPClient> client = Ref<HTTPClient>(HTTPClient::create());
	if (client.is_null()) {
		if (r_error) {
			*r_error = "Could not create HTTPClient.";
		}
		return -1;
	}

	Ref<TLSOptions> tls = use_tls ? TLSOptions::client() : Ref<TLSOptions>();
	Error err = client->connect_to_host(host, port, tls);
	if (err != OK) {
		if (r_error) {
			*r_error = "connect_to_host failed.";
		}
		return -1;
	}

	// Pump until connected (or fail).
	while (client->get_status() == HTTPClient::STATUS_CONNECTING || client->get_status() == HTTPClient::STATUS_RESOLVING) {
		client->poll();
		OS::get_singleton()->delay_usec(2000);
	}
	if (client->get_status() != HTTPClient::STATUS_CONNECTED) {
		if (r_error) {
			*r_error = "Connection failed.";
		}
		return -1;
	}

	Vector<String> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("X-Build-Id: " + _extract_build_id(body));
	String secret = CrashCatchProjectSettings::get_upload_secret();
	if (!secret.is_empty()) {
		headers.push_back("X-CrashCatch-Secret: " + secret);
	}

	CharString body_utf8 = body.utf8();
	err = client->request(HTTPClient::METHOD_POST, req_path, headers, (const uint8_t *)body_utf8.get_data(), body_utf8.length());
	if (err != OK) {
		if (r_error) {
			*r_error = "request failed.";
		}
		return -1;
	}

	while (client->get_status() == HTTPClient::STATUS_REQUESTING) {
		client->poll();
		OS::get_singleton()->delay_usec(2000);
	}

	if (client->get_status() != HTTPClient::STATUS_BODY && client->get_status() != HTTPClient::STATUS_CONNECTED) {
		if (r_error) {
			*r_error = "No response.";
		}
		return -1;
	}

	int code = client->get_response_code();
	// Drain the body so the connection closes cleanly.
	while (client->get_status() == HTTPClient::STATUS_BODY) {
		client->poll();
		client->read_response_body_chunk();
	}
	client->close();
	return code;
}

int CrashCatchUploader::flush(CrashCatch *p_owner) {
	if (!CrashCatchProjectSettings::get_upload_enabled()) {
		return 0;
	}
	if (CrashCatchProjectSettings::get_require_consent() && CrashCatchProjectSettings::get_upload_url().is_empty()) {
		// No endpoint configured; nothing to do.
		return 0;
	}

	int uploaded = 0;
	for (const String &path : list_pending()) {
		String error;
		int code = upload_one(path, &error);
		if (code >= 200 && code < 300) {
			// Mark as sent by renaming.
			Ref<DirAccess> da = DirAccess::open(CrashCatchReport::get_report_dir_global());
			if (da.is_valid()) {
				da->rename(path.get_file(), path.get_file().trim_suffix(".json") + ".sent");
			}
			if (p_owner) {
				p_owner->_emit_report_uploaded(path, code);
			}
			uploaded++;
		} else {
			if (p_owner) {
				p_owner->_emit_upload_failed(path, error.is_empty() ? vformat("HTTP %d", code) : error);
			}
		}
	}
	return uploaded;
}

String CrashCatchUploader::_extract_build_id(const String &p_json_body) {
	// Cheap extraction without a full parse: look for "build_id":"...".
	int idx = p_json_body.find("\"build_id\"");
	if (idx < 0) {
		return "unknown";
	}
	int colon = p_json_body.find_char(':', idx);
	if (colon < 0) {
		return "unknown";
	}
	int q1 = p_json_body.find_char('"', colon + 1);
	if (q1 < 0) {
		return "unknown";
	}
	int q2 = p_json_body.find_char('"', q1 + 1);
	if (q2 < 0) {
		return "unknown";
	}
	return p_json_body.substr(q1 + 1, q2 - q1 - 1);
}
