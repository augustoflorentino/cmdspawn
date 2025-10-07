# cmdspawn

Run another program the same way on Linux, macOS, BSD and Windows: no shell, captured output,
time limits.

- the command is an argument list, never a shell string; the program is looked up in `PATH`
- POSIX uses `posix_spawn`; Windows uses `CreateProcessW` with UTF-8 converted to UTF-16
- Windows command lines are built as the exact inverse of `CommandLineToArgvW` and the
  Microsoft C runtime rules, so every argument arrives unchanged (`spw_win_quote`)
- environment changes and working directory per command
- stdout and stderr captured separately or merged, stdin fed from memory, without deadlock on
  large output
- time limit that kills the whole process tree (process group on POSIX, job object on Windows)
- exit code or signal
- detached launch: fire and forget, no zombie
- non-blocking use: wait with a time limit of 0 and hand the pipe and process handles to your
  own event loop

<p align="center"><img src="docs/images/timeline.svg" alt="examples/poll_loop.c running a child that prints three lines: the parent waits in 100 ms slices and sees the bytes arrive through the stdout pipe" width="820"></p>

## Build and install

```sh
cmake -S . -B build -DBUILD_SHARED_LIBS=ON
cmake --build build
cmake --install build --prefix /usr/local
```

Use it from CMake:

```cmake
find_package(cmdspawn REQUIRED)
target_link_libraries(app PRIVATE cmdspawn::cmdspawn)
```

Or with pkg-config:

```sh
cc app.c $(pkg-config --cflags --libs cmdspawn)
```

## Single-file form

`scripts/amalgamate.sh` writes `dist/cmdspawn/cmdspawn.h`. The folder keeps it from hiding the
system `<spawn.h>`. Copy the `cmdspawn` folder into a directory on your include path and, in
exactly one `.c` file, before any other include:

```c
#define SPW_IMPLEMENTATION
#include "cmdspawn/cmdspawn.h"
```

Define `SPW_STATIC` as well to keep every function private to that file.

## Usage

```c
const char *args[] = {"git", "rev-parse", "HEAD"};
spw_cmd_t *cmd = NULL;
spw_proc_t *proc = NULL;
int status;

status = spw_cmd_create(NULL, NULL, NULL, &cmd);
if (status == SPW_OK)
    status = spw_cmd_args(cmd, args, 3);
if (status == SPW_OK)
    status = spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE);
if (status == SPW_OK)
    status = spw_cmd_timeout(cmd, 5000);
if (status == SPW_OK)
    status = spw_spawn(cmd, &proc);
if (status == SPW_OK)
    status = spw_proc_wait(proc, -1);
```

Then `spw_proc_status` gives the exit code and `spw_proc_output` the captured bytes. See
[`examples/`](examples): `basic.c` (run and capture), `hook.c` (detached launch) and
`poll_loop.c` (non-blocking wait).

Conventions:

- every function that can fail returns `SPW_OK` or an error code; `spw_strerror` describes it
- what `spw_cmd_create` and `spw_spawn` return is released only by `spw_cmd_destroy` and
  `spw_proc_destroy`, which accept `NULL`
- functions never take ownership of their arguments and copy what they keep; the command can
  be changed, reused or destroyed after `spw_spawn`
- pass your own allocator to `spw_cmd_create`, or `NULL` for `malloc`/`free`; processes use
  the allocator of the command that started them
- all text is UTF-8

### Command

- `spw_cmd_args` appends arguments; the first one is the program. A name without a path
  separator is looked up in the `PATH` of the child environment (falling back to `/usr/bin:/bin`
  on POSIX; on Windows the system directory is tried after `PATH`, `.exe` is added when the
  name has no extension, and the current directory is never searched). A relative path is
  relative to the child working directory
- `spw_cmd_env` sets a variable, or removes it when `value` is `NULL`; `spw_cmd_env_clear`
  starts from an empty environment instead of the parent one. Names are case-insensitive on
  Windows
- `spw_cmd_stdio` picks, per stream, `SPW_INHERIT` (default), `SPW_NULL`, `SPW_PIPE` (capture,
  or feed for stdin) or, for stderr only, `SPW_MERGE` (same destination as stdout)
- `spw_cmd_input` copies the bytes to feed on stdin and switches stdin to `SPW_PIPE`; stdin is
  closed once everything is written. A stdin pipe without input gives the child an
  immediate end of file. A later `spw_cmd_stdio` call on stdin drops the input
- `spw_cmd_timeout` sets a limit in milliseconds from `spw_spawn`; `0` means none
- `spw_cmd_kill_grace` makes the time limit ask first: the tree gets `SIGTERM`, and `SIGKILL`
  only if it still runs `grace_ms` later. Windows kills at once
- `spw_cmd_same_group(cmd, 1)` keeps the child in the caller's process group, which an
  interactive program needs to read the terminal (an editor, a pager, a password prompt):
  in its own group it would be stopped by `SIGTTIN`. The price is that kill, terminate and
  the time limit then reach only the child, not its descendants. No effect on Windows, where
  the job object still holds the tree
- `spw_cmd_output_limit` caps the bytes kept per captured stream; `0` (default) means no cap.
  The child is never blocked: bytes past the cap are read and thrown away
- `spw_cmd_output_fn` hands each captured chunk to a callback, from inside `spw_proc_wait`,
  instead of keeping it: `spw_proc_output` then stays empty and the limit does not apply.
  `NULL` goes back to keeping the bytes

### Process

- `spw_proc_wait(proc, timeout_ms)` moves data through the pipes and returns `SPW_OK` once the
  process has exited and its output pipes are closed. `timeout_ms` of `-1` waits as long as
  needed, `0` only does what is ready, and `SPW_ERR_AGAIN` means the process is still running.
  When the command time limit passes, the whole tree is killed and `SPW_ERR_TIMEOUT` is
  returned from then on
- `spw_proc_kill` kills the whole tree now; `spw_proc_wait` then collects it
- `spw_proc_terminate` asks the whole tree to stop (`SIGTERM`), so it can clean up; follow
  with `spw_proc_kill` if it does not. Windows has no such request for a console-less child:
  `SPW_ERR_UNSUPPORTED`
- `spw_proc_status` gives the exit code and the signal (POSIX) that ended the process;
  `SPW_ERR_AGAIN` while it runs. A signal gives exit code `-1`; a killed Windows process exits
  with code `1`
- `spw_proc_output` returns the bytes captured so far; the pointer is valid until the next
  `spw_proc_wait` or `spw_proc_destroy`; `spw_proc_dropped` counts the bytes thrown away
  past the output limit
- `spw_proc_destroy` on a running process kills its tree and collects it first
- `spw_proc_handle` gives what an event loop waits on, then call `spw_proc_wait(proc, 0)`:
  - POSIX: the pipe file descriptors for `SPW_STDIN`, `SPW_STDOUT` and `SPW_STDERR`, and for
    `SPW_PROCESS` a pidfd on Linux 5.3 or later (`-1` elsewhere: use `spw_proc_pid` with
    kqueue, or wait with a small time limit)
  - Windows: an event signaled when the pipe operation completes, and the process handle
  - `SPW_NO_HANDLE` once the stream is closed

### Detached

`spw_spawn_detached(cmd, &pid)` starts the program and forgets it; `pid` (or `NULL`) receives
the program's process id, to write a pid file or stop it later. On POSIX it runs in a new session,
through an intermediate process that exits at once, so no zombie is left; on Windows it is
not put in a job and gets its own process group. Pipes are not allowed;
inherit or `SPW_NULL` only.

### Windows command line

`spw_win_quote` writes the command line that `CommandLineToArgvW` and the Microsoft C runtime
both split back into exactly the given arguments. It is pure logic, available on every system.
The program name cannot contain `"`. `buf_len` is the buffer capacity; `out_len` receives the
length without the final NUL, and `SPW_ERR_RANGE` means the buffer is too small (pass `NULL`
and `0` to ask for the length).

### Limits

- POSIX: the child gets its own process group so the tree can be killed. A child in a
  background group that reads from a terminal is stopped by `SIGTTIN`: give it `SPW_NULL` or
  `SPW_PIPE` on stdin. Descendants that start their own process group or session (daemons,
  shells with job control) leave the tree and are not killed by the time limit
- Windows: descendants created with `CREATE_BREAKAWAY_FROM_JOB` leave the tree. Batch files
  (`.bat`, `.cmd`) run through `cmd.exe`, which has different quoting rules; they are refused
  with `SPW_ERR_UNSUPPORTED`. Run `cmd.exe /c` yourself if you need them
- when a descendant keeps an output pipe open after the child exits, `spw_proc_wait` keeps
  waiting for it; set a time limit if that can happen

## Versioning

Semantic Versioning. Compatible releases bump the minor version; incompatible ones bump the
major version and the soname. See [CHANGELOG.md](CHANGELOG.md).

## License

MIT, see [LICENSE](LICENSE).
