#include "spawn_internal.h"

#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#define spw_environ (*_NSGetEnviron())
#else
extern char **environ;
#define spw_environ environ
#endif

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29)) && !defined(SPW_NO_SPAWN_CHDIR)
#define SPW_HAVE_SPAWN_CHDIR 1
#else
#define SPW_HAVE_SPAWN_CHDIR 0
#endif

#define SPW_READ_CHUNK 16384
#define SPW_POLL_SLICE_MS 10

struct spw_proc {
    struct spw_mem mem;
    pid_t pid;
    int pidfd;
    int fds[3];
    unsigned char *input;
    size_t input_len;
    size_t input_off;
    struct spw_out out[2];
    int exited;
    int exit_code;
    int signal;
    int timed_out;
    int stopped;
    int reaped;
    int killed;
    int own_group;
    long grace_ms;
    long long deadline;
    long long kill_at;
};

struct spw_prep {
    char *path;
    char **argv;
    char **envp;
    const char *cwd;
    int child_fds[3];
    int parent_fds[3];
    int null_fd;
    int merge_err;
    int own_group;
    int err_pipe[2];
};

static long long spw_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void spw_close(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static int spw_cloexec_above_stdio(int fd)
{
    int moved;

    if (fd < 0)
        return -1;
    if (fd > 2) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        return fd;
    }
    moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    close(fd);
    return moved;
}

static int spw_pipe(int out_fds[2])
{
    int fds[2];

    if (pipe(fds) != 0)
        return SPW_ERR_SPAWN;
    out_fds[0] = spw_cloexec_above_stdio(fds[0]);
    out_fds[1] = spw_cloexec_above_stdio(fds[1]);
    if (out_fds[0] < 0 || out_fds[1] < 0) {
        spw_close(&out_fds[0]);
        spw_close(&out_fds[1]);
        return SPW_ERR_SPAWN;
    }
#if defined(F_SETNOSIGPIPE)
    fcntl(out_fds[0], F_SETNOSIGPIPE, 1);
    fcntl(out_fds[1], F_SETNOSIGPIPE, 1);
#endif
    return SPW_OK;
}

static int spw_is_file_executable(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

static int spw_is_executable(const struct spw_mem *mem, const char *cwd, const char *path)
{
    char *full;
    int ok;

    if (cwd == NULL || path[0] == '/')
        return spw_is_file_executable(path);
    full = spw_mem_join(mem, cwd, strlen(cwd), '/', path);
    if (full == NULL)
        return -1;
    ok = spw_is_file_executable(full);
    spw_mem_free(mem, full);
    return ok;
}

static int spw_resolve(const spw_cmd_t *cmd, char **envp, char **out_path)
{
    const char *name = cmd->args[0];
    const char *dirs = spw_env_find(envp, "PATH", 0);
    const char *end;
    char *candidate;
    int found;

    if ((strchr)(name, '/') != NULL) {
        *out_path = spw_mem_strdup(&cmd->mem, name);
        return *out_path ? SPW_OK : SPW_ERR_NOMEM;
    }
    if (dirs == NULL)
        dirs = "/usr/bin:/bin";
    for (;;) {
        end = (strchr)(dirs, ':');
        if (end == NULL)
            end = dirs + strlen(dirs);
        candidate = (end == dirs) ? spw_mem_strdup(&cmd->mem, name)
                                  : spw_mem_join(&cmd->mem, dirs, (size_t)(end - dirs), '/', name);
        found = candidate ? spw_is_executable(&cmd->mem, cmd->cwd, candidate) : -1;
        if (found < 0) {
            spw_mem_free(&cmd->mem, candidate);
            return SPW_ERR_NOMEM;
        }
        if (found) {
            *out_path = candidate;
            return SPW_OK;
        }
        spw_mem_free(&cmd->mem, candidate);
        if (*end == '\0')
            return SPW_ERR_NOTFOUND;
        dirs = end + 1;
    }
}

static void spw_prep_init(struct spw_prep *prep)
{
    int i;

    memset(prep, 0, sizeof(*prep));
    for (i = 0; i < 3; i++) {
        prep->child_fds[i] = -1;
        prep->parent_fds[i] = -1;
    }
    prep->null_fd = -1;
    prep->err_pipe[0] = -1;
    prep->err_pipe[1] = -1;
}

static void spw_prep_close_child(struct spw_prep *prep)
{
    int i;

    for (i = 0; i < 3; i++)
        if (prep->child_fds[i] != prep->null_fd)
            spw_close(&prep->child_fds[i]);
    spw_close(&prep->null_fd);
    for (i = 0; i < 3; i++)
        prep->child_fds[i] = -1;
}

static void spw_prep_free(const struct spw_mem *mem, struct spw_prep *prep)
{
    int i;

    spw_prep_close_child(prep);
    for (i = 0; i < 3; i++)
        spw_close(&prep->parent_fds[i]);
    spw_close(&prep->err_pipe[0]);
    spw_close(&prep->err_pipe[1]);
    spw_mem_free(mem, prep->path);
    spw_mem_free(mem, prep->argv);
    spw_mem_free(mem, prep->envp);
}

static int spw_prep_stream(struct spw_prep *prep, int stream, int mode)
{
    int fds[2];
    int status;

    if (mode == SPW_MERGE) {
        prep->merge_err = 1;
        return SPW_OK;
    }
    if (mode == SPW_NULL) {
        if (prep->null_fd < 0)
            prep->null_fd = spw_cloexec_above_stdio(open("/dev/null", O_RDWR | O_CLOEXEC));
        prep->child_fds[stream] = prep->null_fd;
        return prep->null_fd >= 0 ? SPW_OK : SPW_ERR_SPAWN;
    }
    if (mode != SPW_PIPE)
        return SPW_OK;
    status = spw_pipe(fds);
    if (status != SPW_OK)
        return status;
    prep->child_fds[stream] = stream == SPW_STDIN ? fds[0] : fds[1];
    prep->parent_fds[stream] = stream == SPW_STDIN ? fds[1] : fds[0];
    fcntl(prep->parent_fds[stream], F_SETFL, fcntl(prep->parent_fds[stream], F_GETFL) | O_NONBLOCK);
    return SPW_OK;
}

static int spw_prepare(const spw_cmd_t *cmd, struct spw_prep *prep)
{
    size_t base_len = 0;
    int status;
    int i;

    spw_prep_init(prep);
    while (spw_environ != NULL && spw_environ[base_len] != NULL)
        base_len++;
    status = spw_env_merge(cmd, spw_environ, base_len, 0, &prep->envp);
    if (status == SPW_OK)
        status = spw_resolve(cmd, prep->envp, &prep->path);
    if (status != SPW_OK)
        return status;
    prep->argv = spw_mem_alloc(&cmd->mem, (cmd->args_len + 1) * sizeof(char *));
    if (prep->argv == NULL)
        return SPW_ERR_NOMEM;
    memcpy(prep->argv, cmd->args, cmd->args_len * sizeof(char *));
    prep->argv[cmd->args_len] = NULL;
    prep->cwd = cmd->cwd;
    prep->own_group = !cmd->same_group;
    for (i = 0; i < 3 && status == SPW_OK; i++)
        status = spw_prep_stream(prep, i, cmd->modes[i]);
    return status;
}

static void spw_child_signals(void)
{
    static const int sigs[] = {SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGCHLD, SIGALRM, SIGUSR1, SIGUSR2,
                               SIGTTIN, SIGTTOU, SIGTSTP};
    struct sigaction sa;
    sigset_t empty;
    size_t i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
}

struct spw_child_msg {
    int kind;
    int value;
};

static void spw_child_send(int fd, int kind, int value)
{
    struct spw_child_msg msg;

    msg.kind = kind;
    msg.value = value;
    while (write(fd, &msg, sizeof(msg)) < 0 && errno == EINTR) {
    }
}

static void spw_child_fail(int err_fd)
{
    spw_child_send(err_fd, 0, errno);
    _exit(127);
}

static void spw_child_exec(const struct spw_prep *prep, int new_group)
{
    int i;

    if (new_group)
        setpgid(0, 0);
    for (i = 0; i < 3; i++)
        if (prep->child_fds[i] >= 0 && dup2(prep->child_fds[i], i) < 0)
            spw_child_fail(prep->err_pipe[1]);
    if (prep->merge_err && dup2(1, 2) < 0)
        spw_child_fail(prep->err_pipe[1]);
    spw_child_signals();
    if (prep->cwd != NULL && chdir(prep->cwd) != 0)
        spw_child_fail(prep->err_pipe[1]);
    execve(prep->path, prep->argv, prep->envp);
    spw_child_fail(prep->err_pipe[1]);
}

static int spw_errno_status(int err)
{
    if (err == ENOENT || err == ENOTDIR)
        return SPW_ERR_NOTFOUND;
    if (err == ENOMEM)
        return SPW_ERR_NOMEM;
    if (err == EACCES || err == EPERM)
        return SPW_ERR_ACCESS;
    if (err == ENOEXEC)
        return SPW_ERR_FORMAT;
    return SPW_ERR_SPAWN;
}

static int spw_read_child(struct spw_prep *prep, pid_t *out_pid)
{
    struct spw_child_msg msg;
    int status = SPW_OK;
    ssize_t n;

    spw_close(&prep->err_pipe[1]);
    for (;;) {
        n = read(prep->err_pipe[0], &msg, sizeof(msg));
        if (n < 0 && errno == EINTR)
            continue;
        if (n != (ssize_t)sizeof(msg))
            break;
        if (msg.kind == 0)
            status = spw_errno_status(msg.value);
        else if (out_pid != NULL)
            *out_pid = (pid_t)msg.value;
    }
    spw_close(&prep->err_pipe[0]);
    return status;
}

static void spw_reap(pid_t pid)
{
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
}

static int spw_fork_exec(struct spw_prep *prep, pid_t *out_pid)
{
    pid_t pid;
    int status;

    if (spw_pipe(prep->err_pipe) != SPW_OK)
        return SPW_ERR_SPAWN;
    pid = fork();
    if (pid < 0)
        return SPW_ERR_SPAWN;
    if (pid == 0)
        spw_child_exec(prep, prep->own_group);
    if (prep->own_group)
        setpgid(pid, pid);
    status = spw_read_child(prep, NULL);
    if (status != SPW_OK) {
        spw_reap(pid);
        return status;
    }
    *out_pid = pid;
    return SPW_OK;
}

static int spw_posix_spawn(struct spw_prep *prep, pid_t *out_pid)
{
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    sigset_t sigs;
    int err;
    int i;

    if (posix_spawn_file_actions_init(&actions) != 0)
        return SPW_ERR_NOMEM;
    if (posix_spawnattr_init(&attr) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return SPW_ERR_NOMEM;
    }
    err = 0;
    for (i = 0; i < 3 && err == 0; i++)
        if (prep->child_fds[i] >= 0)
            err = posix_spawn_file_actions_adddup2(&actions, prep->child_fds[i], i);
    if (err == 0 && prep->merge_err)
        err = posix_spawn_file_actions_adddup2(&actions, 1, 2);
#if SPW_HAVE_SPAWN_CHDIR
    if (err == 0 && prep->cwd != NULL)
        err = posix_spawn_file_actions_addchdir_np(&actions, prep->cwd);
#endif
    sigemptyset(&sigs);
    if (err == 0)
        err = posix_spawnattr_setsigmask(&attr, &sigs);
    sigaddset(&sigs, SIGPIPE);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGQUIT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGHUP);
    sigaddset(&sigs, SIGCHLD);
    sigaddset(&sigs, SIGTTIN);
    sigaddset(&sigs, SIGTTOU);
    if (err == 0)
        err = posix_spawnattr_setsigdefault(&attr, &sigs);
    if (err == 0)
        err = posix_spawnattr_setpgroup(&attr, 0);
    if (err == 0)
        err = posix_spawnattr_setflags(&attr, (prep->own_group ? POSIX_SPAWN_SETPGROUP : 0) | POSIX_SPAWN_SETSIGMASK |
                                                  POSIX_SPAWN_SETSIGDEF);
    if (err == 0)
        err = posix_spawn(out_pid, prep->path, &actions, &attr, prep->argv, prep->envp);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    return err == 0 ? SPW_OK : spw_errno_status(err);
}

static int spw_start(struct spw_prep *prep, pid_t *out_pid)
{
    if (prep->cwd == NULL || SPW_HAVE_SPAWN_CHDIR)
        return spw_posix_spawn(prep, out_pid);
    return spw_fork_exec(prep, out_pid);
}

static int spw_open_pidfd(pid_t pid)
{
#if defined(__linux__) && defined(SYS_pidfd_open)
    return (int)syscall(SYS_pidfd_open, pid, 0);
#else
    (void)pid;
    return -1;
#endif
}

static int spw_proc_alloc(const spw_cmd_t *cmd, struct spw_prep *prep, spw_proc_t **out_proc)
{
    spw_proc_t *proc = spw_mem_alloc(&cmd->mem, sizeof(*proc));
    int i;

    if (proc == NULL)
        return SPW_ERR_NOMEM;
    memset(proc, 0, sizeof(*proc));
    proc->mem = cmd->mem;
    proc->pidfd = -1;
    if (cmd->input_len > 0) {
        proc->input = spw_mem_alloc(&cmd->mem, cmd->input_len);
        if (proc->input == NULL) {
            spw_mem_free(&cmd->mem, proc);
            return SPW_ERR_NOMEM;
        }
        memcpy(proc->input, cmd->input, cmd->input_len);
        proc->input_len = cmd->input_len;
    }
    spw_out_init(cmd, &proc->out[0], SPW_STDOUT);
    spw_out_init(cmd, &proc->out[1], SPW_STDERR);
    for (i = 0; i < 3; i++) {
        proc->fds[i] = prep->parent_fds[i];
        prep->parent_fds[i] = -1;
    }
    *out_proc = proc;
    return SPW_OK;
}

int spw_spawn(const spw_cmd_t *cmd, spw_proc_t **out_proc)
{
    struct spw_prep prep;
    spw_proc_t *proc = NULL;
    pid_t pid;
    int status;

    if (out_proc == NULL)
        return SPW_ERR_ARG;
    *out_proc = NULL;
    status = spw_cmd_check(cmd, 0);
    if (status != SPW_OK)
        return status;
    status = spw_prepare(cmd, &prep);
    if (status == SPW_OK)
        status = spw_proc_alloc(cmd, &prep, &proc);
    if (status == SPW_OK)
        status = spw_start(&prep, &pid);
    if (status != SPW_OK) {
        spw_proc_destroy(proc);
        spw_prep_free(&cmd->mem, &prep);
        return status;
    }
    spw_prep_free(&cmd->mem, &prep);
    proc->pid = pid;
    proc->pidfd = spw_open_pidfd(pid);
    if (cmd->timeout_ms > 0)
        proc->deadline = spw_now_ms() + cmd->timeout_ms;
    proc->own_group = !cmd->same_group;
    proc->grace_ms = cmd->grace_ms;
    if (proc->fds[SPW_STDIN] >= 0 && proc->input_len == 0)
        spw_close(&proc->fds[SPW_STDIN]);
    *out_proc = proc;
    return SPW_OK;
}

static void spw_detached_middle(struct spw_prep *prep)
{
    pid_t pid;

    setsid();
    pid = fork();
    if (pid < 0)
        spw_child_fail(prep->err_pipe[1]);
    if (pid == 0)
        spw_child_exec(prep, 0);
    spw_child_send(prep->err_pipe[1], 1, (int)pid);
    _exit(0);
}

int spw_spawn_detached(const spw_cmd_t *cmd, long *out_pid)
{
    struct spw_prep prep;
    pid_t pid;
    pid_t child = -1;
    int status;

    if (out_pid != NULL)
        *out_pid = -1;
    status = spw_cmd_check(cmd, 1);
    if (status != SPW_OK)
        return status;
    status = spw_prepare(cmd, &prep);
    if (status == SPW_OK)
        status = spw_pipe(prep.err_pipe);
    if (status == SPW_OK) {
        pid = fork();
        if (pid == 0)
            spw_detached_middle(&prep);
        if (pid < 0)
            status = SPW_ERR_SPAWN;
        else
            status = spw_read_child(&prep, &child);
        if (pid > 0)
            spw_reap(pid);
    }
    spw_prep_free(&cmd->mem, &prep);
    if (status == SPW_OK && out_pid != NULL)
        *out_pid = (long)child;
    return status;
}

static void spw_check_exit(spw_proc_t *proc)
{
    siginfo_t info;
    int r;

    if (proc->exited)
        return;
    memset(&info, 0, sizeof(info));
    do
        r = waitid(P_PID, (id_t)proc->pid, &info, WEXITED | WNOHANG | WNOWAIT);
    while (r < 0 && errno == EINTR);
    if (r == 0 && info.si_pid == 0)
        return;
    proc->exited = 1;
    proc->exit_code = -1;
    spw_close(&proc->pidfd);
    if (r < 0) {
        proc->reaped = 1;
        return;
    }
    if (info.si_code == CLD_EXITED)
        proc->exit_code = info.si_status;
    else
        proc->signal = info.si_status;
}

static int spw_drain(spw_proc_t *proc, int stream)
{
    struct spw_out *out = &proc->out[stream - 1];
    unsigned char chunk[SPW_READ_CHUNK];
    ssize_t n;

    for (;;) {
        n = read(proc->fds[stream], chunk, sizeof(chunk));
        if (n > 0) {
            if (spw_out_put(&proc->mem, out, chunk, (size_t)n) != SPW_OK)
                return SPW_ERR_NOMEM;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return SPW_OK;
        spw_close(&proc->fds[stream]);
        return SPW_OK;
    }
}

static ssize_t spw_write_nosigpipe(int fd, const void *data, size_t len)
{
#if defined(F_SETNOSIGPIPE)
    return write(fd, data, len);
#else
    sigset_t block;
    sigset_t old;
    sigset_t pending;
    ssize_t n;
    int was_pending;
    int err;

    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    sigprocmask(SIG_BLOCK, &block, &old);
    sigpending(&pending);
    was_pending = sigismember(&pending, SIGPIPE);
    n = write(fd, data, len);
    err = errno;
    if (n < 0 && err == EPIPE && !was_pending) {
        struct timespec zero = {0, 0};

        while (sigtimedwait(&block, NULL, &zero) < 0 && errno == EINTR) {
        }
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    errno = err;
    return n;
#endif
}

static void spw_feed(spw_proc_t *proc)
{
    size_t left = proc->input_len - proc->input_off;
    ssize_t n;

    while (left > 0) {
        n = spw_write_nosigpipe(proc->fds[SPW_STDIN], proc->input + proc->input_off, left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0)
            break;
        proc->input_off += (size_t)n;
        left -= (size_t)n;
    }
    spw_close(&proc->fds[SPW_STDIN]);
}

static int spw_poll_once(spw_proc_t *proc, int wait_ms)
{
    struct pollfd pfds[4];
    int streams[4];
    nfds_t n = 0;
    nfds_t i;
    int status = SPW_OK;
    int s;

    for (s = 0; s < 3; s++) {
        if (proc->fds[s] < 0)
            continue;
        pfds[n].fd = proc->fds[s];
        pfds[n].events = s == SPW_STDIN ? POLLOUT : POLLIN;
        pfds[n].revents = 0;
        streams[n++] = s;
    }
    if (proc->pidfd >= 0 && !proc->exited) {
        pfds[n].fd = proc->pidfd;
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        streams[n++] = SPW_PROCESS;
    }
    if (poll(n ? pfds : NULL, n, wait_ms) <= 0)
        return SPW_OK;
    for (i = 0; i < n && status == SPW_OK; i++) {
        if (pfds[i].revents == 0 || streams[i] == SPW_PROCESS)
            continue;
        if (streams[i] == SPW_STDIN)
            spw_feed(proc);
        else
            status = spw_drain(proc, streams[i]);
    }
    return status;
}

static int spw_done(spw_proc_t *proc)
{
    int s;

    if (!proc->exited)
        return 0;
    spw_close(&proc->fds[SPW_STDIN]);
    if (!proc->timed_out && !proc->stopped)
        return proc->fds[SPW_STDOUT] < 0 && proc->fds[SPW_STDERR] < 0;
    for (s = SPW_STDOUT; s <= SPW_STDERR; s++)
        if (proc->fds[s] >= 0 && spw_drain(proc, s) == SPW_OK)
            spw_close(&proc->fds[s]);
    return 1;
}

static long long spw_next_deadline(const spw_proc_t *proc)
{
    if (proc->deadline > 0 && !proc->timed_out)
        return proc->deadline;
    return proc->kill_at;
}

static int spw_wait_ms(const spw_proc_t *proc, long long now, long long limit)
{
    long long wait = limit;
    long long next = spw_next_deadline(proc);

    if (next > 0 && (wait < 0 || next - now < wait))
        wait = next - now < 0 ? 0 : next - now;
    if (!proc->exited && proc->pidfd < 0 && (wait < 0 || wait > SPW_POLL_SLICE_MS))
        wait = SPW_POLL_SLICE_MS;
    if (wait < 0)
        return -1;
    return wait > 1000000 ? 1000000 : (int)wait;
}

static void spw_signal_tree(spw_proc_t *proc, int sig)
{
    if (proc->own_group && kill(-proc->pid, sig) == 0)
        return;
    if (!proc->exited)
        kill(proc->pid, sig);
}

int spw_proc_kill(spw_proc_t *proc)
{
    if (proc == NULL)
        return SPW_ERR_ARG;
    spw_check_exit(proc);
    if (!proc->exited)
        proc->killed = 1;
    spw_signal_tree(proc, SIGKILL);
    proc->kill_at = 0;
    return SPW_OK;
}

int spw_proc_terminate(spw_proc_t *proc)
{
    if (proc == NULL)
        return SPW_ERR_ARG;
    spw_signal_tree(proc, SIGTERM);
    return SPW_OK;
}

static void spw_time_up(spw_proc_t *proc, long long now)
{
    proc->timed_out = 1;
    if (proc->grace_ms <= 0 || proc->exited) {
        spw_proc_kill(proc);
        return;
    }
    spw_signal_tree(proc, SIGTERM);
    proc->kill_at = now + proc->grace_ms;
}

static int spw_wait_result(const spw_proc_t *proc)
{
    if (proc->stopped)
        return SPW_ERR_STOPPED;
    return proc->timed_out ? SPW_ERR_TIMEOUT : SPW_OK;
}

int spw_proc_wait(spw_proc_t *proc, long timeout_ms)
{
    long long start = spw_now_ms();
    long long now = start;
    long long limit;
    int status;

    if (proc == NULL)
        return SPW_ERR_ARG;
    for (;;) {
        spw_check_exit(proc);
        if (spw_done(proc))
            return spw_wait_result(proc);
        if (proc->deadline > 0 && !proc->timed_out && now >= proc->deadline) {
            spw_time_up(proc, now);
            continue;
        }
        if (proc->kill_at > 0 && now >= proc->kill_at) {
            spw_proc_kill(proc);
            continue;
        }
        if (timeout_ms >= 0 && now - start >= timeout_ms)
            return SPW_ERR_AGAIN;
        limit = timeout_ms < 0 ? -1 : timeout_ms - (now - start);
        status = spw_poll_once(proc, spw_wait_ms(proc, now, limit));
        if (status != SPW_OK)
            return status;
        if (!proc->stopped && (proc->out[0].stopped || proc->out[1].stopped)) {
            proc->stopped = 1;
            spw_proc_kill(proc);
        }
        now = spw_now_ms();
    }
}

void spw_proc_destroy(spw_proc_t *proc)
{
    int i;

    if (proc == NULL)
        return;
    if (proc->pid > 0 && !proc->exited)
        spw_proc_kill(proc);
    if (proc->pid > 0 && !proc->reaped)
        spw_reap(proc->pid);
    for (i = 0; i < 3; i++)
        spw_close(&proc->fds[i]);
    spw_close(&proc->pidfd);
    spw_mem_free(&proc->mem, proc->input);
    spw_mem_free(&proc->mem, proc->out[0].data);
    spw_mem_free(&proc->mem, proc->out[1].data);
    spw_mem_free(&proc->mem, proc);
}

int spw_proc_status(const spw_proc_t *proc, int *out_exit_code, int *out_signal)
{
    if (proc == NULL)
        return SPW_ERR_ARG;
    if (!proc->exited)
        return SPW_ERR_AGAIN;
    if (out_exit_code != NULL)
        *out_exit_code = proc->exit_code;
    if (out_signal != NULL)
        *out_signal = proc->signal;
    return SPW_OK;
}

int spw_proc_killed(const spw_proc_t *proc)
{
    return proc != NULL && proc->killed;
}

long spw_proc_pid(const spw_proc_t *proc)
{
    return proc ? (long)proc->pid : -1;
}

spw_handle_t spw_proc_handle(const spw_proc_t *proc, int which)
{
    if (proc == NULL || which < SPW_STDIN || which > SPW_PROCESS)
        return SPW_NO_HANDLE;
    if (which == SPW_PROCESS)
        return proc->pidfd;
    return proc->fds[which];
}

const unsigned char *spw_proc_output(const spw_proc_t *proc, int stream, size_t *out_len)
{
    if (out_len != NULL)
        *out_len = 0;
    if (proc == NULL || (stream != SPW_STDOUT && stream != SPW_STDERR))
        return NULL;
    if (out_len != NULL)
        *out_len = proc->out[stream - 1].len;
    return proc->out[stream - 1].data;
}

size_t spw_proc_dropped(const spw_proc_t *proc, int stream)
{
    if (proc == NULL || (stream != SPW_STDOUT && stream != SPW_STDERR))
        return 0;
    return proc->out[stream - 1].dropped;
}

#endif
