//
// PanVK loader + unified panvk.log
//

#include <android/api-level.h>
#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <jni.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#include <driver_helper/nsbypass.h>
#include <android/dlext.h>
#include <mojoexec.h>
#include <android/log.h>

#define TAG "MojoExec-Vulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static bool turnip_enabled = false;
static bool panvk_enabled = false;

static FILE* panvk_log_fp = NULL;

static void panvk_log_open(void) {
    if (panvk_log_fp) return;
    const char* candidates[] = {
        getenv("MOD_ANDROID_RUNTIME"),
        getenv("TMPDIR"),
        "/data/local/tmp",
        NULL
    };
    char path[512];
    for (int i = 0; candidates[i]; i++) {
        if (!candidates[i] || !candidates[i][0]) continue;
        snprintf(path, sizeof(path), "%s/panvk.log", candidates[i]);
        panvk_log_fp = fopen(path, "a");
        if (panvk_log_fp) {
            LOGI("panvk.log opened at %s", path);
            time_t now = time(NULL);
            fprintf(panvk_log_fp, "\n===== PanVK session start %s", ctime(&now));
            fflush(panvk_log_fp);
            return;
        }
    }
    LOGE("Could not open panvk.log in any candidate path");
}

static void panvk_log(const char* fmt, ...) {
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    __android_log_print(ANDROID_LOG_INFO, "PANVK", "%s", buf);

    panvk_log_open();
    if (panvk_log_fp) {
        fprintf(panvk_log_fp, "%s\n", buf);
        fflush(panvk_log_fp);
    }
}

#ifdef ENABLE_TURNIP_LOADER
bool load_turnip_vulkan() {
    static bool driver_loaded = false;
    if(driver_loaded) return true;

    const char* cache_dir = getenv("TMPDIR");
    if(!linker_ns_load(mojoexec_native_dir)) return false;
    void* linkerhook = linker_ns_dlopen("liblinkerhook.so", RTLD_LOCAL | RTLD_NOW);
    if(linkerhook == NULL) return false;
    void* turnip_driver_handle = linker_ns_dlopen("libvulkan_freedreno.so", RTLD_LOCAL | RTLD_NOW);
    if(turnip_driver_handle == NULL) {
        printf("MojoExec: Failed to load Turnip!\n%s\n", dlerror());
        goto fail_l;
    }

    void* dl_android = linker_ns_dlopen("libdl_android.so", RTLD_LOCAL | RTLD_LAZY);
    if(dl_android == NULL) goto fail_t;

    void* android_get_exported_namespace = dlsym(dl_android, "android_get_exported_namespace");
    void (*linkerhook_pass_handles)(void*, void*, void*) = dlsym(linkerhook, "app__pojav_linkerhook_pass_handles");

    if(linkerhook_pass_handles == NULL || android_get_exported_namespace == NULL) goto fail_d;
    linkerhook_pass_handles(turnip_driver_handle, android_dlopen_ext, android_get_exported_namespace);

    void* libvulkan = linker_ns_dlopen_unique(cache_dir, "libvulkan.so", "libmjlvlk.so", RTLD_LOCAL | RTLD_NOW);
    printf("MojoExec: Loaded mjlvlk, ptr=%p\n", libvulkan);
    if(libvulkan) {
        driver_loaded = true;
        return true;
    }
    fail_d: dlclose(dl_android);
    fail_t: dlclose(turnip_driver_handle);
    fail_l: dlclose(linkerhook);
    return false;
}
#endif

#ifdef ENABLE_PANVK_LOADER
bool load_panvk_vulkan() {
    static bool driver_loaded = false;
    if (driver_loaded) return true;

    panvk_log("Attempting to load PanVK (libvulkan_panfrost.so)");

    const char* mod_dir = getenv("MOD_ANDROID_RUNTIME");
    const char* native_dir = mojoexec_native_dir;

    const char* names[] = {
        "libvulkan_panfrost.so",
        "libvulkan_panfrost-android-aarch64.so",
        "libvulkan_panfrost-launcher.so",
        NULL
    };

    if (mod_dir && mod_dir[0]) {
        for (int i = 0; names[i]; i++) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", mod_dir, names[i]);
            void* h = dlopen(full, RTLD_LOCAL | RTLD_NOW);
            if (h) {
                panvk_log("Loaded PanVK via dlopen(%s) = %p", full, h);
                driver_loaded = true;
                return true;
            }
            panvk_log("dlopen(%s) failed: %s", full, dlerror() ? dlerror() : "unknown");
        }
    }

    if (!linker_ns_load(native_dir ? native_dir : ".")) {
        panvk_log("linker_ns_load failed for %s", native_dir ? native_dir : ".");
        return false;
    }

    for (int i = 0; names[i]; i++) {
        void* h = linker_ns_dlopen(names[i], RTLD_LOCAL | RTLD_NOW);
        if (h) {
            panvk_log("Loaded PanVK via linker_ns_dlopen(%s) = %p", names[i], h);
            driver_loaded = true;
            return true;
        }
        panvk_log("linker_ns_dlopen(%s) failed: %s", names[i], dlerror() ? dlerror() : "unknown");
    }

    panvk_log("All PanVK load attempts failed");
    return false;
}
#endif

void* mojoexec_acq_vulkan_handle() {
    int flags = RTLD_LOCAL | RTLD_NOW;

#ifdef ENABLE_PANVK_LOADER
    if (panvk_enabled && android_get_device_api_level() >= 28) {
        if (load_panvk_vulkan()) {
            void* h = dlopen("libvulkan_panfrost.so", flags);
            if (!h) h = linker_ns_dlopen("libvulkan_panfrost.so", flags);
            if (h) {
                panvk_log("Returning PanVK handle %p", h);
                return h;
            }
        }
    }
#endif

#ifdef ENABLE_TURNIP_LOADER
    if(android_get_device_api_level() >= 28) {
        if(turnip_enabled && load_turnip_vulkan())
            return linker_ns_dlopen("libmjlvlk.so", flags);
    }
#endif
    void* vulkan_ptr = dlopen("libvulkan.so", flags);
    printf("MojoExec: loaded system vulkan, ptr=%p\n", vulkan_ptr);
    panvk_log("Falling back to system libvulkan.so = %p", vulkan_ptr);
    return vulkan_ptr;
}

JNIEXPORT void JNICALL
Java_git_artdeell_mojoexec_MojoExec_setUseTurnip(JNIEnv *env, jclass clazz, jboolean enable) {
    turnip_enabled = enable;
}

JNIEXPORT void JNICALL
Java_git_artdeell_mojoexec_MojoExec_setUsePanvk(JNIEnv *env, jclass clazz, jboolean enable) {
    panvk_enabled = enable;
    panvk_log("setUsePanvk(%d)", (int)enable);
}

JNIEXPORT void JNICALL
Java_git_artdeell_mojoexec_MojoExec_preloadVulkan(JNIEnv *env, jclass clazz) {
#ifdef ENABLE_PANVK_LOADER
    if (panvk_enabled) {
        if (!load_panvk_vulkan()) {
            panvk_log("Failed to preload PanVK");
        } else {
            panvk_log("PanVK preloaded successfully");
        }
        return;
    }
#endif
#ifdef ENABLE_TURNIP_LOADER
    if(!turnip_enabled) return;
    if(!load_turnip_vulkan()) {
        printf("MojoExec: Failed to preload Turnip!\n");
    }
#endif
}
