#include <cmdspawn/cmdspawn.h>

#include <stdio.h>

int main(int argc, char **argv)
{
    spw_cmd_t *cmd = NULL;
    spw_proc_t *proc = NULL;
    size_t seen = 0;
    size_t len = 0;
    int ticks = 0;
    int status;

    if (argc < 2) {
        fprintf(stderr, "usage: %s program [args...]\n", argv[0]);
        return 2;
    }
    status = spw_cmd_create(NULL, NULL, NULL, &cmd);
    if (status == SPW_OK)
        status = spw_cmd_args(cmd, (const char *const *)argv + 1, (size_t)argc - 1);
    if (status == SPW_OK)
        status = spw_cmd_stdio(cmd, SPW_STDOUT, SPW_PIPE);
    if (status == SPW_OK)
        status = spw_spawn(cmd, &proc);
    while (status == SPW_OK || status == SPW_ERR_AGAIN) {
        status = spw_proc_wait(proc, 100);
        spw_proc_output(proc, SPW_STDOUT, &len);
        if (len > seen)
            printf("tick %d: %zu new bytes\n", ticks, len - seen);
        seen = len;
        ticks++;
        if (status == SPW_OK)
            break;
    }
    spw_proc_destroy(proc);
    spw_cmd_destroy(cmd);
    if (status != SPW_OK) {
        fprintf(stderr, "error: %s\n", spw_strerror(status));
        return 1;
    }
    return 0;
}
