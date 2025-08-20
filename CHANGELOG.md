# Changelog

Each release lists what changed, what updating means for existing code, and every security fix.

## Unreleased

New function and bug fixes; no change needed in existing code.

- `spw_cmd_output_limit` and `spw_proc_dropped`: cap the memory a chatty child can make the
  parent allocate; bytes past the cap are still read, so the child never blocks

- a relative `PATH` entry combined with a relative working directory now finds the program;
  before, the path checked in the parent was then run again from the child directory
- `spw_cmd_args` leaves the argument list unchanged when it runs out of memory; before, the
  arguments copied up to the failure stayed in the list
- `spw_cmd_stdio` on stdin drops input set earlier by `spw_cmd_input`; before, a later
  `SPW_PIPE` on stdin still fed the old bytes

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
