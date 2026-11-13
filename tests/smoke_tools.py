#!/usr/bin/env python3
"""Structural smoke test for Agent98 tools.c (no Win9x runtime required)."""
import re
import sys
from pathlib import Path

tools_c = Path(__file__).resolve().parents[1] / "src" / "agent" / "tools.c"
text = tools_c.read_text(encoding="utf-8", errors="replace")

expected = [
    "list_dir", "read_file", "write_file", "search_files", "run_command",
    "mkdir", "delete_file", "rmdir", "rmdir_force", "move_file", "copy_file",
    "append_file", "edit", "apply_patch", "touch", "file_info", "set_attr",
    "tree", "wc", "which", "glob", "grep", "head", "tail", "diff", "checksum",
    "disk_info", "env", "session_info", "todo_read", "todo_write",
]

names = re.findall(r'\{\s*"([a-z_]+)"\s*,', text)
found = [n for n in names if n in expected]
missing = [n for n in expected if n not in found]

extra_checks = {
    "grep_walk": "grep recursive walker",
    "redact_secrets": "secret redaction",
    "blocked dangerous command": "run_command ban list",
    ".bak": "apply_patch backup",
    "overwrite": "copy_file overwrite",
    '"cwd"': "run_command cwd",
}

print(f"tools registered: {len(found)} / {len(expected)}")
if missing:
    print("MISSING:", ", ".join(missing))
    sys.exit(1)

fail = 0
for needle, label in extra_checks.items():
    if needle not in text:
        print(f"FAIL: {label} ({needle})")
        fail = 1
    else:
        print(f"OK: {label}")

depth = 0
i = 0
n = len(text)
while i < n:
    c = text[i]
    if c == '"' or c == "'":
        q = c
        i += 1
        while i < n:
            if text[i] == '\\':
                i += 2
                continue
            if text[i] == q:
                i += 1
                break
            i += 1
        continue
    if c == '/' and i + 1 < n and text[i+1] == '/':
        while i < n and text[i] != '\n':
            i += 1
        continue
    if c == '/' and i + 1 < n and text[i+1] == '*':
        i += 2
        while i + 1 < n and not (text[i] == '*' and text[i+1] == '/'):
            i += 1
        i += 2
        continue
    if c == '{':
        depth += 1
    elif c == '}':
        depth -= 1
    i += 1
if depth != 0:
    print(f"FAIL: brace depth {depth}")
    fail = 1
else:
    print("OK: brace balance")

sys.exit(fail)
