#include "spawn_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void *spw_default_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void spw_default_free(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
}

void *spw_mem_alloc(const struct spw_mem *mem, size_t size)
{
    return mem->alloc_fn(mem->alloc_ctx, size ? size : 1);
}

void spw_mem_free(const struct spw_mem *mem, void *ptr)
{
    if (ptr != NULL)
        mem->free_fn(mem->alloc_ctx, ptr);
}

char *spw_mem_strdup(const struct spw_mem *mem, const char *str)
{
    size_t len = strlen(str);
    char *copy = spw_mem_alloc(mem, len + 1);

    if (copy != NULL)
        memcpy(copy, str, len + 1);
    return copy;
}

char *spw_mem_join(const struct spw_mem *mem, const char *a, size_t a_len, char sep, const char *b)
{
    size_t b_len = strlen(b);
    char *joined;

    if (a_len > SIZE_MAX - b_len - 2)
        return NULL;
    joined = spw_mem_alloc(mem, a_len + b_len + 2);
    if (joined == NULL)
        return NULL;
    memcpy(joined, a, a_len);
    joined[a_len] = sep;
    memcpy(joined + a_len + 1, b, b_len + 1);
    return joined;
}

int spw_out_reserve(const struct spw_mem *mem, struct spw_out *out, size_t extra)
{
    size_t cap;
    unsigned char *data;

    if (extra > SIZE_MAX - out->len)
        return SPW_ERR_NOMEM;
    if (out->len + extra <= out->cap)
        return SPW_OK;
    cap = out->cap ? out->cap : 4096;
    while (cap < out->len + extra)
        cap = cap > SIZE_MAX / 2 ? out->len + extra : cap * 2;
    data = spw_mem_alloc(mem, cap);
    if (data == NULL)
        return SPW_ERR_NOMEM;
    if (out->len > 0)
        memcpy(data, out->data, out->len);
    spw_mem_free(mem, out->data);
    out->data = data;
    out->cap = cap;
    return SPW_OK;
}

int spw_out_put(const struct spw_mem *mem, struct spw_out *out, const unsigned char *data, size_t data_len)
{
    size_t room = data_len;
    int status;

    if (out->stopped) {
        out->dropped += data_len;
        return SPW_OK;
    }
    if (out->fn != NULL) {
        out->stopped = out->fn(out->fn_ctx, out->stream, data, data_len) != 0;
        return SPW_OK;
    }
    if (out->limit > 0)
        room = out->len >= out->limit ? 0 : out->limit - out->len;
    if (room > data_len)
        room = data_len;
    out->dropped += data_len - room;
    if (room == 0)
        return SPW_OK;
    status = spw_out_reserve(mem, out, room);
    if (status != SPW_OK)
        return status;
    memcpy(out->data + out->len, data, room);
    out->len += room;
    return SPW_OK;
}

static int spw_grow(const struct spw_mem *mem, void **items, size_t item_size, size_t len, size_t *cap)
{
    size_t new_cap;
    void *grown;

    if (len < *cap)
        return SPW_OK;
    new_cap = *cap ? *cap * 2 : 8;
    if (new_cap > SIZE_MAX / item_size)
        return SPW_ERR_NOMEM;
    grown = spw_mem_alloc(mem, new_cap * item_size);
    if (grown == NULL)
        return SPW_ERR_NOMEM;
    if (len > 0)
        memcpy(grown, *items, len * item_size);
    spw_mem_free(mem, *items);
    *items = grown;
    *cap = new_cap;
    return SPW_OK;
}

int spw_cmd_create(spw_alloc_fn alloc_fn, spw_free_fn free_fn, void *alloc_ctx, spw_cmd_t **out_cmd)
{
    spw_cmd_t *cmd;

    if (out_cmd == NULL || (alloc_fn == NULL) != (free_fn == NULL))
        return SPW_ERR_ARG;
    *out_cmd = NULL;
    if (alloc_fn == NULL) {
        alloc_fn = spw_default_alloc;
        free_fn = spw_default_free;
    }
    cmd = alloc_fn(alloc_ctx, sizeof(*cmd));
    if (cmd == NULL)
        return SPW_ERR_NOMEM;
    memset(cmd, 0, sizeof(*cmd));
    cmd->mem.alloc_fn = alloc_fn;
    cmd->mem.free_fn = free_fn;
    cmd->mem.alloc_ctx = alloc_ctx;
    *out_cmd = cmd;
    return SPW_OK;
}

void spw_cmd_destroy(spw_cmd_t *cmd)
{
    size_t i;

    if (cmd == NULL)
        return;
    for (i = 0; i < cmd->args_len; i++)
        spw_mem_free(&cmd->mem, cmd->args[i]);
    for (i = 0; i < cmd->env_len; i++)
        spw_mem_free(&cmd->mem, cmd->env[i].entry);
    spw_mem_free(&cmd->mem, cmd->args);
    spw_mem_free(&cmd->mem, cmd->env);
    spw_mem_free(&cmd->mem, cmd->cwd);
    spw_mem_free(&cmd->mem, cmd->input);
    spw_mem_free(&cmd->mem, cmd);
}

static int spw_cmd_add_arg(spw_cmd_t *cmd, const char *arg)
{
    int status = spw_grow(&cmd->mem, (void **)&cmd->args, sizeof(char *), cmd->args_len, &cmd->args_cap);

    if (status != SPW_OK)
        return status;
    cmd->args[cmd->args_len] = spw_mem_strdup(&cmd->mem, arg);
    if (cmd->args[cmd->args_len] == NULL)
        return SPW_ERR_NOMEM;
    cmd->args_len++;
    return SPW_OK;
}

int spw_cmd_args(spw_cmd_t *cmd, const char *const *args, size_t args_len)
{
    size_t start;
    size_t i;
    int status = SPW_OK;

    if (cmd == NULL || (args == NULL && args_len > 0))
        return SPW_ERR_ARG;
    for (i = 0; i < args_len; i++)
        if (args[i] == NULL)
            return SPW_ERR_ARG;
    start = cmd->args_len;
    for (i = 0; i < args_len && status == SPW_OK; i++)
        status = spw_cmd_add_arg(cmd, args[i]);
    while (status != SPW_OK && cmd->args_len > start)
        spw_mem_free(&cmd->mem, cmd->args[--cmd->args_len]);
    return status;
}

static int spw_name_equal(const char *a, const char *b, size_t len, int fold_case)
{
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];

        if (fold_case && ca >= 'a' && ca <= 'z')
            ca = (unsigned char)(ca - 'a' + 'A');
        if (fold_case && cb >= 'a' && cb <= 'z')
            cb = (unsigned char)(cb - 'a' + 'A');
        if (ca != cb)
            return 0;
    }
    return 1;
}

static int spw_fold_case(void)
{
#if defined(_WIN32)
    return 1;
#else
    return 0;
#endif
}

static size_t spw_env_name_len(const char *entry)
{
    const char *eq = entry[0] ? (strchr)(entry + 1, '=') : NULL;

    return eq ? (size_t)(eq - entry) : strlen(entry);
}

static struct spw_env_mod *spw_env_lookup(spw_cmd_t *cmd, const char *name, size_t name_len)
{
    size_t i;

    for (i = 0; i < cmd->env_len; i++)
        if (cmd->env[i].name_len == name_len && spw_name_equal(cmd->env[i].entry, name, name_len, spw_fold_case()))
            return &cmd->env[i];
    return NULL;
}

int spw_cmd_env(spw_cmd_t *cmd, const char *name, const char *value)
{
    size_t name_len;
    struct spw_env_mod *mod;
    char *entry;
    int status;

    if (cmd == NULL || name == NULL || name[0] == '\0' || (strchr)(name + 1, '=') != NULL)
        return SPW_ERR_ARG;
    name_len = strlen(name);
    entry = value ? spw_mem_join(&cmd->mem, name, name_len, '=', value) : spw_mem_strdup(&cmd->mem, name);
    if (entry == NULL)
        return SPW_ERR_NOMEM;
    mod = spw_env_lookup(cmd, name, name_len);
    if (mod == NULL) {
        status = spw_grow(&cmd->mem, (void **)&cmd->env, sizeof(*cmd->env), cmd->env_len, &cmd->env_cap);
        if (status != SPW_OK) {
            spw_mem_free(&cmd->mem, entry);
            return status;
        }
        mod = &cmd->env[cmd->env_len++];
        mod->entry = NULL;
    }
    spw_mem_free(&cmd->mem, mod->entry);
    mod->entry = entry;
    mod->name_len = name_len;
    mod->unset = value == NULL;
    return SPW_OK;
}

int spw_cmd_env_clear(spw_cmd_t *cmd)
{
    size_t i;

    if (cmd == NULL)
        return SPW_ERR_ARG;
    for (i = 0; i < cmd->env_len; i++)
        spw_mem_free(&cmd->mem, cmd->env[i].entry);
    cmd->env_len = 0;
    cmd->env_clear = 1;
    return SPW_OK;
}

int spw_cmd_cwd(spw_cmd_t *cmd, const char *dir)
{
    char *copy = NULL;

    if (cmd == NULL)
        return SPW_ERR_ARG;
    if (dir != NULL) {
        copy = spw_mem_strdup(&cmd->mem, dir);
        if (copy == NULL)
            return SPW_ERR_NOMEM;
    }
    spw_mem_free(&cmd->mem, cmd->cwd);
    cmd->cwd = copy;
    return SPW_OK;
}

int spw_cmd_stdio(spw_cmd_t *cmd, int stream, int mode)
{
    if (cmd == NULL || stream < SPW_STDIN || stream > SPW_STDERR || mode < SPW_INHERIT || mode > SPW_MERGE)
        return SPW_ERR_ARG;
    if (mode == SPW_MERGE && stream != SPW_STDERR)
        return SPW_ERR_ARG;
    cmd->modes[stream] = mode;
    if (stream == SPW_STDIN) {
        spw_mem_free(&cmd->mem, cmd->input);
        cmd->input = NULL;
        cmd->input_len = 0;
    }
    return SPW_OK;
}

int spw_cmd_input(spw_cmd_t *cmd, const unsigned char *data, size_t data_len)
{
    unsigned char *copy = NULL;

    if (cmd == NULL || (data == NULL && data_len > 0))
        return SPW_ERR_ARG;
    if (data_len > 0) {
        copy = spw_mem_alloc(&cmd->mem, data_len);
        if (copy == NULL)
            return SPW_ERR_NOMEM;
        memcpy(copy, data, data_len);
    }
    spw_mem_free(&cmd->mem, cmd->input);
    cmd->input = copy;
    cmd->input_len = data_len;
    cmd->modes[SPW_STDIN] = SPW_PIPE;
    return SPW_OK;
}

int spw_cmd_timeout(spw_cmd_t *cmd, long timeout_ms)
{
    if (cmd == NULL)
        return SPW_ERR_ARG;
    cmd->timeout_ms = timeout_ms > 0 ? timeout_ms : 0;
    return SPW_OK;
}

int spw_cmd_same_group(spw_cmd_t *cmd, int enabled)
{
    if (cmd == NULL)
        return SPW_ERR_ARG;
    cmd->same_group = enabled != 0;
    return SPW_OK;
}

int spw_cmd_kill_grace(spw_cmd_t *cmd, long grace_ms)
{
    if (cmd == NULL)
        return SPW_ERR_ARG;
    cmd->grace_ms = grace_ms > 0 ? grace_ms : 0;
    return SPW_OK;
}

int spw_cmd_output_limit(spw_cmd_t *cmd, size_t max_bytes)
{
    if (cmd == NULL)
        return SPW_ERR_ARG;
    cmd->output_limit = max_bytes;
    return SPW_OK;
}

int spw_cmd_output_fn(spw_cmd_t *cmd, spw_output_fn output_fn, void *output_ctx)
{
    if (cmd == NULL)
        return SPW_ERR_ARG;
    cmd->output_fn = output_fn;
    cmd->output_ctx = output_ctx;
    return SPW_OK;
}

void spw_out_init(const spw_cmd_t *cmd, struct spw_out *out, int stream)
{
    out->limit = cmd->output_limit;
    out->fn = cmd->output_fn;
    out->fn_ctx = cmd->output_ctx;
    out->stream = stream;
}

int spw_cmd_check(const spw_cmd_t *cmd, int detached)
{
    int i;

    if (cmd == NULL || cmd->args_len == 0 || cmd->args[0][0] == '\0')
        return SPW_ERR_ARG;
    for (i = 0; detached && i < 3; i++)
        if (cmd->modes[i] == SPW_PIPE || (cmd->modes[i] == SPW_MERGE && cmd->modes[SPW_STDOUT] == SPW_PIPE))
            return SPW_ERR_ARG;
    return SPW_OK;
}

static int spw_env_overridden(const spw_cmd_t *cmd, const char *entry, int fold_case)
{
    size_t name_len = spw_env_name_len(entry);
    size_t i;

    for (i = 0; i < cmd->env_len; i++)
        if (cmd->env[i].name_len == name_len && spw_name_equal(cmd->env[i].entry, entry, name_len, fold_case))
            return 1;
    return 0;
}

int spw_env_merge(const spw_cmd_t *cmd, char **base, size_t base_len, int fold_case, char ***out_list)
{
    char **list;
    size_t count = 0;
    size_t i;

    if (base_len > SIZE_MAX / sizeof(char *) - cmd->env_len - 1)
        return SPW_ERR_NOMEM;
    list = spw_mem_alloc(&cmd->mem, (base_len + cmd->env_len + 1) * sizeof(char *));
    if (list == NULL)
        return SPW_ERR_NOMEM;
    for (i = 0; !cmd->env_clear && i < base_len; i++)
        if (!spw_env_overridden(cmd, base[i], fold_case))
            list[count++] = base[i];
    for (i = 0; i < cmd->env_len; i++)
        if (!cmd->env[i].unset)
            list[count++] = cmd->env[i].entry;
    list[count] = NULL;
    *out_list = list;
    return SPW_OK;
}

const char *spw_env_find(char **list, const char *name, int fold_case)
{
    size_t name_len = strlen(name);
    size_t i;

    for (i = 0; list[i] != NULL; i++)
        if (spw_env_name_len(list[i]) == name_len && spw_name_equal(list[i], name, name_len, fold_case))
            return list[i] + name_len + 1;
    return NULL;
}

static void spw_quote_put(char *buf, size_t buf_len, size_t *pos, char c, size_t count)
{
    while (count-- > 0) {
        if (*pos < buf_len)
            buf[*pos] = c;
        (*pos)++;
    }
}

static int spw_quote_needed(const char *arg)
{
    return arg[0] == '\0' || (strpbrk)(arg, " \t\n\v\"") != NULL;
}

static int spw_quote_program(const char *arg, char *buf, size_t buf_len, size_t *pos)
{
    int quote = arg[0] == '\0' || (strpbrk)(arg, " \t") != NULL;

    if ((strchr)(arg, '"') != NULL)
        return SPW_ERR_ARG;
    spw_quote_put(buf, buf_len, pos, '"', (size_t)quote);
    for (; *arg; arg++)
        spw_quote_put(buf, buf_len, pos, *arg, 1);
    spw_quote_put(buf, buf_len, pos, '"', (size_t)quote);
    return SPW_OK;
}

static void spw_quote_arg(const char *arg, char *buf, size_t buf_len, size_t *pos)
{
    size_t slashes = 0;

    if (!spw_quote_needed(arg)) {
        for (; *arg; arg++)
            spw_quote_put(buf, buf_len, pos, *arg, 1);
        return;
    }
    spw_quote_put(buf, buf_len, pos, '"', 1);
    for (; *arg; arg++) {
        if (*arg == '\\') {
            slashes++;
            continue;
        }
        if (*arg == '"')
            slashes = slashes * 2 + 1;
        spw_quote_put(buf, buf_len, pos, '\\', slashes);
        spw_quote_put(buf, buf_len, pos, *arg, 1);
        slashes = 0;
    }
    spw_quote_put(buf, buf_len, pos, '\\', slashes * 2);
    spw_quote_put(buf, buf_len, pos, '"', 1);
}

int spw_win_quote(const char *const *args, size_t args_len, char *buf, size_t buf_len, size_t *out_len)
{
    size_t pos = 0;
    size_t i;
    int status;

    if (args == NULL || args_len == 0 || (buf == NULL && buf_len > 0) || out_len == NULL)
        return SPW_ERR_ARG;
    for (i = 0; i < args_len; i++)
        if (args[i] == NULL)
            return SPW_ERR_ARG;
    status = spw_quote_program(args[0], buf, buf_len, &pos);
    if (status != SPW_OK)
        return status;
    for (i = 1; i < args_len; i++) {
        spw_quote_put(buf, buf_len, &pos, ' ', 1);
        spw_quote_arg(args[i], buf, buf_len, &pos);
    }
    *out_len = pos;
    if (pos >= buf_len)
        return SPW_ERR_RANGE;
    buf[pos] = '\0';
    return SPW_OK;
}

const char *spw_strerror(int status)
{
    switch (status) {
    case SPW_OK:
        return "ok";
    case SPW_ERR_ARG:
        return "invalid argument";
    case SPW_ERR_NOMEM:
        return "out of memory";
    case SPW_ERR_NOTFOUND:
        return "program not found";
    case SPW_ERR_SPAWN:
        return "the system could not start the program";
    case SPW_ERR_IO:
        return "pipe input/output failed";
    case SPW_ERR_AGAIN:
        return "process still running";
    case SPW_ERR_TIMEOUT:
        return "time limit reached, process tree killed";
    case SPW_ERR_ENCODING:
        return "text is not valid UTF-8";
    case SPW_ERR_RANGE:
        return "buffer too small";
    case SPW_ERR_UNSUPPORTED:
        return "not supported on this system";
    case SPW_ERR_ACCESS:
        return "permission denied";
    case SPW_ERR_FORMAT:
        return "not an executable this system can run";
    case SPW_ERR_STOPPED:
        return "stopped by the output callback, process tree killed";
    default:
        return "unknown status";
    }
}
