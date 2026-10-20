#include <initializer_list>
#include <string>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <mutex>
#include <android_linker_ns.h>
#include <android/dlext.h>
#include <android/log.h>
#include "kgsl.h"
#include "hook_impl_params.h"
#include "hook_impl.h"

#define TAG "hook_impl"
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, TAG, fmt, ##__VA_ARGS__)

const HookImplParams *hook_params; //!< Bunch of info needed to load/patch the driver
int (*gsl_memory_alloc_pure_sym)(uint32_t, uint32_t, void *);
int (*gsl_memory_alloc_pure_64_sym)(uint64_t, uint32_t, void *);
int (*gsl_memory_free_pure_sym)(void *);

// Inicializamos en -1 para evitar conflictos con el descriptor 0 (stdin)
int kgsl_fd = -1;
std::mutex kgsl_mutex; // Mutex para proteger el acceso multihilo bajo Mesa 26

using gsl_memory_alloc_pure_t = decltype(gsl_memory_alloc_pure_sym);
using gsl_memory_alloc_pure_64_t = decltype(gsl_memory_alloc_pure_64_sym);
using gsl_memory_free_pure_t = decltype(gsl_memory_free_pure_sym);

__attribute__((visibility("default"))) void init_hook_param(const void *param) {
    hook_params = reinterpret_cast<const HookImplParams *>(param);
}

__attribute__((visibility("default"))) void init_gsl(void *alloc, void *alloc64, void *free) {
    gsl_memory_alloc_pure_sym = reinterpret_cast<gsl_memory_alloc_pure_t>(alloc);
    gsl_memory_alloc_pure_64_sym = reinterpret_cast<gsl_memory_alloc_pure_64_t>(alloc64);
    gsl_memory_free_pure_sym = reinterpret_cast<gsl_memory_free_pure_t>(free);
}

__attribute__((visibility("default"))) void *hook_android_dlopen_ext(const char *filename, int flags, const android_dlextinfo *extinfo) {
    auto fallback{[&]() {
        LOGI("hook_android_dlopen_ext: falling back!");
        return android_dlopen_ext(filename, flags, extinfo);
    }};

    LOGI("hook_android_dlopen_ext: filename: %s", filename);

    // Ignore non-vulkan libraries
    if (!strstr(filename, "vulkan.") && !strstr(filename, "panfrost") && !strstr(filename, "pankaku"))
        return android_dlopen_ext(filename, flags, extinfo);

    if (extinfo->library_namespace == nullptr || !(extinfo->flags & ANDROID_DLEXT_USE_NAMESPACE)) {
        LOGI("hook_android_dlopen_ext: hook failed: namespace not supplied!");
        return fallback();
    }

    auto driverNs{android_create_namespace(filename, hook_params->customDriverDir.c_str(),
                                           hook_params->hookLibDir.c_str(), ANDROID_NAMESPACE_TYPE_SHARED,
                                           nullptr, extinfo->library_namespace)};
    if (!driverNs) {
        LOGI("hook_android_dlopen_ext: hook failed: namespace not supplied!");
        return fallback();
    }

    android_link_namespaces(driverNs, nullptr, "libandroid.so");

    auto hookImpl{linkernsbypass_namespace_dlopen("libhook_impl.so", RTLD_NOW, driverNs)};
    if (!hookImpl)
        return nullptr;

    auto initHookParam{reinterpret_cast<void (*)(const void *)>(dlsym(hookImpl, "init_hook_param"))};
    if (!initHookParam)
        return nullptr;

    initHookParam(hook_params);

    if (hook_params->featureFlags & ADRENOTOOLS_DRIVER_FILE_REDIRECT) {
        if (!linkernsbypass_namespace_dlopen("libfile_redirect_hook.so", RTLD_GLOBAL, driverNs)) {
            LOGI("hook_android_dlopen_ext: hook failed: failed to apply libfopen_redirect_hook!");
            return fallback();
        }

        LOGI("hook_android_dlopen_ext: applied libfile_redirect_hook");
    }

    auto newExtinfo{*extinfo};
    newExtinfo.library_namespace = driverNs;

    if (hook_params->featureFlags & ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT) {
        if (!linkernsbypass_namespace_dlopen("libgsl_alloc_hook.so", RTLD_GLOBAL, driverNs)) {
            LOGI("hook_android_dlopen_ext: hook failed: failed to apply libgsl_alloc_hook!");
            return fallback();
        }

        auto libgslHandle{android_dlopen_ext("vkbgsl.so", RTLD_NOW, &newExtinfo)};
        if (!libgslHandle) {
            libgslHandle = android_dlopen_ext("notgsl.so", RTLD_NOW, &newExtinfo);
            if (!libgslHandle)
                libgslHandle = android_dlopen_ext("libgsl.so", RTLD_NOW, &newExtinfo);
        }

        if (libgslHandle) {
            gsl_memory_alloc_pure_sym = reinterpret_cast<decltype(gsl_memory_alloc_pure_sym)>(dlsym(libgslHandle, "gsl_memory_alloc_pure"));
            gsl_memory_alloc_pure_64_sym = reinterpret_cast<decltype(gsl_memory_alloc_pure_64_sym)>(dlsym(libgslHandle, "gsl_memory_alloc_pure_64"));
            gsl_memory_free_pure_sym = reinterpret_cast<decltype(gsl_memory_free_pure_sym)>(dlsym(libgslHandle, "gsl_memory_free_pure"));
            if ((gsl_memory_alloc_pure_sym || gsl_memory_alloc_pure_64_sym) && gsl_memory_free_pure_sym) {
                auto initGsl{reinterpret_cast<void (*)(gsl_memory_alloc_pure_t, gsl_memory_alloc_pure_64_t, gsl_memory_free_pure_t)>(dlsym(hookImpl, "init_gsl"))};
                if (!initGsl)
                    return fallback();

                initGsl(gsl_memory_alloc_pure_sym, gsl_memory_alloc_pure_64_sym, gsl_memory_free_pure_sym);
                LOGI("hook_android_dlopen_ext: applied libgsl_alloc_hook");
                hook_params->nextGpuMapping->gpu_addr = ADRENOTOOLS_GPU_MAPPING_SUCCEEDED_MAGIC;
            }
        }

        if (!((gsl_memory_alloc_pure_sym || gsl_memory_alloc_pure_64_sym) && gsl_memory_free_pure_sym))
            LOGI("hook_android_dlopen_ext
