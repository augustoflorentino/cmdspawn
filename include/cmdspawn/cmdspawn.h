#ifndef CMDSPAWN_H
#define CMDSPAWN_H

#include <stddef.h>

#if defined(SPW_STATIC) && defined(__GNUC__)
#define SPW_API static __attribute__((unused))
#elif defined(SPW_STATIC)
#define SPW_API static
#elif defined(_WIN32) && defined(SPW_BUILD_SHARED)
#define SPW_API __declspec(dllexport)
#elif defined(_WIN32) && defined(SPW_SHARED)
#define SPW_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(SPW_BUILD_SHARED)
#define SPW_API __attribute__((visibility("default")))
#else
#define SPW_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SPW_OK 0
#define SPW_ERR_ARG 1
#define SPW_ERR_NOMEM 2
#define SPW_ERR_NOTFOUND 3
#define SPW_ERR_SPAWN 4
#define SPW_ERR_IO 5
#define SPW_ERR_AGAIN 6
#define SPW_ERR_TIMEOUT 7
#define SPW_ERR_ENCODING 8
#define SPW_ERR_RANGE 9
#define SPW_ERR_UNSUPPORTED 10

#define SPW_STDIN 0
#define SPW_STDOUT 1
#define SPW_STDERR 2
#define SPW_PROCESS 3

#define SPW_INHERIT 0
#define SPW_NULL 1
#define SPW_PIPE 2
#define SPW_MERGE 3

#if defined(_WIN32)
typedef void *spw_handle_t;
#define SPW_NO_HANDLE ((spw_handle_t)0)
#else
typedef int spw_handle_t;
#define SPW_NO_HANDLE (-1)
#endif

typedef struct spw_cmd spw_cmd_t;
typedef struct spw_proc spw_proc_t;

typedef void *(*spw_alloc_fn)(void *ctx, size_t size);
typedef void (*spw_free_fn)(void *ctx, void *ptr);

SPW_API int spw_cmd_create(spw_alloc_fn alloc_fn, spw_free_fn free_fn, void *alloc_ctx, spw_cmd_t **out_cmd);
SPW_API void spw_cmd_destroy(spw_cmd_t *cmd);
SPW_API int spw_cmd_args(spw_cmd_t *cmd, const char *const *args, size_t args_len);
SPW_API int spw_cmd_env(spw_cmd_t *cmd, const char *name, const char *value);
SPW_API int spw_cmd_env_clear(spw_cmd_t *cmd);
SPW_API int spw_cmd_cwd(spw_cmd_t *cmd, const char *dir);
SPW_API int spw_cmd_stdio(spw_cmd_t *cmd, int stream, int mode);
SPW_API int spw_cmd_input(spw_cmd_t *cmd, const unsigned char *data, size_t data_len);
SPW_API int spw_cmd_timeout(spw_cmd_t *cmd, long timeout_ms);

SPW_API int spw_spawn(const spw_cmd_t *cmd, spw_proc_t **out_proc);
SPW_API int spw_spawn_detached(const spw_cmd_t *cmd);

SPW_API void spw_proc_destroy(spw_proc_t *proc);
SPW_API int spw_proc_wait(spw_proc_t *proc, long timeout_ms);
SPW_API int spw_proc_kill(spw_proc_t *proc);
SPW_API int spw_proc_status(const spw_proc_t *proc, int *out_exit_code, int *out_signal);
SPW_API long spw_proc_pid(const spw_proc_t *proc);
SPW_API spw_handle_t spw_proc_handle(const spw_proc_t *proc, int which);
SPW_API const unsigned char *spw_proc_output(const spw_proc_t *proc, int stream, size_t *out_len);

SPW_API int spw_win_quote(const char *const *args, size_t args_len, char *buf, size_t buf_len, size_t *out_len);
SPW_API const char *spw_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif
