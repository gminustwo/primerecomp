#include "launcher_cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool prime_headless;

static bool path_arg(const char *value, char *out, size_t cap) {
    if (!value || !*value) return false;
#ifdef _WIN32
    return _fullpath(out, value, cap) != NULL;
#else
    char *full = realpath(value, NULL);
    if (!full) {
        char cwd[4096];
        if (!getcwd(cwd, sizeof cwd)) return false;
        int n = value[0] == '/' ? snprintf(out, cap, "%s", value) : snprintf(out, cap, "%s/%s", cwd, value);
        return n >= 0 && (size_t)n < cap;
    }
    bool fits = strlen(full) < cap;
    if (fits) memcpy(out, full, strlen(full) + 1);
    free(full);
    return fits;
#endif
}

int prime_cli_parse(int argc, char **argv, PrimeLaunchOptions *o) {
    memset(o, 0, sizeof *o);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *value = NULL;
        char *dest = NULL;
        if (!strcmp(a, "--prepare")) o->prepare = true;
        else if (!strcmp(a, "--play")) o->play = true;
        else if (!strcmp(a, "--preload-shaders")) o->preload = true;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) o->help = true;
        else {
            static const char *const names[] = {"--game", "--keys", "--title-keys", "--data-root"};
            char *paths[] = {o->game, o->keys, o->title_keys, o->data_root};
            for (int k = 0; k < 4; k++) {
                size_t n = strlen(names[k]);
                if (!strncmp(a, names[k], n) && (a[n] == '=' || !a[n])) {
                    value = a[n] == '=' ? a + n + 1 : i + 1 < argc ? argv[++i] : NULL;
                    dest = paths[k];
                    break;
                }
            }
            if (!dest && a[0] != '-' && !o->root[0]) dest = o->root, value = a;
            if (!dest || dest[0] || !path_arg(value, dest, sizeof o->game)) {
                fprintf(stderr, "Invalid argument: %s. See --help.\n", a);
                return 2;
            }
        }
    }
    if (o->help) return 0;
    if ((o->prepare && (!o->game[0] || o->play || o->preload || o->root[0])) ||
        (!o->prepare && (o->game[0] || o->keys[0] || o->title_keys[0]))) {
        fprintf(stderr, "Use --prepare --game=<path> [--keys=<path>] [--title-keys=<path>].\n");
        return 2;
    }
    for (int k = 0; k < 3; k++) {
        const char *path = k == 0 ? o->game : k == 1 ? o->keys : o->title_keys;
        struct stat st;
        if (*path && (stat(path, &st) || (k ? !S_ISREG(st.st_mode) : !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)))) {
            fprintf(stderr, "The selected %s is unavailable.\n", k ? "keys file" : "game path");
            return 2;
        }
    }
    return 0;
}

void prime_cli_help(void) {
    puts("PrimeRecomp\n"
         "  --prepare --game=<folder|NSP|XCI> [--keys=<prod.keys>] [--title-keys=<title.keys>]\n"
         "      Set up Metroid Prime Remastered v1.0.0 without a window; exit 0 when ready.\n"
         "      Prints progress <0..1> <message>. Details: primerecomp-setup.log in the data folder.\n"
         "      Without explicit keys, use the package's folder or supported emulator key locations.\n"
         "  --play               Start the prepared game directly.\n"
         "  --preload-shaders    Preload shaders, then play.\n"
         "  --data-root=<path>    Use this data folder for setup, settings and saves.\n"
         "  --help               Show this help.\n");
}
