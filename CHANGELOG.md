# Changelog

All notable changes to Agent98 are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-11-12

First public build. Runs on Windows 95 OSR2 / 98 / 98SE / Me.

### Added

- Native Win32 GUI (`agent98.exe`) and console client (`a98.exe`) sharing one
  `agent98.ini`.
- OpenAI-compatible Chat Completions client with streaming (SSE) and
  non-streaming modes, implemented on BearSSL 0.6 (TLS 1.2) with bundled
  Mozilla trust anchors.
- Local tools: `read_file`, `write_file`, `list_dir`, `search_files`,
  `run_command`, with a confirmation dialog for writes and shell commands.
- Provider presets (OpenAI, DeepSeek, Moonshot/Kimi, SiliconFlow, Ollama,
  Custom), custom API base/key/model, system prompt, temperature, extra
  headers and a "Test connection" button in `Tools -> Settings...`.
- Automatic runtime environment block injected ahead of the system prompt, so
  the model knows it is talking to a Win9x machine with 16-bit `COMMAND.COM`.
- Session save/load and a configurable workspace sandbox.
- TLS probe utility (`tlsprobe.exe`).
- Mock OpenAI server and PowerShell smoke tests for the GUI, the settings
  dialog and the local tools.

### Fixed

- GUI worker thread is now created with `_beginthreadex()` and the project is
  built against the multithreaded OpenWatcom runtime (`-bm`); plain
  `CreateThread()` left the CRT per-thread data uninitialised and corrupted
  the heap.
- Executables are linked with an explicit image base (`0x400000`), a stripped
  relocation table and image version 4.0, and ship a 256 KB stack, so the
  Win9x PE loader accepts them.
- Both entry points open `agent98.log` as their first action, before the
  configuration is read, so a crash during startup still leaves a trace.
- `build_ow.cmd clean` no longer fails with `Error(E15)` and now really removes
  `build\ow\obj`.

### Notes

- Ship the binaries to a writable local disk. Running them from a read-only
  directory (CD-ROM, network share) prevents the log, configuration and
  workspace from being created.
- Windows 95 requires Winsock 2 (`WS2_32.DLL`); Windows 98 and Me ship it.
