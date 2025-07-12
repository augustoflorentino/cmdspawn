#include "cmdspawn/cmdspawn.h"
#include "win_argv_ref.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t split_args(char *text, size_t text_len, const char **args, size_t cap)
{
    size_t n = 0;
    size_t start = 0;
    size_t i;

    for (i = 0; i <= text_len && n < cap; i++) {
        if (i < text_len && text[i] != '\0')
            continue;
        args[n++] = text + start;
        start = i + 1;
    }
    return n;
}

static void check_variant(const char *line, const char **args, size_t args_len, int variant)
{
    ref_argv_t parsed;
    size_t i;

    if (ref_parse(line, variant, &parsed) != 0)
        return;
    if (parsed.args_len != args_len)
        abort();
    for (i = 0; i < args_len; i++)
        if (strcmp(parsed.args[i], args[i]) != 0)
            abort();
    ref_argv_free(&parsed);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const char *args[64];
    char *text = malloc(size + 1);
    char *line = NULL;
    size_t args_len;
    size_t len = 0;

    if (text == NULL)
        return 0;
    memcpy(text, data, size);
    text[size] = '\0';
    args_len = split_args(text, size, args, 64);
    if (spw_win_quote(args, args_len, NULL, 0, &len) == SPW_ERR_RANGE)
        line = malloc(len + 1);
    if (line != NULL && spw_win_quote(args, args_len, line, len + 1, &len) == SPW_OK) {
        check_variant(line, args, args_len, REF_UCRT);
        check_variant(line, args, args_len, REF_SHELL32);
    }
    free(line);
    free(text);
    return 0;
}
