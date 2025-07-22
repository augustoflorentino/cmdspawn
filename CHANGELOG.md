# Changelog

Each release lists what changed, what updating means for existing code, and every security fix.

## Unreleased

## 0.1.0 - 2025-07-21

First release.

- `spw_cmd_*`: build a command as an argument list, with environment changes, working
  directory, stdio mode per stream (inherit, null, pipe, stderr merged into stdout), stdin
  data and a time limit
- `spw_spawn`, `spw_proc_wait`, `spw_proc_kill`, `spw_proc_status`, `spw_proc_output`: start
  the program without a shell, capture its output without deadlock, kill the whole tree on
  time limit, read the exit code or signal; `posix_spawn` on POSIX, `CreateProcessW` with a
  job object on Windows
- `spw_proc_handle`, `spw_proc_pid`: pipe and process handles for an outside event loop
- `spw_spawn_detached`: fire and forget, no zombie
- `spw_win_quote`: Windows command line that `CommandLineToArgvW` and the Microsoft C runtime
  split back into the exact arguments
- batch files are refused on Windows (`SPW_ERR_UNSUPPORTED`): `cmd.exe` would parse their
  arguments with different rules
