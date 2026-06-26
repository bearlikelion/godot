# GDCrashCatch

A Godot Engine C++ module that captures crashes gracefully, writes a structured crash
report (native stack, system info, log tail), and optionally uploads it on next launch to a
private analyzer that symbolicates the native stack against the build's debug symbols.

Unlike the engine's built-in crash handler (which is compiled out of `template_release`
exports), GDCrashCatch installs its own signal/exception handlers and therefore also works in
shipped games. It chains to any previously installed handler, so it does not replace the
engine's own debug-build backtrace.

## Features

- Native crash capture on Linux, Windows, and macOS via async-signal-safe handlers.
- Two-phase reporting: a minimal, signal-safe record is written during the crash; it is
  promoted to a full JSON report (log tail, system info) on the next launch.
- Deferred, consent-gated upload to a private endpoint using the low-level `HTTPClient`.
- GNU Build ID captured per build for server-side symbolication.
- GDScript singleton `CrashCatch` with a test API (`crash_here()`, `crash_with_signal()`,
  `crash_via_abort()`, and the non-crashing `force_write_test_report()`).

## Install

Place this directory at `modules/gdcrashcatch/` in the Godot source tree (or add it as a git
submodule) and rebuild the engine.

## Project settings

- `crash_catch/enabled` (bool, default true)
- `crash_catch/upload/enabled` (bool, default false)
- `crash_catch/upload/require_consent` (bool, default true)
- `crash_catch/upload/url` (string)
- `crash_catch/upload/secret` (string) — shared secret header for the private analyzer
- `crash_catch/report/app_version` (string)
- `crash_catch/report/log_tail_lines` (int, default 200)
- `crash_catch/manual/zip_reports` (bool, default true) — bundle each report into a sendable `.zip`
- `crash_catch/manual/contact` (string) — email/URL shown in the zip's `READ_ME.txt`
- `crash_catch/debug/enable_test_api` (bool, default false)

## Manual send

When `crash_catch/manual/zip_reports` is on, each recovered crash is bundled on the next
launch into `user://crash_reports/<name>.zip` containing the report JSON, a copy of the game
log, and a `READ_ME.txt` pointing at `crash_catch/manual/contact`. In your UI, call
`CrashCatch.get_last_crash_zip()` (or `get_pending_zips()`) in `_ready` to detect a pending
crash and prompt the player to email you the zip. `CrashCatch.zip_report(path)` bundles a
specific report on demand and emits `crash_zip_ready`.

## License

MIT. See `LICENSE`.
