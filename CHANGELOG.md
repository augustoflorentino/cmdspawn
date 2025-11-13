# Changelog

Each release lists what changed, what updating means for existing code, and every security fix.

## Unreleased

## 2.0.0 - 2025-11-12

Incompatible change: the output callback type.

- `spw_output_fn` returns `int`: `0` goes on, anything else kills the process tree and makes
  `spw_proc_wait` return the new `SPW_ERR_STOPPED`. Before, a callback could only watch: it
  had no way to stop a child whose output it no longer wanted (first error found, runaway
  output), since it receives neither the process nor a way to signal back
- soname `libcmdspawn.so.2`

### Upgrading

- change each output callback to return `int` and add `return 0;` to keep the old behavior
- a `switch` on `spw_proc_wait` results may want a case for `SPW_ERR_STOPPED`; it is only
  returned when a callback asks to stop
- rebuild; programs linked to `libcmdspawn.so.1` keep using the old library

## 1.4.0 - 2025-10-06

New functions; no change needed in existing code.

- `spw_cmd_same_group`: run the child in the caller's process group, so interactive programs
  that read the terminal work; before, every child got its own group and was stopped by
  `SIGTTIN` on its first terminal read
- `spw_cmd_kill_grace`: when the time limit passes, send `SIGTERM` and wait `grace_ms` before
  `SIGKILL`, like `timeout -k`

## 1.0.0 - 2025-09-21

The interface is stable from here on: incompatible changes only in a new major version, with
the soname bumped. The ABI baseline is recorded in `abi/cmdspawn.abi`.

- `spw_spawn_detached` takes a second parameter, `long *out_pid`, that receives the process id
  of the detached program (or `-1` on failure); before, a daemon started this way could not be
  tracked or stopped
- soname `libcmdspawn.so.1`

### Upgrading

- add `NULL` as the second argument of `spw_spawn_detached` to keep the old behavior, or pass a
  `long` to receive the pid
- rebuild against the new header; programs linked to `libcmdspawn.so.0` keep using the old library

## 0.5.0 - 2025-09-21

New functions; no change needed in existing code.

- `spw_cmd_output_fn`: stream captured output to a callback chunk by chunk instead of keeping
  it in memory, for long-running children and progress display
- `spw_proc_terminate`: send `SIGTERM` to the whole tree so the child can clean up before a
  `spw_proc_kill`; `SPW_ERR_UNSUPPORTED` on Windows

## 0.2.0 - 2025-08-19

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
