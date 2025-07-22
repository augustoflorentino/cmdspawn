#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "cmdspawn/cmdspawn.h"
#include "win_argv_ref.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                    \
        }                                                               \
    } while (0)

static const char *helper;

typedef struct {
    int remaining;
    int live;
} failing_alloc_t;

static void *failing_alloc(void *ctx, size_t size)
{
    failing_alloc_t *fa = ctx;

    if (fa->remaining-- == 0)
        return NULL;
    fa->live++;
    return malloc(size);
}

static void failing_free(void *ctx, void *ptr)
{
    failing_alloc_t *fa = ctx;

    fa->live--;
    free(ptr);
}

static unsigned long long rng_state = 0x9e3779b97f4a7c15ULL;

static unsigned rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned)(rng_state >> 32);
}

static void pause_ms(long ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static long now_ms(void)
{
#if defined(_WIN32)
    return (long)GetTickCount64();
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

static char *quote(const char *const *args, size_t args_len)
{
    size_t len = 0;
    char *line;

    CHECK(spw_win_quote(args, args_len, NULL, 0, &len) == SPW_ERR_RANGE);
    line = malloc(len + 1);
    CHECK(line != NULL);
    CHECK(spw_win_quote(args, args_len, line, len + 1, &len) == SPW_OK);
    CHECK(strlen(line) == len);
    return line;
}

static void check_round_trip(const char *const *args, size_t args_len)
{
    char *line = quote(args, args_len);
    int variant;
    size_t i;

    for (variant = REF_UCRT; variant <= REF_SHELL32; variant++) {
        ref_argv_t parsed;

        CHECK(ref_parse(line, variant, &parsed) == 0);
        if (parsed.args_len != args_len)
            fprintf(stderr, "line: [%s]\n", line);
        CHECK(parsed.args_len == args_len);
        for (i = 0; i < args_len; i++)
            CHECK(strcmp(parsed.args[i], args[i]) == 0);
        ref_argv_free(&parsed);
    }
    free(line);
}

static void test_win_quote_known(void)
{
    static const struct {
        const char *args[4];
        size_t args_len;
        const char *line;
    } cases[] = {
        {{"prog", "a", "b c"}, 3, "prog a \"b c\""},
        {{"prog", ""}, 2, "prog \"\""},
        {{"prog", "a\"b"}, 2, "prog \"a\\\"b\""},
        {{"prog", "a\\b"}, 2, "prog a\\b"},
        {{"prog", "a\\ b\\"}, 2, "prog \"a\\ b\\\\\""},
        {{"prog", "a\\\"b"}, 2, "prog \"a\\\\\\\"b\""},
        {{"prog", "\\\\"}, 2, "prog \\\\"},
        {{"prog", "x y\\\\"}, 2, "prog \"x y\\\\\\\\\""},
        {{"C:\\Program Files\\x.exe", "1"}, 2, "\"C:\\Program Files\\x.exe\" 1"},
        {{"C:\\dir\\", "\t"}, 2, "C:\\dir\\ \"\t\""},
        {{"", "a"}, 2, "\"\" a"},
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *line = quote(cases[i].args, cases[i].args_len);

        CHECK(strcmp(line, cases[i].line) == 0);
        free(line);
        check_round_trip(cases[i].args, cases[i].args_len);
    }
}

static void test_win_quote_errors(void)
{
    const char *bad_program[] = {"a\"b", "x"};
    const char *ok[] = {"prog", "abc"};
    const char *with_null[] = {"prog", NULL};
    char small[4];
    size_t len = 0;

    CHECK(spw_win_quote(bad_program, 2, NULL, 0, &len) == SPW_ERR_ARG);
    CHECK(spw_win_quote(with_null, 2, NULL, 0, &len) == SPW_ERR_ARG);
    CHECK(spw_win_quote(ok, 0, NULL, 0, &len) == SPW_ERR_ARG);
    CHECK(spw_win_quote(NULL, 1, NULL, 0, &len) == SPW_ERR_ARG);
    CHECK(spw_win_quote(ok, 2, NULL, 4, &len) == SPW_ERR_ARG);
    CHECK(spw_win_quote(ok, 2, NULL, 0, NULL) == SPW_ERR_ARG);
    CHECK(spw_win_quote(ok, 2, small, sizeof(small), &len) == SPW_ERR_RANGE);
    CHECK(len == 8);
}

static void random_arg(char *arg, size_t cap)
{
    static const char *pieces[] = {"a", " ", "\t", "\"", "\\", "\\\\", "\\\"", "\xc3\xa9", "\xf0\x9f\x8e\xae", "\n", "x"};
    size_t n = rng() % 8;
    size_t len = 0;
    size_t i;

    arg[0] = '\0';
    for (i = 0; i < n; i++) {
        const char *piece = pieces[rng() % (sizeof(pieces) / sizeof(pieces[0]))];

        if (len + strlen(piece) + 1 > cap)
            break;
        strcpy(arg + len, piece);
        len += strlen(piece);
    }
}

static void test_win_quote_random(void)
{
    char storage[6][64];
    const char *args[6];
    int round;
    size_t i;
    size_t n;

    for (round = 0; round < 200000; round++) {
        n = 1 + rng() % 6;
        strcpy(storage[0], (rng() % 2) ? "prog" : "C:\\Program Files\\p q\\");
        args[0] = storage[0];
        for (i = 1; i < n; i++) {
            random_arg(storage[i], sizeof(storage[i]));
            args[i] = storage[i];
        }
        check_round_trip(args, n);
    }
}

#if defined(_WIN32)
static void test_win_quote_system(void)
{
    char storage[6][64];
    const char *args[6];
    int round;
    size_t i;
    size_t n;

    for (round = 0; round < 20000; round++) {
        wchar_t wline[2048];
        LPWSTR *wargs;
        int wargc;
        char *line;

        n = 1 + rng() % 6;
        strcpy(storage[0], (rng() % 2) ? "prog" : "C:\\Program Files\\p q\\");
        args[0] = storage[0];
        for (i = 1; i < n; i++) {
            random_arg(storage[i], sizeof(storage[i]));
            args[i] = storage[i];
        }
        line = quote(args, n);
        CHECK(MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, 2048) > 0);
        wargs = CommandLineToArgvW(wline, &wargc);
        CHECK(wargs != NULL && (size_t)wargc == n);
        for (i = 0; i < n; i++) {
            char back[512];

            CHECK(WideCharToMultiByte(CP_UTF8, 0, wargs[i], -1, back, sizeof(back), NULL, NULL) > 0);
            CHECK(strcmp(back, args[i]) == 0);
        }
        LocalFree(wargs);
        free(line);
    }
}
#endif

static spw_cmd_t *helper_cmd(const char *mode, const char *arg)
{
    const char *args[3];
    spw_cmd_t *cmd = NULL;

    args[0] = helper;
    args[1] = mode;
    args[2] = arg;
    CHECK(spw_cmd_create(NULL, NULL, NULL, &cmd) == SPW_OK);
    CHECK(spw_cmd_args(cmd, args, arg ? 3 : 2) == SPW_OK);
    return cmd;
}

static spw_proc_t *run_cmd(spw_cmd_t *cmd, int expected)
{
    spw_proc_t *proc = NULL;

    CHECK(spw_spawn(cmd, &proc) == SPW_OK);
    CHECK(proc != NULL);
    CHECK(spw_proc_wait(proc, -1) == expected);
    return proc;
}

static int output_is(const spw_proc_t *proc, int stream, const char *text)
{
    size_t len = 0;
    const unsigned char *data = spw_proc_output(proc, stream, &len);

    return len == strlen(text) && (len == 0 || memcmp(data, text, len) == 0);
}

static void hex_line(const char *arg, char *out)
{
    for (; *arg; arg++, out += 2)
        sprintf(out, "%02x", (unsigned char)*arg);
    strcpy(out, "\n");
}

static void test_argv_round_trip(void)
{
    static const char *const cases[] = {"plain", "", "with space", "tab\there", "quote\"inside", "\"",
                                        "\\", "trail\\", "trail space\\", "\\\\\"", "a\\\\b c",
                                        "\xc3\xa7\xc3\xa3o", "\xf0\x9f\x8e\xae pad", "--flag=\"x y\"",
                                        "%PATH%", "$HOME", "*", "a;b|c&d<e>f", "''"};
    size_t n = sizeof(cases) / sizeof(cases[0]);
    const char *args[32];
    char expected[4096] = "";
    spw_cmd_t *cmd = NULL;
    spw_proc_t *proc;
    size_t i;

    args[0] = helper;
    args[1] = "argv";
    for (i = 0; i < n; i++) {
        args[i + 2] = cases[i];
        hex_line(cases[i], expected + strlen(expected));
    }
    CHECK(spw_cmd_create(NULL, NULL, NULL, &cmd) == SPW_OK);
    CHECK(spw_cmd_args(cmd, args, n + 2) == SPW_OK);
    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    proc = run_cmd(cmd, SPW_OK);
#if defined(_WIN32)
    {
        char both[8200];

        snprintf(both, sizeof(both), "%s--\n%s", expected, expected);
        CHECK(output_is(proc, SPW_STDOUT, both));
    }
#else
    CHECK(output_is(proc, SPW_STDOUT, expected));
#endif
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
}

static void test_exit_code(void)
{
    spw_cmd_t *cmd = helper_cmd("exit", "7");
    spw_proc_t *proc = run_cmd(cmd, SPW_OK);
    int code = -2;
    int sig = -2;

    CHECK(spw_proc_status(proc, &code, &sig) == SPW_OK);
    CHECK(code == 7 && sig == 0);
    CHECK(spw_proc_pid(proc) > 0);
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
}

#if !defined(_WIN32)
static void test_signal(void)
{
    spw_cmd_t *cmd = helper_cmd("signal", "15");
    spw_proc_t *proc = run_cmd(cmd, SPW_OK);
    int code = 0;
    int sig = 0;

    CHECK(spw_proc_status(proc, &code, &sig) == SPW_OK);
    CHECK(code == -1 && sig == 15);
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
}
#endif

static void test_stdin_round_trip(void)
{
    size_t len = 3 * 1024 * 1024 + 17;
    unsigned char *data = malloc(len);
    spw_cmd_t *cmd = helper_cmd("cat", NULL);
    spw_proc_t *proc;
    const unsigned char *out;
    size_t out_len = 0;
    size_t i;

    CHECK(data != NULL);
    for (i = 0; i < len; i++)
        data[i] = (unsigned char)(rng() & 0xff);
    CHECK(spw_cmd_input(cmd, data, len) == SPW_OK);
    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    proc = run_cmd(cmd, SPW_OK);
    out = spw_proc_output(proc, SPW_STDOUT, &out_len);
    CHECK(out_len == len && memcmp(out, data, len) == 0);
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
    free(data);
}

static void test_empty_input(void)
{
    spw_cmd_t *cmd = helper_cmd("cat", NULL);
    spw_proc_t *proc;

    CHECK(spw_cmd_input(cmd, NULL, 0) == SPW_OK);
    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    proc = run_cmd(cmd, SPW_OK);
    CHECK(output_is(proc, SPW_STDOUT, ""));
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
}

static void test_flood(void)
{
    spw_cmd_t *cmd = helper_cmd("flood", "8000000");
    spw_proc_t *proc;
    size_t out_len = 0;
    size_t err_len = 0;
    const unsigned char *out;
    const unsigned char *err;

    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    CHECK(spw_cmd_stdio(cmd, SPW_STDERR, SPW_PIPE) == SPW_OK);
    proc = run_cmd(cmd, SPW_OK);
    out = spw_proc_output(proc, SPW_STDOUT, &out_len);
    err = spw_proc_output(proc, SPW_STDERR, &err_len);
    CHECK(out_len == 8000000 && err_len == 8000000);
    CHECK(out[0] == 'o' && out[out_len - 1] == 'o' && err[0] == 'e' && err[err_len - 1] == 'e');
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
}

static void test_merge(void)
{
    spw_cmd_t *cmd = helper_cmd("outerr", NULL);
    spw_proc_t *proc;

    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    CHECK(spw_cmd_stdio(cmd, SPW_STDERR, SPW_MERGE) == SPW_OK);
    proc = run_cmd(cmd, SPW_OK);
    CHECK(output_is(proc, SPW_STDOUT, "out\nerr\n"));
    CHECK(output_is(proc, SPW_STDERR, ""));
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
}

static void test_null_stdio(void)
{
    spw_cmd_t *cmd = helper_cmd("outerr", NULL);
    spw_proc_t *proc;

    CHECK(spw_cmd_stdio(cmd, SPW_STDIN, SPW_NULL) == SPW_OK);
    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_NULL) == SPW_OK);
    CHECK(spw_cmd_stdio(cmd, SPW_STDERR, SPW_NULL) == SPW_OK);
    proc = run_cmd(cmd, SPW_OK);
    CHECK(output_is(proc, SPW_STDOUT, ""));
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
}

static void check_env(spw_cmd_t *cmd, const char *expected)
{
    spw_proc_t *proc;

    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    proc = run_cmd(cmd, SPW_OK);
    CHECK(output_is(proc, SPW_STDOUT, expected));
    spw_proc_destroy(proc);
}

static void test_env(void)
{
    spw_cmd_t *cmd = helper_cmd("env", "SPW_TEST_VAR");

    check_env(cmd, "<unset>");
    CHECK(spw_cmd_env(cmd, "SPW_TEST_VAR", "first") == SPW_OK);
    CHECK(spw_cmd_env(cmd, "SPW_TEST_VAR", "va l=ue \xc3\xa9") == SPW_OK);
    check_env(cmd, "va l=ue \xc3\xa9");
    CHECK(spw_cmd_env(cmd, "SPW_TEST_VAR", NULL) == SPW_OK);
    check_env(cmd, "<unset>");
    CHECK(spw_cmd_env(cmd, "", "x") == SPW_ERR_ARG);
    CHECK(spw_cmd_env(cmd, "A=B", "x") == SPW_ERR_ARG);
    spw_cmd_destroy(cmd);

    cmd = helper_cmd("env", "PATH");
#if defined(_WIN32)
    CHECK(spw_cmd_env(cmd, "path", "lower") == SPW_OK);
    check_env(cmd, "lower");
#endif
    CHECK(spw_cmd_env_clear(cmd) == SPW_OK);
    CHECK(spw_cmd_env(cmd, "SPW_ONLY", "1") == SPW_OK);
    check_env(cmd, "<unset>");
    spw_cmd_destroy(cmd);
}

static void test_cwd(void)
{
#if defined(_WIN32)
    const char *dir = "C:\\";
#else
    const char *dir = "/";
#endif
    spw_cmd_t *cmd = helper_cmd("cwd", NULL);
    spw_proc_t *proc;

    CHECK(spw_cmd_cwd(cmd, dir) == SPW_OK);
    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    proc = run_cmd(cmd, SPW_OK);
    CHECK(output_is(proc, SPW_STDOUT, dir));
    spw_proc_destroy(proc);
    CHECK(spw_cmd_cwd(cmd, "/spw/missing/dir") == SPW_OK);
    CHECK(spw_spawn(cmd, &proc) != SPW_OK);
    CHECK(proc == NULL);
    CHECK(spw_cmd_cwd(cmd, NULL) == SPW_OK);
    spw_cmd_destroy(cmd);
}

static int process_gone(long pid)
{
#if defined(_WIN32)
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    DWORD r;

    if (h == NULL)
        return 1;
    r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_OBJECT_0;
#else
    return kill((pid_t)pid, 0) != 0 && errno == ESRCH;
#endif
}

static void test_timeout_kills_tree(void)
{
    spw_cmd_t *cmd = helper_cmd("tree", "30000");
    spw_proc_t *proc;
    long start = now_ms();
    long grandchild;
    size_t len = 0;
    const unsigned char *out;
    int i;

    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    CHECK(spw_cmd_timeout(cmd, 500) == SPW_OK);
    proc = run_cmd(cmd, SPW_ERR_TIMEOUT);
    CHECK(now_ms() - start < 10000);
    CHECK(spw_proc_wait(proc, -1) == SPW_ERR_TIMEOUT);
    out = spw_proc_output(proc, SPW_STDOUT, &len);
    CHECK(len > 0);
    grandchild = strtol((const char *)out, NULL, 10);
    CHECK(grandchild > 0);
    for (i = 0; i < 100 && !process_gone(grandchild); i++)
        pause_ms(50);
    CHECK(process_gone(grandchild));
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
}

static void test_poll_and_kill(void)
{
    spw_cmd_t *cmd = helper_cmd("sleep", "30000");
    spw_proc_t *proc = NULL;
    int code = 0;
    int sig = 0;

    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    CHECK(spw_spawn(cmd, &proc) == SPW_OK);
    CHECK(spw_proc_wait(proc, 0) == SPW_ERR_AGAIN);
    CHECK(spw_proc_wait(proc, 50) == SPW_ERR_AGAIN);
    CHECK(spw_proc_status(proc, &code, &sig) == SPW_ERR_AGAIN);
    CHECK(spw_proc_handle(proc, SPW_STDOUT) != SPW_NO_HANDLE);
    CHECK(spw_proc_handle(proc, SPW_STDERR) == SPW_NO_HANDLE);
    CHECK(spw_proc_handle(proc, 9) == SPW_NO_HANDLE);
#if defined(_WIN32) || defined(__linux__)
    CHECK(spw_proc_handle(proc, SPW_PROCESS) != SPW_NO_HANDLE);
#endif
    CHECK(spw_proc_kill(proc) == SPW_OK);
    CHECK(spw_proc_wait(proc, -1) == SPW_OK);
    CHECK(spw_proc_status(proc, &code, &sig) == SPW_OK);
#if defined(_WIN32)
    CHECK(code == 1 && sig == 0);
#else
    CHECK(code == -1 && sig == SIGKILL);
#endif
    CHECK(spw_proc_handle(proc, SPW_STDOUT) == SPW_NO_HANDLE);
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
}

static void test_destroy_running(void)
{
    spw_cmd_t *cmd = helper_cmd("sleep", "30000");
    spw_proc_t *proc = NULL;
    long start = now_ms();

    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    CHECK(spw_spawn(cmd, &proc) == SPW_OK);
    spw_proc_destroy(proc);
    CHECK(now_ms() - start < 10000);
    spw_cmd_destroy(cmd);
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "r");

    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

static void test_detached(void)
{
    char path[512];
    spw_cmd_t *cmd;
    int i;

    snprintf(path, sizeof(path), "%s.detached", helper);
    remove(path);
    cmd = helper_cmd("touch", path);
    CHECK(spw_cmd_stdio(cmd, SPW_STDIN, SPW_NULL) == SPW_OK);
    CHECK(spw_spawn_detached(cmd) == SPW_OK);
#if !defined(_WIN32)
    CHECK(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
#endif
    for (i = 0; i < 200 && !file_exists(path); i++)
        pause_ms(25);
    CHECK(file_exists(path));
    remove(path);
    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    CHECK(spw_spawn_detached(cmd) == SPW_ERR_ARG);
    spw_cmd_destroy(cmd);
}

static void test_not_found(void)
{
    const char *missing[] = {"spw-surely-missing-program"};
    const char *missing_path[] = {"./spw/surely/missing"};
    spw_cmd_t *cmd = NULL;
    spw_proc_t *proc = NULL;

    CHECK(spw_cmd_create(NULL, NULL, NULL, &cmd) == SPW_OK);
    CHECK(spw_spawn(cmd, &proc) == SPW_ERR_ARG);
    CHECK(spw_cmd_args(cmd, missing, 1) == SPW_OK);
    CHECK(spw_spawn(cmd, &proc) == SPW_ERR_NOTFOUND);
    CHECK(spw_spawn_detached(cmd) == SPW_ERR_NOTFOUND);
    spw_cmd_destroy(cmd);
    CHECK(spw_cmd_create(NULL, NULL, NULL, &cmd) == SPW_OK);
    CHECK(spw_cmd_args(cmd, missing_path, 1) == SPW_OK);
    CHECK(spw_spawn(cmd, &proc) == SPW_ERR_NOTFOUND);
    CHECK(proc == NULL);
    spw_cmd_destroy(cmd);
}

static void test_path_search(void)
{
#if defined(_WIN32)
    const char *args[] = {"cmd", "/c", "exit 3"};
#else
    const char *args[] = {"sh", "-c", "exit 3"};
#endif
    spw_cmd_t *cmd = NULL;
    spw_proc_t *proc;
    int code = 0;

    CHECK(spw_cmd_create(NULL, NULL, NULL, &cmd) == SPW_OK);
    CHECK(spw_cmd_args(cmd, args, 3) == SPW_OK);
    proc = run_cmd(cmd, SPW_OK);
    CHECK(spw_proc_status(proc, &code, NULL) == SPW_OK);
    CHECK(code == 3);
    spw_proc_destroy(proc);
    CHECK(spw_cmd_env(cmd, "PATH", "/spw/nowhere") == SPW_OK);
#if !defined(_WIN32)
    CHECK(spw_spawn(cmd, &proc) == SPW_ERR_NOTFOUND);
#endif
    spw_cmd_destroy(cmd);
}

#if !defined(_WIN32)
static void test_relative_path_and_cwd(void)
{
    char saved[4096];
    char dir[4096];
    const char *args[2];
    char *base;
    char *parent;
    spw_cmd_t *cmd = NULL;
    spw_proc_t *proc;

    CHECK(getcwd(saved, sizeof(saved)) != NULL);
    CHECK(helper[0] == '/' && strlen(helper) < sizeof(dir));
    strcpy(dir, helper);
    base = strrchr(dir, '/');
    *base++ = '\0';
    parent = strrchr(dir, '/');
    CHECK(parent != NULL && parent != dir);
    *parent++ = '\0';
    args[0] = base;
    args[1] = "cwd";
    CHECK(spw_cmd_create(NULL, NULL, NULL, &cmd) == SPW_OK);
    CHECK(spw_cmd_args(cmd, args, 2) == SPW_OK);
    CHECK(spw_cmd_env(cmd, "PATH", parent) == SPW_OK);
    CHECK(spw_cmd_cwd(cmd, dir + 1) == SPW_OK);
    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE) == SPW_OK);
    CHECK(chdir("/") == 0);
    proc = run_cmd(cmd, SPW_OK);
    CHECK(chdir(saved) == 0);
    CHECK(output_is(proc, SPW_STDOUT, dir));
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
}
#endif

static void test_bad_args(void)
{
    spw_cmd_t *cmd = NULL;
    const char *with_null[] = {"a", NULL};

    CHECK(spw_cmd_create(failing_alloc, NULL, NULL, &cmd) == SPW_ERR_ARG);
    CHECK(spw_cmd_create(NULL, NULL, NULL, NULL) == SPW_ERR_ARG);
    CHECK(spw_cmd_args(NULL, NULL, 0) == SPW_ERR_ARG);
    CHECK(spw_cmd_env(NULL, "a", "b") == SPW_ERR_ARG);
    CHECK(spw_cmd_env_clear(NULL) == SPW_ERR_ARG);
    CHECK(spw_cmd_cwd(NULL, "/") == SPW_ERR_ARG);
    CHECK(spw_cmd_stdio(NULL, SPW_STDOUT, SPW_PIPE) == SPW_ERR_ARG);
    CHECK(spw_cmd_input(NULL, NULL, 0) == SPW_ERR_ARG);
    CHECK(spw_cmd_timeout(NULL, 1) == SPW_ERR_ARG);
    CHECK(spw_spawn(NULL, NULL) == SPW_ERR_ARG);
    CHECK(spw_spawn_detached(NULL) == SPW_ERR_ARG);
    CHECK(spw_proc_wait(NULL, 0) == SPW_ERR_ARG);
    CHECK(spw_proc_kill(NULL) == SPW_ERR_ARG);
    CHECK(spw_proc_status(NULL, NULL, NULL) == SPW_ERR_ARG);
    CHECK(spw_proc_pid(NULL) == -1);
    CHECK(spw_proc_handle(NULL, SPW_STDOUT) == SPW_NO_HANDLE);
    CHECK(spw_proc_output(NULL, SPW_STDOUT, NULL) == NULL);
    CHECK(spw_cmd_create(NULL, NULL, NULL, &cmd) == SPW_OK);
    CHECK(spw_cmd_args(cmd, with_null, 2) == SPW_ERR_ARG);
    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, SPW_MERGE) == SPW_ERR_ARG);
    CHECK(spw_cmd_stdio(cmd, SPW_PROCESS, SPW_PIPE) == SPW_ERR_ARG);
    CHECK(spw_cmd_stdio(cmd, SPW_STDOUT, 7) == SPW_ERR_ARG);
    CHECK(spw_cmd_input(cmd, NULL, 3) == SPW_ERR_ARG);
    spw_cmd_destroy(cmd);
    spw_cmd_destroy(NULL);
    spw_proc_destroy(NULL);
    CHECK(strcmp(spw_strerror(SPW_ERR_TIMEOUT), spw_strerror(-1)) != 0);
    CHECK(spw_strerror(-1) != NULL);
}

static int run_with_failure(failing_alloc_t *fa)
{
    static const unsigned char input[] = "spawn under allocation failure";
    const char *args[2];
    spw_cmd_t *cmd = NULL;
    spw_proc_t *proc = NULL;
    int status;

    args[0] = helper;
    args[1] = "cat";
    status = spw_cmd_create(failing_alloc, failing_free, fa, &cmd);
    if (status == SPW_OK)
        status = spw_cmd_args(cmd, args, 2);
    if (status == SPW_OK)
        status = spw_cmd_env(cmd, "SPW_TEST_VAR", "1");
    if (status == SPW_OK)
        status = spw_cmd_input(cmd, input, sizeof(input) - 1);
    if (status == SPW_OK)
        status = spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE);
    if (status == SPW_OK)
        status = spw_spawn(cmd, &proc);
    if (status == SPW_OK)
        status = spw_proc_wait(proc, -1);
    if (status == SPW_OK && !output_is(proc, SPW_STDOUT, (const char *)input))
        status = SPW_ERR_IO;
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
    return status;
}

static void test_alloc_failure(void)
{
    int n;

    for (n = 0;; n++) {
        failing_alloc_t fa = {n, 0};
        int status = run_with_failure(&fa);

        CHECK(fa.live == 0);
        if (status == SPW_OK)
            break;
        CHECK(status == SPW_ERR_NOMEM);
    }
    CHECK(n > 5);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    helper = argv[1];
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_DFL);
#endif
    test_bad_args();
    test_win_quote_known();
    test_win_quote_errors();
    test_win_quote_random();
#if defined(_WIN32)
    test_win_quote_system();
#endif
    test_argv_round_trip();
    test_exit_code();
#if !defined(_WIN32)
    test_signal();
#endif
    test_stdin_round_trip();
    test_empty_input();
    test_flood();
    test_merge();
    test_null_stdio();
    test_env();
    test_cwd();
    test_path_search();
#if !defined(_WIN32)
    test_relative_path_and_cwd();
#endif
    test_not_found();
    test_poll_and_kill();
    test_destroy_running();
    test_timeout_kills_tree();
    test_detached();
    test_alloc_failure();
    return 0;
}
