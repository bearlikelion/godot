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
- Module load base recorded on all three platforms so absolute frame addresses can be
  rebased against the debug symbols.
- Fault detail (`fault_pc`, `fault_address`, `fault_access`) captured where the platform
  provides it.
- GDScript singleton `CrashCatch` with a test API (`crash_here()`, `crash_with_signal()`,
  `crash_via_abort()`, and the non-crashing `force_write_test_report()`).

## Symbolicating a report

Frames in `native_frames` are absolute runtime addresses. In every case subtract
`load_base` to get the address the debug symbols use:

```
file_addr = frame - load_base
```

`load_base_kind` records what was subtracted. On Linux and macOS (`"slide"`) it is the
relocation slide, so the result is a module-relative offset. On Windows (`"image_base"`)
it is the absolute image base reported by `GetModuleHandleW(nullptr)`, so the result still
carries the PE preferred base (`0x140000000` on x86_64) and lands directly in the section
VMAs that `objdump -h` shows. Do not add the preferred base a second time.

Frames outside the main module (system DLLs, libc) will not resolve; grouping addresses by
their high bits separates them from game frames. Godot's Windows builds are MinGW-built and
carry DWARF, so the GNU tools work directly, with no PDB involved:

```
addr2line -e godot.windows.template_release.x86_64.exe.debugsymbols -f -C -i <file_addr>
```

Pass every address in one invocation; the debug info is large and each run reloads it.

`signal` holds a POSIX signal number when `signal_kind` is `posix_signal`, and an NTSTATUS
exception code (e.g. `0xC0000005` as the signed int `-1073741819`) when it is
`nt_exception`.

## Install

Place this directory at `modules/gdcrashcatch/` in the Godot source tree (or add it as a git
submodule) and rebuild the engine.

## Project settings

- `crash_catch/enabled` (bool, default true)
- `crash_catch/upload/enabled` (bool, default false)
- `crash_catch/upload/require_consent` (bool, default true)
- `crash_catch/upload/url` (string)
- `crash_catch/upload/secret` (string): shared secret header for the private analyzer
- `crash_catch/report/app_version` (string)
- `crash_catch/report/log_tail_lines` (int, default 200)
- `crash_catch/manual/zip_reports` (bool, default true): bundle each report into a sendable `.zip`
- `crash_catch/manual/contact` (string): email/URL shown in the zip's `READ_ME.txt`
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
