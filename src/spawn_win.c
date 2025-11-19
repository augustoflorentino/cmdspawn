#include "spawn_internal.h"

#if defined(_WIN32)

#include <windows.h>

#include <stdio.h>
#include <string.h>

#define SPW_WIN_CHUNK 65536

struct spw_wpipe {
    HANDLE h;
    HANDLE ev;
    OVERLAPPED ov;
    int pending;
    unsigned char buf[SPW_WIN_CHUNK];
};

struct spw_proc {
    struct spw_mem mem;
    HANDLE process;
    HANDLE job;
    DWORD pid;
    struct spw_wpipe pipes[3];
    unsigned char *input;
    size_t input_len;
    size_t input_off;
    struct spw_out out[2];
    int exited;
    int exit_code;
    int timed_out;
    int stopped;
    int killed;
    ULONGLONG deadline;
};

struct spw_wprep {
    wchar_t *path;
    wchar_t *cmdline;
    wchar_t *env;
    wchar_t *cwd;
    HANDLE child[3];
    HANDLE parent[3];
};

static volatile LONG spw_pipe_counter;

static int spw_to_wide(const struct spw_mem *mem, const char *str, wchar_t **out_wide)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str, -1, NULL, 0);
    wchar_t *wide;

    if (n <= 0)
        return SPW_ERR_ENCODING;
    wide = spw_mem_alloc(mem, (size_t)n * sizeof(wchar_t));
    if (wide == NULL)
        return SPW_ERR_NOMEM;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str, -1, wide, n);
    *out_wide = wide;
    return SPW_OK;
}

static int spw_to_utf8(const struct spw_mem *mem, const wchar_t *wide, char **out_str)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *str;

    if (n <= 0)
        return SPW_ERR_ENCODING;
    str = spw_mem_alloc(mem, (size_t)n);
    if (str == NULL)
        return SPW_ERR_NOMEM;
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, str, n, NULL, NULL);
    *out_str = str;
    return SPW_OK;
}

static void spw_close_handle(HANDLE *h)
{
    if (*h != NULL && *h != INVALID_HANDLE_VALUE)
        CloseHandle(*h);
    *h = NULL;
}

static void spw_free_strings(const struct spw_mem *mem, char **list, size_t len)
{
    size_t i;

    for (i = 0; list != NULL && i < len; i++)
        spw_mem_free(mem, list[i]);
    spw_mem_free(mem, list);
}

static int spw_parent_env(const struct spw_mem *mem, char ***out_list, size_t *out_len)
{
    wchar_t *block = GetEnvironmentStringsW();
    const wchar_t *p;
    size_t count = 0;
    char **list;
    int status = SPW_OK;

    *out_list = NULL;
    *out_len = 0;
    if (block == NULL)
        return SPW_OK;
    for (p = block; *p; p += wcslen(p) + 1)
        count++;
    list = spw_mem_alloc(mem, (count + 1) * sizeof(char *));
    if (list == NULL) {
        FreeEnvironmentStringsW(block);
        return SPW_ERR_NOMEM;
    }
    count = 0;
    for (p = block; *p && status == SPW_OK; p += wcslen(p) + 1) {
        status = spw_to_utf8(mem, p, &list[count]);
        if (status == SPW_OK)
            count++;
    }
    FreeEnvironmentStringsW(block);
    *out_list = list;
    *out_len = count;
    return status;
}

static int spw_env_block(const struct spw_mem *mem, char **merged, wchar_t **out_block)
{
    size_t total = 1;
    size_t i;
    int n;
    wchar_t *block;
    wchar_t *at;

    for (i = 0; merged[i] != NULL; i++) {
        n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, merged[i], -1, NULL, 0);
        if (n <= 0)
            return SPW_ERR_ENCODING;
        total += (size_t)n;
    }
    if (i == 0)
        total = 2;
    block = spw_mem_alloc(mem, total * sizeof(wchar_t));
    if (block == NULL)
        return SPW_ERR_NOMEM;
    at = block;
    for (i = 0; merged[i] != NULL; i++)
        at += MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, merged[i], -1, at, (int)(total - (size_t)(at - block)));
    if (i == 0)
        *at++ = L'\0';
    *at = L'\0';
    *out_block = block;
    return SPW_OK;
}

static int spw_has_extension(const char *name)
{
    const char *base = name;
    const char *p;

    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;
    return (strchr)(base, '.') != NULL;
}

static int spw_is_batch(const char *path)
{
    size_t len = strlen(path);
    const char *ext = len >= 4 ? path + len - 4 : "";

    return _stricmp(ext, ".bat") == 0 || _stricmp(ext, ".cmd") == 0;
}

static int spw_is_absolute(const char *path)
{
    return path[0] == '\\' || path[0] == '/' || (path[0] && path[1] == ':');
}

static int spw_try_file(const struct spw_mem *mem, char *candidate, wchar_t **out_path)
{
    wchar_t *wide = NULL;
    DWORD attrs;
    int status;

    status = spw_to_wide(mem, candidate, &wide);
    if (status != SPW_OK)
        return status;
    attrs = GetFileAttributesW(wide);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        spw_mem_free(mem, wide);
        return SPW_ERR_NOTFOUND;
    }
    if (spw_is_batch(candidate)) {
        spw_mem_free(mem, wide);
        return SPW_ERR_UNSUPPORTED;
    }
    *out_path = wide;
    return SPW_OK;
}

static int spw_try_dir(const spw_cmd_t *cmd, const char *dir, size_t dir_len, const char *name, wchar_t **out_path)
{
    char *candidate;
    char *with_ext;
    char *full;
    int status;

    candidate = dir_len ? spw_mem_join(&cmd->mem, dir, dir_len, '\\', name) : spw_mem_strdup(&cmd->mem, name);
    if (candidate != NULL && cmd->cwd != NULL && !spw_is_absolute(candidate)) {
        full = spw_mem_join(&cmd->mem, cmd->cwd, strlen(cmd->cwd), '\\', candidate);
        spw_mem_free(&cmd->mem, candidate);
        candidate = full;
    }
    if (candidate == NULL)
        return SPW_ERR_NOMEM;
    if (!spw_has_extension(name)) {
        with_ext = spw_mem_join(&cmd->mem, candidate, strlen(candidate), '.', "exe");
        spw_mem_free(&cmd->mem, candidate);
        candidate = with_ext;
        if (candidate == NULL)
            return SPW_ERR_NOMEM;
    }
    status = spw_try_file(&cmd->mem, candidate, out_path);
    spw_mem_free(&cmd->mem, candidate);
    return status;
}

static int spw_system_dir(const struct spw_mem *mem, char **out_dir)
{
    wchar_t dir[MAX_PATH + 1];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH + 1);

    if (n == 0 || n > MAX_PATH)
        return SPW_ERR_NOTFOUND;
    return spw_to_utf8(mem, dir, out_dir);
}

static int spw_search_path(const spw_cmd_t *cmd, const char *dirs, wchar_t **out_path)
{
    const char *name = cmd->args[0];
    const char *end;
    size_t len;
    int status;

    while (dirs != NULL && *dirs) {
        end = (strchr)(dirs, ';');
        if (end == NULL)
            end = dirs + strlen(dirs);
        len = (size_t)(end - dirs);
        if (len >= 2 && dirs[0] == '"' && dirs[len - 1] == '"') {
            dirs++;
            len -= 2;
        }
        if (len > 0) {
            status = spw_try_dir(cmd, dirs, len, name, out_path);
            if (status != SPW_ERR_NOTFOUND)
                return status;
        }
        dirs = *end ? end + 1 : end;
        if (dirs[-1] == '"' && *dirs == ';')
            dirs++;
    }
    return SPW_ERR_NOTFOUND;
}

static int spw_resolve(const spw_cmd_t *cmd, char **merged, wchar_t **out_path)
{
    const char *name = cmd->args[0];
    char *sysdir = NULL;
    int status;

    if ((strpbrk)(name, "\\/:") != NULL)
        return spw_try_dir(cmd, "", 0, name, out_path);
    status = spw_search_path(cmd, spw_env_find(merged, "PATH", 1), out_path);
    if (status != SPW_ERR_NOTFOUND)
        return status;
    status = spw_system_dir(&cmd->mem, &sysdir);
    if (status != SPW_OK)
        return status;
    status = spw_try_dir(cmd, sysdir, strlen(sysdir), name, out_path);
    spw_mem_free(&cmd->mem, sysdir);
    return status;
}

static int spw_build_cmdline(const spw_cmd_t *cmd, wchar_t **out_cmdline)
{
    size_t len = 0;
    char *line;
    int status;

    status = spw_win_quote((const char *const *)cmd->args, cmd->args_len, NULL, 0, &len);
    if (status != SPW_ERR_RANGE)
        return status;
    line = spw_mem_alloc(&cmd->mem, len + 1);
    if (line == NULL)
        return SPW_ERR_NOMEM;
    status = spw_win_quote((const char *const *)cmd->args, cmd->args_len, line, len + 1, &len);
    if (status == SPW_OK)
        status = spw_to_wide(&cmd->mem, line, out_cmdline);
    spw_mem_free(&cmd->mem, line);
    return status;
}

static int spw_prepare_text(const spw_cmd_t *cmd, struct spw_wprep *prep)
{
    char **base = NULL;
    char **merged = NULL;
    size_t base_len = 0;
    int status;

    status = spw_parent_env(&cmd->mem, &base, &base_len);
    if (status == SPW_OK)
        status = spw_env_merge(cmd, base, base_len, 1, &merged);
    if (status == SPW_OK)
        status = spw_resolve(cmd, merged, &prep->path);
    if (status == SPW_OK)
        status = spw_build_cmdline(cmd, &prep->cmdline);
    if (status == SPW_OK)
        status = spw_env_block(&cmd->mem, merged, &prep->env);
    if (status == SPW_OK && cmd->cwd != NULL)
        status = spw_to_wide(&cmd->mem, cmd->cwd, &prep->cwd);
    spw_mem_free(&cmd->mem, merged);
    spw_free_strings(&cmd->mem, base, base_len);
    return status;
}

static void spw_wprep_free(const struct spw_mem *mem, struct spw_wprep *prep)
{
    int i;

    for (i = 0; i < 3; i++) {
        spw_close_handle(&prep->child[i]);
        spw_close_handle(&prep->parent[i]);
    }
    spw_mem_free(mem, prep->path);
    spw_mem_free(mem, prep->cmdline);
    spw_mem_free(mem, prep->env);
    spw_mem_free(mem, prep->cwd);
}

static int spw_make_pipe(int parent_reads, HANDLE *out_parent, HANDLE *out_child)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    char name[96];
    wchar_t wname[96];
    size_t i;

    snprintf(name, sizeof(name), "\\\\.\\pipe\\spawn-%lu-%ld-%llu", (unsigned long)GetCurrentProcessId(),
             (long)InterlockedIncrement(&spw_pipe_counter), (unsigned long long)GetTickCount64());
    for (i = 0; i < sizeof(name) && (i == 0 || name[i - 1]); i++)
        wname[i] = (wchar_t)name[i];
    *out_parent = CreateNamedPipeW(wname,
                                   (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED |
                                       FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                   SPW_WIN_CHUNK, SPW_WIN_CHUNK, 0, NULL);
    if (*out_parent == INVALID_HANDLE_VALUE) {
        *out_parent = NULL;
        return SPW_ERR_SPAWN;
    }
    *out_child = CreateFileW(wname, parent_reads ? GENERIC_WRITE : GENERIC_READ, 0, &sa, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (*out_child == INVALID_HANDLE_VALUE) {
        *out_child = NULL;
        spw_close_handle(out_parent);
        return SPW_ERR_SPAWN;
    }
    return SPW_OK;
}

static int spw_open_null(HANDLE *out_handle)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};

    *out_handle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, 0, NULL);
    if (*out_handle == INVALID_HANDLE_VALUE) {
        *out_handle = NULL;
        return SPW_ERR_SPAWN;
    }
    return SPW_OK;
}

static int spw_dup_inheritable(HANDLE source, HANDLE *out_handle)
{
    *out_handle = NULL;
    if (source == NULL || source == INVALID_HANDLE_VALUE)
        return SPW_OK;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), out_handle, 0, TRUE,
                         DUPLICATE_SAME_ACCESS))
        *out_handle = NULL;
    return SPW_OK;
}

static int spw_prepare_stream(const spw_cmd_t *cmd, struct spw_wprep *prep, int stream)
{
    static const DWORD std_ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};

    switch (cmd->modes[stream]) {
    case SPW_NULL:
        return spw_open_null(&prep->child[stream]);
    case SPW_PIPE:
        return spw_make_pipe(stream != SPW_STDIN, &prep->parent[stream], &prep->child[stream]);
    case SPW_MERGE:
        return spw_dup_inheritable(prep->child[SPW_STDOUT] ? prep->child[SPW_STDOUT] : GetStdHandle(STD_OUTPUT_HANDLE),
                                   &prep->child[stream]);
    default:
        return spw_dup_inheritable(GetStdHandle(std_ids[stream]), &prep->child[stream]);
    }
}

static int spw_prepare(const spw_cmd_t *cmd, struct spw_wprep *prep)
{
    int status;
    int i;

    memset(prep, 0, sizeof(*prep));
    status = spw_prepare_text(cmd, prep);
    for (i = 0; i < 3 && status == SPW_OK; i++)
        status = spw_prepare_stream(cmd, prep, i);
    return status;
}

static int spw_create_process(struct spw_wprep *prep, DWORD flags, PROCESS_INFORMATION *pi)
{
    STARTUPINFOEXW si;
    HANDLE list[3];
    SIZE_T attr_size = 0;
    DWORD count = 0;
    BOOL ok;
    int i;
    int j;

    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = prep->child[0];
    si.StartupInfo.hStdOutput = prep->child[1];
    si.StartupInfo.hStdError = prep->child[2];
    for (i = 0; i < 3; i++) {
        for (j = 0; j < (int)count && list[j] != prep->child[i]; j++) {
        }
        if (prep->child[i] != NULL && j == (int)count)
            list[count++] = prep->child[i];
    }
    if (count > 0) {
        InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
        si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attr_size);
        if (si.lpAttributeList == NULL)
            return SPW_ERR_NOMEM;
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) ||
            !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                                       count * sizeof(HANDLE), NULL, NULL)) {
            HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
            return SPW_ERR_SPAWN;
        }
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    ok = CreateProcessW(prep->path, prep->cmdline, NULL, NULL, count > 0, flags | CREATE_UNICODE_ENVIRONMENT,
                        prep->env, prep->cwd, &si.StartupInfo, pi);
    if (si.lpAttributeList != NULL) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    }
    if (ok)
        return SPW_OK;
    if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ||
        GetLastError() == ERROR_DIRECTORY)
        return SPW_ERR_NOTFOUND;
    return SPW_ERR_SPAWN;
}

static int spw_pipe_start(spw_proc_t *proc, int stream);

static int spw_proc_setup(const spw_cmd_t *cmd, struct spw_wprep *prep, spw_proc_t **out_proc)
{
    spw_proc_t *proc = spw_mem_alloc(&cmd->mem, sizeof(*proc));
    int i;

    if (proc == NULL)
        return SPW_ERR_NOMEM;
    memset(proc, 0, sizeof(*proc));
    proc->mem = cmd->mem;
    *out_proc = proc;
    if (cmd->input_len > 0) {
        proc->input = spw_mem_alloc(&cmd->mem, cmd->input_len);
        if (proc->input == NULL)
            return SPW_ERR_NOMEM;
        memcpy(proc->input, cmd->input, cmd->input_len);
        proc->input_len = cmd->input_len;
    }
    spw_out_init(cmd, &proc->out[0], SPW_STDOUT);
    spw_out_init(cmd, &proc->out[1], SPW_STDERR);
    for (i = 0; i < 3; i++) {
        if (prep->parent[i] == NULL)
            continue;
        proc->pipes[i].ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (proc->pipes[i].ev == NULL)
            return SPW_ERR_SPAWN;
        proc->pipes[i].h = prep->parent[i];
        prep->parent[i] = NULL;
    }
    return SPW_OK;
}

static void spw_start_job(spw_proc_t *proc, PROCESS_INFORMATION *pi)
{
    proc->job = CreateJobObjectW(NULL, NULL);
    if (proc->job != NULL && !AssignProcessToJobObject(proc->job, pi->hProcess))
        spw_close_handle(&proc->job);
    ResumeThread(pi->hThread);
    CloseHandle(pi->hThread);
    proc->process = pi->hProcess;
    proc->pid = pi->dwProcessId;
}

int spw_spawn(const spw_cmd_t *cmd, spw_proc_t **out_proc)
{
    struct spw_wprep prep;
    PROCESS_INFORMATION pi;
    spw_proc_t *proc = NULL;
    int status;
    int i;

    if (out_proc == NULL)
        return SPW_ERR_ARG;
    *out_proc = NULL;
    status = spw_cmd_check(cmd, 0);
    if (status != SPW_OK)
        return status;
    status = spw_prepare(cmd, &prep);
    if (status == SPW_OK)
        status = spw_proc_setup(cmd, &prep, &proc);
    if (status == SPW_OK)
        status = spw_create_process(&prep, CREATE_SUSPENDED, &pi);
    spw_wprep_free(&cmd->mem, &prep);
    if (status != SPW_OK) {
        spw_proc_destroy(proc);
        return status;
    }
    spw_start_job(proc, &pi);
    if (cmd->timeout_ms > 0)
        proc->deadline = GetTickCount64() + (ULONGLONG)cmd->timeout_ms;
    for (i = 0; i < 3 && status == SPW_OK; i++)
        status = spw_pipe_start(proc, i);
    if (status != SPW_OK) {
        spw_proc_destroy(proc);
        return status;
    }
    *out_proc = proc;
    return SPW_OK;
}

int spw_spawn_detached(const spw_cmd_t *cmd, long *out_pid)
{
    struct spw_wprep prep;
    PROCESS_INFORMATION pi;
    int status;

    if (out_pid != NULL)
        *out_pid = -1;
    status = spw_cmd_check(cmd, 1);
    if (status != SPW_OK)
        return status;
    status = spw_prepare(cmd, &prep);
    if (status == SPW_OK)
        status = spw_create_process(&prep, CREATE_NEW_PROCESS_GROUP, &pi);
    spw_wprep_free(&cmd->mem, &prep);
    if (status != SPW_OK)
        return status;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (out_pid != NULL)
        *out_pid = (long)pi.dwProcessId;
    return SPW_OK;
}

static void spw_pipe_close(struct spw_wpipe *pipe)
{
    DWORD n;

    if (pipe->h != NULL && pipe->pending) {
        CancelIoEx(pipe->h, &pipe->ov);
        GetOverlappedResult(pipe->h, &pipe->ov, &n, TRUE);
    }
    pipe->pending = 0;
    spw_close_handle(&pipe->h);
}

static int spw_pipe_issue(spw_proc_t *proc, int stream)
{
    struct spw_wpipe *pipe = &proc->pipes[stream];
    size_t left = proc->input_len - proc->input_off;
    BOOL ok;

    memset(&pipe->ov, 0, sizeof(pipe->ov));
    pipe->ov.hEvent = pipe->ev;
    if (stream == SPW_STDIN)
        ok = WriteFile(pipe->h, proc->input + proc->input_off, (DWORD)(left > SPW_WIN_CHUNK ? SPW_WIN_CHUNK : left),
                       NULL, &pipe->ov);
    else
        ok = ReadFile(pipe->h, pipe->buf, SPW_WIN_CHUNK, NULL, &pipe->ov);
    if (ok || GetLastError() == ERROR_IO_PENDING) {
        pipe->pending = 1;
        return 1;
    }
    spw_pipe_close(pipe);
    return 0;
}

static int spw_pipe_start(spw_proc_t *proc, int stream)
{
    if (proc->pipes[stream].h == NULL)
        return SPW_OK;
    if (stream == SPW_STDIN && proc->input_off >= proc->input_len) {
        spw_pipe_close(&proc->pipes[stream]);
        return SPW_OK;
    }
    spw_pipe_issue(proc, stream);
    return SPW_OK;
}

static int spw_pipe_complete(spw_proc_t *proc, int stream)
{
    struct spw_wpipe *pipe = &proc->pipes[stream];
    struct spw_out *out;
    DWORD n = 0;

    if (pipe->h == NULL || !pipe->pending || !HasOverlappedIoCompleted(&pipe->ov))
        return SPW_OK;
    pipe->pending = 0;
    if (!GetOverlappedResult(pipe->h, &pipe->ov, &n, FALSE)) {
        spw_pipe_close(pipe);
        return SPW_OK;
    }
    if (stream == SPW_STDIN) {
        proc->input_off += n;
        return spw_pipe_start(proc, stream);
    }
    out = &proc->out[stream - 1];
    if (spw_out_put(&proc->mem, out, pipe->buf, n) != SPW_OK)
        return SPW_ERR_NOMEM;
    return spw_pipe_start(proc, stream);
}

static void spw_check_exit(spw_proc_t *proc)
{
    DWORD code;

    if (proc->exited || WaitForSingleObject(proc->process, 0) != WAIT_OBJECT_0)
        return;
    proc->exited = 1;
    proc->exit_code = GetExitCodeProcess(proc->process, &code) ? (int)code : -1;
}

static int spw_done(spw_proc_t *proc)
{
    if (!proc->exited)
        return 0;
    spw_pipe_close(&proc->pipes[SPW_STDIN]);
    if (proc->timed_out || proc->stopped) {
        spw_pipe_complete(proc, SPW_STDOUT);
        spw_pipe_complete(proc, SPW_STDERR);
        spw_pipe_close(&proc->pipes[SPW_STDOUT]);
        spw_pipe_close(&proc->pipes[SPW_STDERR]);
    }
    return proc->pipes[SPW_STDOUT].h == NULL && proc->pipes[SPW_STDERR].h == NULL;
}

static DWORD spw_wait_ms(const spw_proc_t *proc, ULONGLONG now, long long limit)
{
    long long wait = limit;

    if (proc->deadline > 0 && !proc->timed_out && (wait < 0 || (long long)(proc->deadline - now) < wait))
        wait = (long long)(proc->deadline - now);
    if (wait < 0)
        return INFINITE;
    return wait > 1000000 ? 1000000 : (DWORD)wait;
}

static int spw_wait_once(spw_proc_t *proc, DWORD wait_ms)
{
    HANDLE handles[4];
    DWORD n = 0;
    int status = SPW_OK;
    int s;

    for (s = 0; s < 3; s++)
        if (proc->pipes[s].h != NULL && proc->pipes[s].pending)
            handles[n++] = proc->pipes[s].ev;
    if (!proc->exited)
        handles[n++] = proc->process;
    if (n == 0)
        return SPW_OK;
    WaitForMultipleObjects(n, handles, FALSE, wait_ms);
    for (s = 0; s < 3 && status == SPW_OK; s++)
        status = spw_pipe_complete(proc, s);
    return status;
}

int spw_proc_kill(spw_proc_t *proc)
{
    if (proc == NULL)
        return SPW_ERR_ARG;
    spw_check_exit(proc);
    if (!proc->exited)
        proc->killed = 1;
    if (proc->job != NULL)
        TerminateJobObject(proc->job, 1);
    if (!proc->exited)
        TerminateProcess(proc->process, 1);
    return SPW_OK;
}

int spw_proc_terminate(spw_proc_t *proc)
{
    if (proc == NULL)
        return SPW_ERR_ARG;
    return SPW_ERR_UNSUPPORTED;
}

int spw_proc_wait(spw_proc_t *proc, long timeout_ms)
{
    ULONGLONG start = GetTickCount64();
    ULONGLONG now = start;
    long long limit;
    int status;

    if (proc == NULL)
        return SPW_ERR_ARG;
    for (;;) {
        spw_check_exit(proc);
        if (spw_done(proc))
            return proc->stopped ? SPW_ERR_STOPPED : proc->timed_out ? SPW_ERR_TIMEOUT : SPW_OK;
        if (proc->deadline > 0 && !proc->timed_out && now >= proc->deadline) {
            proc->timed_out = 1;
            spw_proc_kill(proc);
            WaitForSingleObject(proc->process, INFINITE);
            continue;
        }
        if (timeout_ms >= 0 && (long long)(now - start) >= timeout_ms)
            return SPW_ERR_AGAIN;
        limit = timeout_ms < 0 ? -1 : timeout_ms - (long long)(now - start);
        status = spw_wait_once(proc, spw_wait_ms(proc, now, limit));
        if (status != SPW_OK)
            return status;
        if (!proc->stopped && (proc->out[0].stopped || proc->out[1].stopped)) {
            proc->stopped = 1;
            spw_proc_kill(proc);
            WaitForSingleObject(proc->process, INFINITE);
        }
        now = GetTickCount64();
    }
}

void spw_proc_destroy(spw_proc_t *proc)
{
    int i;

    if (proc == NULL)
        return;
    if (proc->process != NULL && !proc->exited) {
        spw_proc_kill(proc);
        WaitForSingleObject(proc->process, INFINITE);
    }
    for (i = 0; i < 3; i++) {
        spw_pipe_close(&proc->pipes[i]);
        spw_close_handle(&proc->pipes[i].ev);
    }
    spw_close_handle(&proc->job);
    spw_close_handle(&proc->process);
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
        *out_signal = 0;
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
        return proc->exited ? SPW_NO_HANDLE : proc->process;
    return proc->pipes[which].h != NULL ? proc->pipes[which].ev : SPW_NO_HANDLE;
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
