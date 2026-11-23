// Bug reports sent from PrimeRecomp to its GitHub Issues, with the log (runtime/report.c; the Report a bug tab,
// runtime/ui/menu.mm).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    REP_SIGNED_OUT,   // not signed in to GitHub
    REP_CODE,         // asking GitHub for a sign-in code
    REP_AUTHORIZE,    // the code is shown: waiting for the player to enter it at verify_url
    REP_SIGNED_IN,    // ready to send
    REP_SENDING,      // the log, then the issue
    REP_SENT,         // issue_number / issue_url
};

typedef struct {
    int state;               // REP_*
    char user_code[32];      // REP_AUTHORIZE: the code to enter...
    char verify_url[160];    // ... on this page (github.com/login/device)
    int64_t code_expires;    // ... until then (time(NULL) seconds)
    char login[64];          // the GitHub account signed in (shown in the tab, never written to the log)
    char step[160];          // REP_SENDING: what's being done
    int issue_number;        // REP_SENT
    char issue_url[300];
    char error[400];         // the last failure, "" if none (stays until the next attempt)
} ReportState;

// The logs that can be sent: this run's (primerecomp.log, while PrimeRecomp writes it) and the run before's
// (primerecomp.previous.log), those of them that are there
typedef struct {
    char path[1024];
    char name[64];           // "primerecomp.log"
    bool this_run;           // the log this run writes
    int64_t size;            // bytes
    int64_t modified;        // time(NULL) seconds: when it was last written (the run before's: about when it ended)
} ReportLog;
int report_logs(ReportLog out[2]);

typedef struct {
    char title[256];
    char what[1024];         // What happened?
    char where[1024];        // Where, and how to make it happen
    char setup[1024];        // Your setup
    char log_path[1024];     // the log to send (one of report_logs')
    char log_label[96];      // which one, in words ("this run's log")
    bool checked_issues;     // "I looked at the open issues"
} ReportForm;

bool report_available(void);         // sign-in is set up in this build (a GitHub App client id)
void report_state(ReportState *out);
void report_sign_in(void);           // REP_SIGNED_OUT: starts GitHub's device flow
void report_sign_in_cancel(void);
void report_sign_out(void);          // forgets the sign-in (it's only kept in memory anyway)
// REP_SIGNED_IN: sends the log, then the issue that links it (on a thread of its own); false if it can't start
bool report_send(const ReportForm *f);
void report_sent_done(void);         // REP_SENT: back to REP_SIGNED_IN, for another report
const char *report_repo(void);       // "gminustwo/primerecomp"
const char *report_platform(void);   // the Bug report form's Platform answer for this copy
// What PrimeRecomp knows of the setup, for the form's Your setup (the controller, Steam, the display, the frame rate,
// HDR, the upscaler, mods, the GPU)
void report_setup_text(char *buf, size_t n);
// The Bug report form on GitHub with the fields filled in as far as a link can (not the log: it's dragged in there)
void report_form_url(const ReportForm *f, char *buf, size_t n);

#ifdef __cplusplus
}
#endif
