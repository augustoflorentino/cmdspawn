#include <cmdspawn/cmdspawn.h>

#include <stdio.h>

int main(int argc, char **argv)
{
    spw_cmd_t *cmd = NULL;
    int status;

    if (argc < 2) {
        fprintf(stderr, "usage: %s program [args...]\n", argv[0]);
        return 2;
    }
    status = spw_cmd_create(NULL, NULL, NULL, &cmd);
    if (status == SPW_OK)
        status = spw_cmd_args(cmd, (const char *const *)argv + 1, (size_t)argc - 1);
    if (status == SPW_OK)
        status = spw_cmd_env(cmd, "PADBRIDGE_EVENT", "connected");
    if (status == SPW_OK)
        status = spw_cmd_stdio(cmd, SPW_STDIN, SPW_NULL);
    if (status == SPW_OK)
        status = spw_spawn_detached(cmd);
    spw_cmd_destroy(cmd);
    if (status != SPW_OK) {
        fprintf(stderr, "error: %s\n", spw_strerror(status));
        return 1;
    }
    return 0;
}
