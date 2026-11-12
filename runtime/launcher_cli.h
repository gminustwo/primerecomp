// Command-line setup shared with the start screen, without initializing a window.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool prepare, play, preload, help;
    char game[4096], keys[4096], title_keys[4096], root[4096], data_root[4096];
} PrimeLaunchOptions;

extern bool prime_headless;
// 0 = parsed, 2 = invalid arguments. Input paths become absolute before rt_paths_init changes cwd.
int prime_cli_parse(int argc, char **argv, PrimeLaunchOptions *out);
void prime_cli_help(void);
// The same preparation used by the start screen; rt_paths_init must have run. stdout is progress only.
int prime_prepare(const char *source, const char *keys, const char *title_keys);
bool prime_game_ready(const char *root);
#ifdef __cplusplus
}
#endif
