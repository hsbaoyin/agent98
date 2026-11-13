# Changelog

All notable changes to Agent98 are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- 26 new tools (31 total): `mkdir`, `delete_file`, `rmdir`, `rmdir_force`,
  `move_file`, `copy_file`, `append_file`, `edit`, `apply_patch`, `touch`,
  `set_attr`, `file_info`, `head`, `tail`, `grep`, `glob`, `tree`, `wc`,
  `which`, `diff`, `checksum`, `disk_info`, `env`, `session_info`,
  `todo_read` and `todo_write`.
- Confirmation summaries with a risk level (`tools_format_confirm`): HIGH RISK
  calls default to Deny in the GUI and require typing `YES` in the CLI.
- CLI: `--ini`, `--workspace`, `--message`, `--yes`, `--no-shell`, and the
  `/tools`, `/stop`, `/export`, `/workspace` and `/model` commands; `/config`
  masks the API key.
- GUI: **Tools -> List tools...** and Esc to stop the current run.
- Tool arguments are redacted (`api_key`, `token`, `password`, ...) before they
  reach the log.

### Fixed

- `rmdir_force` only refused the workspace root when `workspace=` was absolute
  and had no trailing separator. Both paths are normalised now, so a relative
  or trailing-slash workspace can no longer be deleted recursively.
- The GUI confirmation dialog forced focus back to Allow after marking a HIGH
  RISK call. Cancel is now the default button and keeps the focus.
- `env` requires confirmation instead of handing any environment variable
  (including API keys) to the model without a prompt.
- CLI `/export` writes CP_ACP with CRLF like `write_file` and the GUI exporter,
  instead of raw UTF-8 with bare LF.
- The CLI honours `[log] level` instead of forcing DEBUG; the value is clamped
  to 0..4.
- `apply_patch` aborts when the `.bak` backup cannot be written, instead of
  editing without a recovery copy.
- The GUI transcript uses the system CJK charset (`GB2312_CHARSET` on code page
  936, `SHIFTJIS_CHARSET` on 932, `HANGUL_CHARSET` on 949) for the mono font and
  for every inserted run. On Windows 9x/RichEdit 2.0 the first character of a
  Chinese answer used to inherit the previous run's ANSI charset, so GDI
  substituted a font without CJK glyphs and only that character came out as a
  box.

## [0.2.0] - 2026-11-13

First packaged release: the GUI gets the retro transcript, session history and
status bar, file tools learn the Win9x code page, and the agent loop stops
hammering the same tool call.

### Added

- `write_file` converts UTF-8 content to the local ANSI code page (CP_ACP,
  GBK on Simplified Chinese systems) before writing, so text files open
  correctly in Win98/Me Notepad, EDIT and `COMMAND.COM`. Conversion falls
  back to raw UTF-8 when the code page cannot represent a character.
- `read_file` auto-detects the encoding: valid UTF-8 is returned as is (a
  UTF-8 BOM is stripped), anything else is converted from CP_ACP to UTF-8,
  so GBK files created by Win9x applications are readable by the model.
- `write_file` converts bare LF line endings to DOS CRLF for text files
  (`.bat`, `.cmd`, `.ini`, `.txt`, `.asm`, `.c`, `.h` and any unknown
  extension); binaries keep their bytes untouched. Windows Me's 16-bit
  `COMMAND.COM` used to read an LF-only `.BAT` as one huge command line and
  fail with "Bad command or file name".
- `api_chat` retries transient failures up to twice with a 1 s / 2 s backoff:
  connection and TLS read errors, plus HTTP 429, 502, 503 and 504. Retries are
  logged and are skipped once part of the answer has already been displayed,
  so streamed text is never duplicated.
- Retro GUI upgrade: a 9 pt fixed-pitch transcript with semantic colours
  (dark blue `You:`, grey `[round]`, teal `[tool]`, green `[result]`, red
  `[error]`), a History combo box for the auto-saved sessions in `sessions/`,
  auto-save after every completed request, DOSKEY-style `Alt+Up` /
  `Alt+Down` prompt history, CP_ACP/CRLF transcript export
  (`Ctrl+E`), a native status bar with a spinner and round counter, and
  **Tools -> Open workspace** / **Workspace** (ShellExecuteA).

### Fixed

- The console client no longer drops streamed text when a provider splits a
  multi-byte character across two deltas: the partial sequence was overwritten
  with a NUL before it was moved to the front of the buffer, which also lost
  the chunk that followed.
- `write_file` output truncation in the agent loop backs off to a UTF-8
  character boundary. Trimming the last byte of a *complete* multi-byte
  character used to leave an invalid sequence in the request.
- Streaming tool calls whose `name`/`id` are repeated in every delta no longer
  become `list_dirlist_dir` (unknown tool) or a mangled `tool_call_id`
  (HTTP 400 on the following round).
- `run_command` builds its temporary output path with `snprintf()`; the old
  `sprintf()` could overflow a 260-byte stack buffer when TEMP is long.
- A long `system_prompt` in the INI is no longer truncated to 519 bytes, and a
  truncated value is logged instead of being written back silently.
- `config_save()` reports write failures, so a read-only directory (CD-ROM,
  network share) surfaces the error instead of losing settings silently.
- `allow_shell` now defaults to 0, matching `dist/SAMPLE.INI` and this README.
  A fresh install no longer enables shell execution implicitly.
- `--insecure` in the TLS probe and `tls.verify=0` now really skip
  certificate-name matching (self-signed hosts and bare IP addresses work).
  Previously the X.509 engine was replaced with nothing, so the handshake had
  no server key at all.
- Paths reported by the tools keep their on-disk spelling; the sandbox check
  used to lower-case the resolved path before handing it back.
- The TLS probe no longer sends a second `Host` header, and the transport only
  adds its default `Accept` when the caller did not supply one.
- `api_key`/`extra_headers` values are stripped of CR/LF before they go on the
  wire, instead of splitting or injecting headers.
- The confirmation dialog can no longer be abandoned by the worker thread: the
  dialog owns the request struct until it signals, so cancelling while it was
  pending could read freed memory and signal a closed handle.
- `log_write()` is serialised with a critical section and copies the
  `localtime()` result; session timestamps use `GetLocalTime()`.
- `ow/rules.mk` lists the project headers as dependencies, so editing a header
  rebuilds every module that includes it (wmake has no dependency discovery of
  its own).
- The GUI dialog procedures use `INT_PTR CALLBACK` and the 64-bit-safe
  `GWLP_WNDPROC`, so the MinGW/CMake GUI target compiles again.

### Changed

- Default `agent.max_steps` raised from 8 to 32. A value of 0 now means
  unlimited: the run stops only when the model answers or the user presses
  Stop. The GUI and INI accept 0 or 1..200.
- Added a loop guard to the agent loop. Three consecutive identical tool calls
  (same name and arguments) no longer execute on the third attempt: the model
  receives an error tool result asking for a different approach, and a fourth
  identical call stops the run. Blocked calls still produce a tool message, so
  the conversation history stays valid.
- The loop guard also compares whole rounds, so a model alternating between two
  different calls is stopped as well. The two counters are independent.
- README is now bilingual: full English documentation followed by the
  Simplified Chinese original, with language links at the top.

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
