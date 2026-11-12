// Standalone parser tests: no SDL, graphics driver or game content required.
#include "launcher_cli.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 2);  // fixture directory, including spaces and non-ASCII characters
    PrimeLaunchOptions o;
    char *normal[] = {"prime", "--play"};
    assert(prime_cli_parse(2, normal, &o) == 0 && o.play && !o.prepare);
    char *both[] = {"prime", "--play", "--preload-shaders"};
    assert(prime_cli_parse(3, both, &o) == 0 && o.play && o.preload);
    char *split[] = {"prime", "--prepare", "--game", argv[1]};
    assert(prime_cli_parse(4, split, &o) == 0 && o.prepare && o.game[0]);
    char game[8192];
    snprintf(game, sizeof game, "--game=%s", argv[1]);
    char *joined[] = {"prime", "--prepare", game};
    assert(prime_cli_parse(3, joined, &o) == 0 && o.prepare);
    char *help[] = {"prime", "--help"};
    assert(prime_cli_parse(2, help, &o) == 0 && o.help);
    char *missing[] = {"prime", "--prepare", "--game"};
    assert(prime_cli_parse(3, missing, &o) == 2);
    char *empty[] = {"prime", "--prepare", "--game="};
    assert(prime_cli_parse(3, empty, &o) == 2);
    char *duplicate[] = {"prime", "--prepare", game, game};
    assert(prime_cli_parse(4, duplicate, &o) == 2);
    char *conflict[] = {"prime", "--prepare", game, "--play"};
    assert(prime_cli_parse(4, conflict, &o) == 2);
    char *keys_only[] = {"prime", "--keys=missing.keys"};
    assert(prime_cli_parse(2, keys_only, &o) == 2);
    char *folder_keys[] = {"prime", "--prepare", game, "--keys", argv[1]};
    assert(prime_cli_parse(5, folder_keys, &o) == 2);
    char *unknown[] = {"prime", "--unknown"};
    assert(prime_cli_parse(2, unknown, &o) == 2);
    puts("CLI parser tests passed.");
    return 0;
}
