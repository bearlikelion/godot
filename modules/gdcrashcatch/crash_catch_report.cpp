/**************************************************************************/
/*  crash_catch_report.cpp                                                */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#include "crash_catch_report.h"

#include "crash_catch_project_settings.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/print_string.h"
#include "core/variant/dictionary.h"
#include "core/version.h"

#include "modules/modules_enabled.gen.h" // For zip.

#ifdef MODULE_ZIP_ENABLED
#include "modules/zip/zip_packer.h"
#endif

#include <cstring>

#if defined(__linux__)
#include <link.h>
#endif

// ---------------------------------------------------------------------------
// Cached metadata (filled at init while engine calls are safe, then read from
// the signal handler as raw bytes).
// ---------------------------------------------------------------------------
static char s_build_id[128] = { 0 }; // hex GNU build-id, or engine hash fallback
static char s_engine_version[128] = { 0 };
static char s_app_version[128] = { 0 };
static char s_platform[32] = { 0 };
static uint64_t s_static_memory_at_init = 0;
// PIE load base / relocation. Subtracting this from a runtime frame address
// yields the module-relative address that addr2line expects. Captured at init.
static uint64_t s_load_base = 0;
static String s_pending_script_backtrace;

static const char *CC_REPORT_DIR_GLOBAL = "user://crash_reports";

// ---------------------------------------------------------------------------
// Build ID extraction
// ---------------------------------------------------------------------------
#if defined(__linux__)
static int cc_phdr_callback(struct dl_phdr_info *info, size_t size, void *data) {
	// Only the main program (empty name or first entry).
	if (info->dlpi_name && info->dlpi_name[0] != '\0') {
		return 0; // keep looking for the main object (first call has empty name)
	}
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
		if (phdr.p_type != PT_NOTE) {
			continue;
		}
		const char *base = (const char *)(info->dlpi_addr + phdr.p_vaddr);
		size_t offset = 0;
		while (offset + sizeof(ElfW(Nhdr)) <= phdr.p_memsz) {
			const ElfW(Nhdr) *nhdr = (const ElfW(Nhdr) *)(base + offset);
			const char *name = base + offset + sizeof(ElfW(Nhdr));
			const char *desc = name + ((nhdr->n_namesz + 3) & ~3);
			if (nhdr->n_type == NT_GNU_BUILD_ID && nhdr->n_namesz == 4 && memcmp(name, "GNU", 3) == 0) {
				char *out = (char *)data;
				int w = 0;
				for (unsigned int b = 0; b < nhdr->n_descsz && w < 120; b++) {
					static const char hexd[] = "0123456789abcdef";
					unsigned char byte = (unsigned char)desc[b];
					out[w++] = hexd[byte >> 4];
					out[w++] = hexd[byte & 0xF];
				}
				out[w] = 0;
				return 1; // found
			}
			offset += sizeof(ElfW(Nhdr)) + ((nhdr->n_namesz + 3) & ~3) + ((nhdr->n_descsz + 3) & ~3);
		}
	}
	return 0;
}
#endif

static void cc_capture_build_id(char *dst, size_t dst_size) {
	dst[0] = 0;
#if defined(__linux__)
	dl_iterate_phdr(cc_phdr_callback, dst);
#endif
	if (dst[0] == 0) {
		// Fallback: engine version hash (still useful to group by build).
		const char *hash = GODOT_VERSION_HASH;
		if (hash && hash[0]) {
			strncpy(dst, hash, dst_size - 1);
			dst[dst_size - 1] = 0;
		} else {
			strncpy(dst, "unknown", dst_size - 1);
			dst[dst_size - 1] = 0;
		}
	}
}

static void cc_copy_cstr(char *dst, size_t dst_size, const String &p_src) {
	CharString cs = p_src.utf8();
	size_t n = MIN((size_t)cs.length(), dst_size - 1);
	memcpy(dst, cs.get_data(), n);
	dst[n] = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

String CrashCatchReport::get_report_dir_global() {
	return CC_REPORT_DIR_GLOBAL;
}

String CrashCatchReport::get_report_dir() {
	return OS::get_singleton()->get_user_data_dir().path_join("crash_reports");
}

bool CrashCatchReport::ensure_report_dir() {
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_USERDATA);
	if (da.is_null()) {
		return false;
	}
	if (da->dir_exists(CC_REPORT_DIR_GLOBAL)) {
		return true;
	}
	return da->make_dir_recursive(CC_REPORT_DIR_GLOBAL) == OK;
}

void CrashCatchReport::cache_static_metadata() {
	cc_capture_build_id(s_build_id, sizeof(s_build_id));
#if defined(__linux__) && defined(__GLIBC__)
	// Same relocation the engine's crash handler uses (crash_handler_linuxbsd.cpp).
	s_load_base = (uint64_t)_r_debug.r_map->l_addr;
#endif
	cc_copy_cstr(s_engine_version, sizeof(s_engine_version), String(GODOT_VERSION_FULL_NAME));
	cc_copy_cstr(s_app_version, sizeof(s_app_version), CrashCatchProjectSettings::get_app_version());
	cc_copy_cstr(s_platform, sizeof(s_platform), OS::get_singleton()->get_name());
	s_static_memory_at_init = OS::get_singleton()->get_static_memory_usage();
}

void CrashCatchReport::set_pending_script_backtrace(const String &p_text) {
	s_pending_script_backtrace = p_text;
}

// ---------------------------------------------------------------------------
// Phase 1: signal-safe partial writer
// ---------------------------------------------------------------------------

#if defined(UNIX_ENABLED) || defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#define CC_RAW_WRITE(fd, buf, len) (void)write((fd), (buf), (len))
#elif defined(WINDOWS_ENABLED) || defined(_WIN32)
#include <io.h>
#define CC_RAW_WRITE(fd, buf, len) (void)_write((fd), (buf), (unsigned int)(len))
#else
#define CC_RAW_WRITE(fd, buf, len) ((void)0)
#endif

static void cc_write_str(int fd, const char *s) {
	if (!s) {
		return;
	}
	CC_RAW_WRITE(fd, s, strlen(s));
}

static void cc_write_u64_hex(int fd, uint64_t v) {
	char buf[19];
	buf[0] = '0';
	buf[1] = 'x';
	static const char hexd[] = "0123456789abcdef";
	for (int i = 0; i < 16; i++) {
		buf[2 + i] = hexd[(v >> ((15 - i) * 4)) & 0xF];
	}
	CC_RAW_WRITE(fd, buf, 18);
}

static void cc_write_i64(int fd, int64_t v) {
	char buf[24];
	int n = 0;
	bool neg = v < 0;
	uint64_t uv = neg ? (uint64_t)(-v) : (uint64_t)v;
	char tmp[24];
	int t = 0;
	if (uv == 0) {
		tmp[t++] = '0';
	}
	while (uv > 0) {
		tmp[t++] = (char)('0' + (uv % 10));
		uv /= 10;
	}
	if (neg) {
		buf[n++] = '-';
	}
	for (int i = 0; i < t; i++) {
		buf[n++] = tmp[t - 1 - i];
	}
	CC_RAW_WRITE(fd, buf, n);
}

// The partial format is a tiny line-based text record (NOT JSON), chosen so the
// writer can stay async-signal-safe. The next-launch promotion step parses it.
void CrashCatchReport::write_partial_signal_safe(int p_fd, int p_signal, void *const *p_frames, int p_frame_count) {
	cc_write_str(p_fd, "CCPARTIAL1\n");
	cc_write_str(p_fd, "build_id=");
	cc_write_str(p_fd, s_build_id);
	cc_write_str(p_fd, "\nengine=");
	cc_write_str(p_fd, s_engine_version);
	cc_write_str(p_fd, "\napp=");
	cc_write_str(p_fd, s_app_version);
	cc_write_str(p_fd, "\nplatform=");
	cc_write_str(p_fd, s_platform);
	cc_write_str(p_fd, "\nsignal=");
	cc_write_i64(p_fd, p_signal);
	cc_write_str(p_fd, "\nstatic_memory=");
	cc_write_i64(p_fd, (int64_t)s_static_memory_at_init);
	cc_write_str(p_fd, "\nload_base=");
	cc_write_u64_hex(p_fd, s_load_base);
	cc_write_str(p_fd, "\nframes=");
	cc_write_i64(p_fd, p_frame_count);
	cc_write_str(p_fd, "\n");
	for (int i = 0; i < p_frame_count; i++) {
		cc_write_str(p_fd, "frame=");
		cc_write_u64_hex(p_fd, (uint64_t)(uintptr_t)p_frames[i]);
		cc_write_str(p_fd, "\n");
	}
	cc_write_str(p_fd, "END\n");
}

// ---------------------------------------------------------------------------
// Phase 2: promotion + enrichment (safe context)
// ---------------------------------------------------------------------------

static String cc_read_log_tail(int p_lines) {
	if (p_lines <= 0) {
		return String();
	}
	String log_path = GLOBAL_GET("debug/file_logging/log_path");
	if (log_path.is_empty()) {
		return String();
	}
	String full = OS::get_singleton()->expand_path(log_path);
	if (!FileAccess::exists(full)) {
		return String();
	}
	Ref<FileAccess> f = FileAccess::open(full, FileAccess::READ);
	if (f.is_null()) {
		return String();
	}
	// Read the whole file then keep the last N lines. Logs are small; simple wins.
	Vector<String> lines;
	while (!f->eof_reached()) {
		lines.push_back(f->get_line());
	}
	int start = MAX(0, (int)lines.size() - p_lines);
	String out;
	for (int i = start; i < lines.size(); i++) {
		out += lines[i];
		out += "\n";
	}
	return out;
}

static Dictionary cc_collect_system_info() {
	Dictionary sys;
	OS *os = OS::get_singleton();
	sys["os_name"] = os->get_name();
	sys["os_version"] = os->get_version();
	sys["distribution"] = os->get_distribution_name();
	sys["model"] = os->get_model_name();
	sys["locale"] = os->get_locale();
	sys["processor"] = os->get_processor_name();
	sys["processor_count"] = os->get_processor_count();
	return sys;
}

// Parse a CCPARTIAL1 record into a JSON-ready Dictionary, adding enrichment.
static Dictionary cc_parse_partial(const String &p_text) {
	Dictionary d;
	Array frames;
	Vector<String> lines = p_text.split("\n", false);
	for (const String &line : lines) {
		int eq = line.find_char('=');
		if (eq < 0) {
			continue;
		}
		String key = line.substr(0, eq);
		String val = line.substr(eq + 1);
		if (key == "frame") {
			frames.push_back(val);
		} else if (key == "signal" || key == "static_memory" || key == "frames") {
			d[key] = val.to_int();
		} else {
			d[key] = val;
		}
	}
	d["native_frames"] = frames;
	return d;
}

Vector<String> CrashCatchReport::promote_partials() {
	Vector<String> produced;
	ensure_report_dir();

	Ref<DirAccess> da = DirAccess::open(CC_REPORT_DIR_GLOBAL);
	if (da.is_null()) {
		return produced;
	}

	Vector<String> partials;
	da->list_dir_begin();
	String name = da->get_next();
	while (!name.is_empty()) {
		if (!da->current_is_dir() && name.ends_with(".partial")) {
			partials.push_back(name);
		}
		name = da->get_next();
	}
	da->list_dir_end();

	for (const String &pname : partials) {
		String partial_global = String(CC_REPORT_DIR_GLOBAL).path_join(pname);
		Ref<FileAccess> f = FileAccess::open(partial_global, FileAccess::READ);
		if (f.is_null()) {
			continue;
		}
		String text = f->get_as_text();
		f.unref();

		Dictionary report = cc_parse_partial(text);
		report["schema"] = "gdcrashcatch/1";
		report["log_tail"] = cc_read_log_tail(CrashCatchProjectSettings::get_log_tail_lines());
		report["system"] = cc_collect_system_info();
		report["created_at"] = Time::get_singleton()->get_datetime_string_from_system(true);
		if (!s_pending_script_backtrace.is_empty()) {
			report["gdscript_backtrace"] = s_pending_script_backtrace;
		}
		if (!s_app_version[0]) {
			report["app"] = CrashCatchProjectSettings::get_app_version();
		}

		String json = JSON::stringify(report, "  ");
		String json_global = partial_global.trim_suffix(".partial") + ".json";
		Ref<FileAccess> wf = FileAccess::open(json_global, FileAccess::WRITE);
		if (wf.is_valid()) {
			wf->store_string(json);
			wf.unref();
			produced.push_back(json_global);
			// Optionally bundle a sendable zip (report + log) alongside the report.
			if (CrashCatchProjectSettings::get_zip_reports()) {
				(void)zip_report(json_global);
			}
			// Consume the partial now that it has been promoted.
			da->remove(pname);
		}
	}

	s_pending_script_backtrace = String();
	return produced;
}

String CrashCatchReport::write_test_report(const String &p_note) {
	ensure_report_dir();
	Dictionary report;
	report["schema"] = "gdcrashcatch/1";
	report["build_id"] = String(s_build_id);
	report["engine"] = String(s_engine_version);
	report["app"] = CrashCatchProjectSettings::get_app_version();
	report["platform"] = String(s_platform);
	report["signal"] = 0;
	report["test"] = true;
	report["load_base"] = vformat("0x%016x", s_load_base);
	report["native_frames"] = Array();
	report["static_memory"] = (int64_t)OS::get_singleton()->get_static_memory_usage();
	report["log_tail"] = cc_read_log_tail(CrashCatchProjectSettings::get_log_tail_lines());
	report["system"] = cc_collect_system_info();
	report["user_note"] = p_note;
	report["created_at"] = Time::get_singleton()->get_datetime_string_from_system(true);

	String fname = vformat("crash-test-%d.json", (int)Time::get_singleton()->get_unix_time_from_system());
	String json_global = String(CC_REPORT_DIR_GLOBAL).path_join(fname);
	Ref<FileAccess> wf = FileAccess::open(json_global, FileAccess::WRITE);
	if (wf.is_null()) {
		return String();
	}
	wf->store_string(JSON::stringify(report, "  "));
	wf.unref();
	return json_global;
}

String CrashCatchReport::zip_report(const String &p_report_json_path) {
#ifdef MODULE_ZIP_ENABLED
	if (!FileAccess::exists(p_report_json_path)) {
		return String();
	}

	String zip_global = p_report_json_path.trim_suffix(".json") + ".zip";

	Ref<ZIPPacker> zip;
	zip.instantiate();
	if (zip->open(zip_global, ZIPPacker::APPEND_CREATE) != OK) {
		return String();
	}

	// 1) The enriched crash report.
	{
		Ref<FileAccess> rf = FileAccess::open(p_report_json_path, FileAccess::READ);
		if (rf.is_valid()) {
			Vector<uint8_t> data = rf->get_buffer(rf->get_length());
			zip->start_file("report.json");
			zip->write_file(data);
			zip->close_file();
		}
	}

	// 2) A copy of the current/last log file, if available.
	{
		String log_path = GLOBAL_GET("debug/file_logging/log_path");
		if (!log_path.is_empty()) {
			String full = OS::get_singleton()->expand_path(log_path);
			if (FileAccess::exists(full)) {
				Ref<FileAccess> lf = FileAccess::open(full, FileAccess::READ);
				if (lf.is_valid()) {
					Vector<uint8_t> data = lf->get_buffer(lf->get_length());
					zip->start_file(full.get_file());
					zip->write_file(data);
					zip->close_file();
				}
			}
		}
	}

	// 3) A short READ_ME telling the player where to send the zip.
	{
		String contact = CrashCatchProjectSettings::get_contact();
		String readme = "This archive contains a crash report for the game.\n";
		if (!contact.is_empty()) {
			readme += "Please send this .zip file to: " + contact + "\n";
		} else {
			readme += "Please send this .zip file to the game's developer.\n";
		}
		readme += "It contains a crash report (report.json) and the game log.\n";
		CharString cs = readme.utf8();
		Vector<uint8_t> data;
		data.resize(cs.length());
		memcpy(data.ptrw(), cs.get_data(), cs.length());
		zip->start_file("READ_ME.txt");
		zip->write_file(data);
		zip->close_file();
	}

	zip->close();
	return zip_global;
#else
	return String();
#endif
}
