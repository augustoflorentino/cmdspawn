#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#include <wchar.h>
#else
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_hex(const char *arg)
{
    for (; *arg; arg++)
        printf("%02x", (unsigned char)*arg);
    printf("\n");
}

static void pause_ms(long ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) {
    }
#endif
}

static int cmd_cat(void)
{
    char buf[4096];
    size_t n;

    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0)
        fwrite(buf, 1, n, stdout);
    return 0;
}

static int cmd_lines(void)
{
    char line[256];

    while (fgets(line, sizeof(line), stdin) != NULL) {
        fputs(line, stdout);
        fflush(stdout);
    }
    return 0;
}

static int cmd_flood(long total)
{
    static char outs[4096];
    static char errs[4096];
    long done;

    memset(outs, 'o', sizeof(outs));
    memset(errs, 'e', sizeof(errs));
    for (done = 0; done < total; done += (long)sizeof(outs)) {
        size_t n = total - done < (long)sizeof(outs) ? (size_t)(total - done) : sizeof(outs);

        fwrite(outs, 1, n, stdout);
        fwrite(errs, 1, n, stderr);
    }
    return 0;
}

static int cmd_outerr(void)
{
    printf("out\n");
    fflush(stdout);
    fprintf(stderr, "err\n");
    return 0;
}

#if defined(_WIN32)
static int cmd_env(const char *name)
{
    wchar_t wname[256];
    wchar_t wvalue[4096];
    char value[8192];

    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
    if (GetEnvironmentVariableW(wname, wvalue, 4096) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        printf("<unset>");
        return 0;
    }
    WideCharToMultiByte(CP_UTF8, 0, wvalue, -1, value, sizeof(value), NULL, NULL);
    printf("%s", value);
    return 0;
}
#else
static int cmd_env(const char *name)
{
    const char *value = getenv(name);

    printf("%s", value ? value : "<unset>");
    return 0;
}
#endif

static int cmd_touch(const char *path)
{
    char tmp[1024];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (f == NULL)
        return 1;
#if defined(_WIN32)
    fprintf(f, "%lu\n", (unsigned long)GetCurrentProcessId());
#else
    fprintf(f, "%ld\n", (long)getpid());
#endif
    fclose(f);
    remove(path);
    return rename(tmp, path) != 0;
}

#if defined(_WIN32)

static void print_wide_hex(const wchar_t *arg)
{
    char buf[4096];

    WideCharToMultiByte(CP_UTF8, 0, arg, -1, buf, sizeof(buf), NULL, NULL);
    print_hex(buf);
}

static int cmd_argv_win(int argc, wchar_t **argv)
{
    LPWSTR *shell_argv;
    int shell_argc;
    int i;

    for (i = 2; i < argc; i++)
        print_wide_hex(argv[i]);
    printf("--\n");
    shell_argv = CommandLineToArgvW(GetCommandLineW(), &shell_argc);
    for (i = 2; shell_argv != NULL && i < shell_argc; i++)
        print_wide_hex(shell_argv[i]);
    LocalFree(shell_argv);
    return 0;
}

static int cmd_cwd(void)
{
    wchar_t dir[1024];
    char buf[4096];

    GetCurrentDirectoryW(1024, dir);
    WideCharToMultiByte(CP_UTF8, 0, dir, -1, buf, sizeof(buf), NULL, NULL);
    printf("%s", buf);
    return 0;
}

static int cmd_tree(long ms)
{
    wchar_t self[1024];
    wchar_t line[1200];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    GetModuleFileNameW(NULL, self, 1024);
    swprintf(line, 1200, L"\"%ls\" sleep %ld", self, ms);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(self, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 1;
    printf("%lu\n", (unsigned long)pi.dwProcessId);
    fflush(stdout);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    pause_ms(ms);
    return 0;
}

static int run(int argc, char **argv);

int wmain(int argc, wchar_t **wargv)
{
    char **argv = calloc((size_t)argc + 1, sizeof(char *));
    int i;

    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    if (argc > 1 && wcscmp(wargv[1], L"argv") == 0)
        return cmd_argv_win(argc, wargv);
    for (i = 0; i < argc; i++) {
        argv[i] = malloc(4096);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], 4096, NULL, NULL);
    }
    return run(argc, argv);
}

#else

static int cmd_cwd(void)
{
    char dir[4096];

    if (getcwd(dir, sizeof(dir)) == NULL)
        return 1;
    printf("%s", dir);
    return 0;
}

static int cmd_tree(long ms)
{
    pid_t pid = fork();

    if (pid < 0)
        return 1;
    if (pid == 0) {
        pause_ms(ms);
        _exit(0);
    }
    printf("%ld\n", (long)pid);
    fflush(stdout);
    pause_ms(ms);
    return 0;
}

static void on_term(int sig)
{
    static const char bye[] = "bye\n";

    (void)sig;
    if (write(1, bye, sizeof(bye) - 1) < 0)
        _exit(6);
    _exit(5);
}

static int cmd_graceful(void)
{
    signal(SIGTERM, on_term);
    printf("ready\n");
    fflush(stdout);
    pause_ms(30000);
    return 0;
}

static int cmd_stubborn(void)
{
    signal(SIGTERM, SIG_IGN);
    printf("ready\n");
    fflush(stdout);
    pause_ms(30000);
    return 0;
}

static int run(int argc, char **argv);

int main(int argc, char **argv)
{
    int i;

    if (argc > 1 && strcmp(argv[1], "argv") == 0) {
        for (i = 2; i < argc; i++)
            print_hex(argv[i]);
        return 0;
    }
    return run(argc, argv);
}

#endif

static int run(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    long n = argc > 2 ? strtol(argv[2], NULL, 10) : 0;

    if (strcmp(mode, "exit") == 0)
        return (int)n;
    if (strcmp(mode, "lines") == 0)
        return cmd_lines();
    if (strcmp(mode, "cat") == 0)
        return cmd_cat();
    if (strcmp(mode, "flood") == 0)
        return cmd_flood(n);
    if (strcmp(mode, "outerr") == 0)
        return cmd_outerr();
    if (strcmp(mode, "env") == 0 && argc > 2)
        return cmd_env(argv[2]);
    if (strcmp(mode, "cwd") == 0)
        return cmd_cwd();
    if (strcmp(mode, "sleep") == 0) {
        pause_ms(n);
        return 0;
    }
    if (strcmp(mode, "tree") == 0)
        return cmd_tree(n);
    if (strcmp(mode, "touch") == 0 && argc > 2)
        return cmd_touch(argv[2]);
#if !defined(_WIN32)
    if (strcmp(mode, "graceful") == 0)
        return cmd_graceful();
    if (strcmp(mode, "stubborn") == 0)
        return cmd_stubborn();
    if (strcmp(mode, "pgid") == 0) {
        printf("%ld", (long)getpgrp());
        return 0;
    }
    if (strcmp(mode, "signal") == 0) {
        signal((int)n, SIG_DFL);
        raise((int)n);
    }
#endif
    return 99;
}
