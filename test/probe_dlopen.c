/* Diagnostic only — built and run by the ubuntu smoke job (not a CMake target).
 *
 * Loads libvlccore (the provider of VLC core symbols) globally, then dlopens a
 * VLC module and reports EXACTLY where the load fails:
 *   - dlopen fails    -> an unresolved symbol (printed)
 *   - dlopen ok but dlsym(entry) NULL -> entry-symbol name mismatch
 *
 *   gcc test/probe_dlopen.c -o probe -ldl && ./probe /path/to/libgridtv_plugin.so
 */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s module.so\n", argv[0]); return 2; }

    void* cc = dlopen("libvlccore.so", RTLD_NOW | RTLD_GLOBAL);
    const char* e1 = cc ? NULL : dlerror();
    printf("libvlccore dlopen: %s\n", e1 ? e1 : "ok");

    /* mirror how VLC loads a module */
    void* p = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!p) { printf("plugin dlopen FAILED: %s\n", dlerror()); return 0; }
    printf("plugin dlopen: ok\n");

    dlerror(); /* clear */
    void* entry = dlsym(p, "vlc_entry__3_0_0f");
    const char* e3 = entry ? NULL : dlerror();
    printf("dlsym(vlc_entry__3_0_0f): %s\n", entry ? "FOUND" : (e3 ? e3 : "NULL (no error)"));
    return 0;
}
