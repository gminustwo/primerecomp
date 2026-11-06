// In-app updates (issue #36): the start screen asks GitHub for the latest release and offers it (runtime/update.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { UPD_IDLE, UPD_CHECKING, UPD_NONE, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_INSTALLING, UPD_DONE, UPD_FAILED };

typedef struct {
    char tag[32];        // the release's tag ("v0.0.8")
    char name[200];      // its title
    char page[300];      // its page on GitHub
    char asset[400];     // this platform's download ("" = none: the page instead)
    uint64_t size;       // its size, bytes (0: not known)
    char news[8][160];   // the release notes' "What's new" items (their bold titles)
    int nnews;
    bool installs;       // this copy installs it itself (a Windows release folder, an AppImage); else the page
} UpdateInfo;

// At the start: what the last update moved aside (old program files), deleted
void update_cleanup(void);
// Asks for the latest release on a thread of its own (once; force: again, and its failure is reported)
void update_check_start(bool force);
// What's going on (UPD_*); info, progress (0..1, downloading) and a message (a failure's) when given
int update_state(UpdateInfo *info, float *progress, char *msg, size_t msg_size);
// Downloads and installs the update found (UPD_AVAILABLE, info.installs): UPD_DOWNLOADING, UPD_INSTALLING, then
// UPD_DONE (update_restart) or UPD_FAILED (the copy as it was)
void update_install_start(void);
// Starts the new version in this one's place (returns only if it couldn't)
void update_restart(void);

#ifdef __cplusplus
}
#endif
