#ifndef SPAWN_INTERNAL_H
#define SPAWN_INTERNAL_H

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0601
#endif

#include "cmdspawn/cmdspawn.h"

#include <stddef.h>

struct spw_mem {
    spw_alloc_fn alloc_fn;
    spw_free_fn free_fn;
    void *alloc_ctx;
};

struct spw_env_mod {
    char *entry;
    size_t name_len;
    int unset;
};

struct spw_cmd {
    struct spw_mem mem;
    char **args;
    size_t args_len;
    size_t args_cap;
    struct spw_env_mod *env;
    size_t env_len;
    size_t env_cap;
    int env_clear;
    char *cwd;
    int modes[3];
    unsigned char *input;
    size_t input_len;
    long timeout_ms;
};

struct spw_out {
    unsigned char *data;
    size_t len;
    size_t cap;
};

void *spw_mem_alloc(const struct spw_mem *mem, size_t size);
void spw_mem_free(const struct spw_mem *mem, void *ptr);
char *spw_mem_strdup(const struct spw_mem *mem, const char *str);
char *spw_mem_join(const struct spw_mem *mem, const char *a, size_t a_len, char sep, const char *b);
int spw_out_reserve(const struct spw_mem *mem, struct spw_out *out, size_t extra);
int spw_env_merge(const spw_cmd_t *cmd, char **base, size_t base_len, int fold_case, char ***out_list);
const char *spw_env_find(char **list, const char *name, int fold_case);
int spw_cmd_check(const spw_cmd_t *cmd, int detached);

#endif
