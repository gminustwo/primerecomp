// The executable's side of NVIDIA DLSS (runtime/nvn/dlss.h). Three builds of the same interface:
// - Windows: loads primerecomp_ngx.dll from the executable's folder if it is there (NVIDIA's NGX library is MSVC-only:
//   the DLL is built with tools/build_ngx_bridge_windows.bat). The executable needs no SDK and builds as before.
// - Linux with NGX_SDK=...: nvn/ngx/dlss_ngx.cpp is compiled in.
// - otherwise: stubs, and the menu doesn't offer DLSS.
#include "../dlss.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)

#include <windows.h>

extern "C" void rt_log(const char *fmt, ...);

namespace {

struct Bridge {
    bool tried;
    HMODULE mod;
    bool (*available)(void);
    const char *(*status)(void);
    uint32_t (*instance_ext)(const char **, uint32_t);
    uint32_t (*device_ext)(void *, void *, const char **, uint32_t);
    void (*init)(void *, void *, void *, void *, void *);
    void (*shutdown)(void *);
    void (*set_log)(void (*)(const char *));
    void (*jitter)(uint32_t, uint32_t, float *, float *);
    bool (*evaluate)(const DlssEval *);
} B;

void log_line(const char *line) { rt_log("%s", line); }  // (the DLL's lines end with a newline)

template <class T> bool sym(T &f, const char *name) {
    f = reinterpret_cast<T>(GetProcAddress(B.mod, name));
    return f != NULL;
}

bool load() {
    if (B.tried) return B.mod != NULL;
    B.tried = true;
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, p, (DWORD)(sizeof p / sizeof *p - 32));
    while (n > 0 && p[n - 1] != L'\\' && p[n - 1] != L'/') n--;
    p[n] = 0;
    wcscat(p, L"primerecomp_ngx.dll");
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return false;
    // (LOAD_WITH_ALTERED_SEARCH_PATH: the DLL's own folder is searched for what it loads)
    B.mod = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!B.mod) { rt_log("[dlss] primerecomp_ngx.dll didn't load (error %lu): is the Visual C++ runtime installed?\n", GetLastError()); return false; }
    bool ok = sym(B.available, "dlss_available") && sym(B.status, "dlss_status") && sym(B.instance_ext, "dlss_instance_extensions") &&
              sym(B.device_ext, "dlss_device_extensions") && sym(B.init, "dlss_init") && sym(B.shutdown, "dlss_shutdown") &&
              sym(B.set_log, "dlss_set_log") && sym(B.jitter, "dlss_jitter") && sym(B.evaluate, "dlss_evaluate");
    if (!ok) {
        rt_log("[dlss] primerecomp_ngx.dll isn't from this version of PrimeRecomp\n");
        FreeLibrary(B.mod);
        B.mod = NULL;
        return false;
    }
    B.set_log(log_line);
    return true;
}

}  // namespace

bool dlss_compiled(void) { return load(); }
bool dlss_available(void) { return B.mod && B.available(); }
const char *dlss_status(void) { return B.mod ? B.status() : "primerecomp_ngx.dll not found next to primerecomp.exe"; }
uint32_t dlss_instance_extensions(const char **names, uint32_t cap) { return load() ? B.instance_ext(names, cap) : 0; }
uint32_t dlss_device_extensions(void *i, void *p, const char **names, uint32_t cap) { return B.mod ? B.device_ext(i, p, names, cap) : 0; }
void dlss_init(void *i, void *p, void *d, void *gi, void *gd) { if (B.mod) B.init(i, p, d, gi, gd); }
void dlss_shutdown(void *d) { if (B.mod) B.shutdown(d); }
void dlss_set_log(void (*)(const char *)) {}
void dlss_jitter(uint32_t in_w, uint32_t out_w, float *x, float *y) {
    if (B.mod) B.jitter(in_w, out_w, x, y);
    else *x = *y = 0;
}
bool dlss_evaluate(const DlssEval *e) { return B.mod && B.evaluate(e); }

#elif defined(HAVE_DLSS)

#include "../ngx/dlss_ngx.cpp"

#else

bool dlss_compiled(void) { return false; }
bool dlss_available(void) { return false; }
const char *dlss_status(void) { return "not built in (make NGX_SDK=...)"; }
uint32_t dlss_instance_extensions(const char **, uint32_t) { return 0; }
uint32_t dlss_device_extensions(void *, void *, const char **, uint32_t) { return 0; }
void dlss_init(void *, void *, void *, void *, void *) {}
void dlss_shutdown(void *) {}
void dlss_set_log(void (*)(const char *)) {}
void dlss_jitter(uint32_t, uint32_t, float *x, float *y) { *x = *y = 0; }
bool dlss_evaluate(const DlssEval *) { return false; }

#endif
