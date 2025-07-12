#ifndef WIN_ARGV_REF_H
#define WIN_ARGV_REF_H

#include <stdlib.h>
#include <string.h>

#define REF_UCRT 0
#define REF_SHELL32 1

typedef struct {
    char **args;
    size_t args_len;
} ref_argv_t;

static void ref_argv_free(ref_argv_t *argv)
{
    size_t i;

    for (i = 0; i < argv->args_len; i++)
        free(argv->args[i]);
    free(argv->args);
    argv->args = NULL;
    argv->args_len = 0;
}

static char *ref_push_arg(ref_argv_t *argv, size_t cap)
{
    char *arg = calloc(cap + 1, 1);
    char **grown = realloc(argv->args, (argv->args_len + 1) * sizeof(char *));

    if (arg == NULL || grown == NULL) {
        free(arg);
        if (grown != NULL)
            argv->args = grown;
        return NULL;
    }
    argv->args = grown;
    argv->args[argv->args_len++] = arg;
    return arg;
}

static int ref_is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static const char *ref_program(const char *p, char *out)
{
    size_t n = 0;

    if (*p == '"') {
        for (p++; *p && *p != '"'; p++)
            out[n++] = *p;
        if (*p == '"')
            p++;
    } else {
        for (; *p && !ref_is_blank(*p); p++)
            out[n++] = *p;
    }
    out[n] = '\0';
    return p;
}

static const char *ref_one_arg(const char *p, char *out, int variant)
{
    size_t n = 0;
    size_t slashes;
    int quoted = 0;

    while (*p && (quoted || !ref_is_blank(*p))) {
        for (slashes = 0; *p == '\\'; p++)
            slashes++;
        if (*p != '"') {
            memset(out + n, '\\', slashes);
            n += slashes;
            if (*p && (quoted || !ref_is_blank(*p)))
                out[n++] = *p++;
            continue;
        }
        memset(out + n, '\\', slashes / 2);
        n += slashes / 2;
        if (slashes % 2) {
            out[n++] = *p++;
            continue;
        }
        if (quoted && p[1] == '"') {
            out[n++] = '"';
            p += 2;
            if (variant == REF_SHELL32)
                quoted = 0;
            continue;
        }
        quoted = !quoted;
        p++;
    }
    out[n] = '\0';
    return p;
}

static int ref_parse(const char *line, int variant, ref_argv_t *out_argv)
{
    size_t cap = strlen(line);
    const char *p = line;
    char *arg;

    out_argv->args = NULL;
    out_argv->args_len = 0;
    arg = ref_push_arg(out_argv, cap);
    if (arg == NULL)
        return -1;
    p = ref_program(p, arg);
    for (;;) {
        while (ref_is_blank(*p))
            p++;
        if (*p == '\0')
            return 0;
        arg = ref_push_arg(out_argv, cap);
        if (arg == NULL) {
            ref_argv_free(out_argv);
            return -1;
        }
        p = ref_one_arg(p, arg, variant);
    }
}

#endif
