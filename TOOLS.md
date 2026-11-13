# Agent98 Tools Reference (31)

## Read / Search
| Tool | Confirm | Notes |
|------|---------|-------|
| list_dir | no | List workspace directory |
| read_file | no | Optional start_line / max_lines |
| head / tail | no | First/last N lines (default 20) |
| file_info | no | Size, mtime, attributes |
| search_files | no | Literal content search |
| grep | no | Literal search; `include=*.c` filters basename |
| glob | no | `*` / `?` path patterns |
| tree | no | Directory tree, max_depth |
| wc | no | lines / words / bytes |
| which | no | Resolve name on PATH |
| diff | no | Line-by-line a vs b |
| checksum | no | CRC32 + size |

## Write / Change
| Tool | Confirm | Notes |
|------|---------|-------|
| write_file | yes | Full rewrite, ANSI+CRLF for text |
| append_file | yes | Append text |
| edit | yes | Unique old_string replace; replace_all option |
| apply_patch | yes | Multi-hunk `*** SEARCH` / `*** REPLACE` / `*** END`; writes `.bak` (aborts if the backup fails) |
| touch | no | Create empty or bump mtime |
| mkdir | no | Create parents |
| set_attr | yes | readonly / hidden / archive / system |

## Manage
| Tool | Confirm | Notes |
|------|---------|-------|
| delete_file | yes | Files only |
| rmdir | yes | Empty directory only |
| rmdir_force | yes | Needs `confirm=true`; recursive; never workspace root |
| move_file | yes | Rename/move file |
| copy_file | yes | `overwrite=true` optional |

## Execute / Env / Tasks
| Tool | Confirm | Notes |
|------|---------|-------|
| run_command | yes | Optional `cwd`; blocks format/deltree/rd /s etc. |
| disk_info | no | Drive free/total |
| env | yes | One var or common list; prompts before reading |
| session_info | no | OS, workspace, tool names |
| todo_read / todo_write | write yes | `.agent98_todos.txt` in workspace |

## Safety
- All paths stay under workspace (resolve_path).
- Destructive tools require GUI/CLI confirmation.
- Tool argument logs redact api_key / password / token / secret patterns.
- `run_command` refuses common disk-wipe style command patterns.
- `rmdir_force` normalises the workspace root first, so a relative or
  trailing-slash `workspace=` in the INI cannot be deleted recursively.

## apply_patch example
```
*** SEARCH
int x = 1;
*** REPLACE
int x = 2;
*** END
```
