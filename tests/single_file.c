#define SPW_IMPLEMENTATION
#include "cmdspawn/cmdspawn.h"

#include <string.h>

int main(void)
{
    const char *args[] = {"prog", "a b", "c\"d"};
    char line[64];
    size_t len = 0;
    spw_cmd_t *cmd = NULL;
    int status;

    status = spw_win_quote(args, 3, line, sizeof(line), &len);
    if (status != SPW_OK || strcmp(line, "prog \"a b\" \"c\\\"d\"") != 0)
        return 1;
    status = spw_cmd_create(NULL, NULL, NULL, &cmd);
    if (status == SPW_OK)
        status = spw_cmd_args(cmd, args, 1);
    spw_cmd_destroy(cmd);
    return status == SPW_OK && spw_strerror(status) != NULL ? 0 : 1;
}
