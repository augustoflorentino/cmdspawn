#include <cmdspawn/cmdspawn.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *self[] = {argv[0], "--hello"};
    const char *const *args = argc > 1 ? (const char *const *)argv + 1 : self;
    size_t args_len = argc > 1 ? (size_t)argc - 1 : 2;
    spw_cmd_t *cmd = NULL;
    spw_proc_t *proc = NULL;
    const unsigned char *out;
    size_t out_len = 0;
    int code = 0;
    int status;

    if (argc == 2 && strcmp(argv[1], "--hello") == 0) {
        printf("hello from the child\n");
        return 0;
    }
    status = spw_cmd_create(NULL, NULL, NULL, &cmd);
    if (status == SPW_OK)
        status = spw_cmd_args(cmd, args, args_len);
    if (status == SPW_OK)
        status = spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE);
    if (status == SPW_OK)
        status = spw_cmd_timeout(cmd, 5000);
    if (status == SPW_OK)
        status = spw_spawn(cmd, &proc);
    if (status == SPW_OK)
        status = spw_proc_wait(proc, -1);
    if (status == SPW_OK)
        status = spw_proc_status(proc, &code, NULL);
    if (status == SPW_OK) {
        out = spw_proc_output(proc, SPW_STDOUT, &out_len);
        printf("exit code %d, output: %.*s", code, (int)out_len, (const char *)out);
    }
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
    if (status != SPW_OK) {
        fprintf(stderr, "error: %s\n", spw_strerror(status));
        return 1;
    }
    return 0;
}
