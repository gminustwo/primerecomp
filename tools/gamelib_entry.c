// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library. Everything else in it stays hidden. One row per module of
// GAME_MODULES (rt.h), in its order.
#include "gamelib.h"

extern const uint64_t main_fn_addrs[], sdk_fn_addrs[], main_fn_count, sdk_fn_count;
extern const GuestFn main_fn_ptrs[], sdk_fn_ptrs[];

GAMELIB_EXPORT const GameLibTables *lasgame_tables(void) {
    static const char *const names[] = {"main", "sdk"};
    static const uint64_t *const addrs[] = {main_fn_addrs, sdk_fn_addrs};
    static const GuestFn *const fns[] = {main_fn_ptrs, sdk_fn_ptrs};
    static const uint64_t *const counts[] = {&main_fn_count, &sdk_fn_count};
    static const GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, 2, names, addrs, fns, counts};
    return &t;
}
