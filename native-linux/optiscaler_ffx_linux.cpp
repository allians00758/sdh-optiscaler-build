// SDH Native Linux OptiScaler FFX bridge: stage 1, Vulkan-compatible FFX providers.
// This deliberately does NOT claim to implement OptiScaler's Windows DLSS/XeSS hooks.
#include "ffx_api/ffx_api.h"
#include <dlfcn.h>
#include <limits.h>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <cstdio>

namespace {
std::mutex lock;
std::string requested_path;
std::string provider_path;
void *provider_module = nullptr;
PfnFfxCreateContext create_fn = nullptr;
PfnFfxDestroyContext destroy_fn = nullptr;
PfnFfxConfigure configure_fn = nullptr;
PfnFfxQuery query_fn = nullptr;
PfnFfxDispatch dispatch_fn = nullptr;
uint32_t active_contexts = 0;

std::string own_directory() {
    Dl_info i={};
    if(!dladdr((void*)&own_directory,&i) || !i.dli_fname)return "./";
    std::string file=i.dli_fname;
    auto slash=file.find_last_of('/');
    return slash==std::string::npos ? "./" : file.substr(0,slash+1);
}
std::string default_provider() {
    const char *env=getenv("SDH_OPTISCALER_FFX_PROVIDER");
    return env && *env ? env : own_directory()+"libdittosr_vk.so";
}
std::string canonical(const std::string &path) {
    char resolved[PATH_MAX]={};
    return realpath(path.c_str(),resolved) ? resolved : path;
}
void close_provider() {
    if(provider_module)dlclose(provider_module);
    provider_module=nullptr;
    provider_path.clear();
    create_fn=nullptr;destroy_fn=nullptr;configure_fn=nullptr;
    query_fn=nullptr;dispatch_fn=nullptr;
}
bool ensure_provider() {
    if(provider_module)return true;
    const std::string path=requested_path.empty()?default_provider():requested_path;
    if(canonical(path)==canonical(own_directory()+"libsdh_optiscaler_ffx.so")) {
        fprintf(stderr,"SDH native FFX: refusing to load itself\n");
        return false;
    }
    void *m=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
    if(!m) {
        fprintf(stderr,"SDH native FFX: cannot load %s: %s\n",path.c_str(),dlerror());
        return false;
    }
    auto a=(PfnFfxCreateContext)dlsym(m,"ffxCreateContext");
    auto b=(PfnFfxDestroyContext)dlsym(m,"ffxDestroyContext");
    auto c=(PfnFfxConfigure)dlsym(m,"ffxConfigure");
    auto d=(PfnFfxQuery)dlsym(m,"ffxQuery");
    auto e=(PfnFfxDispatch)dlsym(m,"ffxDispatch");
    if(!a||!b||!c||!d||!e) {
        fprintf(stderr,"SDH native FFX: incomplete FidelityFX provider %s\n",path.c_str());
        dlclose(m);
        return false;
    }
    create_fn=a;destroy_fn=b;configure_fn=c;query_fn=d;dispatch_fn=e;
    provider_module=m;
    provider_path=path;
    return true;
}
}

extern "C" {
FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext *context, ffxCreateContextDescHeader *desc,
                                               const ffxAllocationCallbacks *alloc) {
    std::lock_guard<std::mutex> guard(lock);
    if(!ensure_provider())return FFX_API_RETURN_NO_PROVIDER;
    auto result=create_fn(context,desc,alloc);
    if(result==FFX_API_RETURN_OK && context && *context)active_contexts++;
    return result;
}
FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext *context,const ffxAllocationCallbacks *alloc) {
    std::lock_guard<std::mutex> guard(lock);
    if(!ensure_provider())return FFX_API_RETURN_NO_PROVIDER;
    auto result=destroy_fn(context,alloc);
    if(result==FFX_API_RETURN_OK && active_contexts)active_contexts--;
    return result;
}
FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext *context,const ffxConfigureDescHeader *desc) {
    std::lock_guard<std::mutex> guard(lock);
    return ensure_provider()?configure_fn(context,desc):FFX_API_RETURN_NO_PROVIDER;
}
FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext *context,ffxQueryDescHeader *desc) {
    std::lock_guard<std::mutex> guard(lock);
    return ensure_provider()?query_fn(context,desc):FFX_API_RETURN_NO_PROVIDER;
}
FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext *context,const ffxDispatchDescHeader *desc) {
    // No live backend replacement while a context exists. The downstream
    // provider keeps its own per-context state and rendering synchronization.
    std::lock_guard<std::mutex> guard(lock);
    return ensure_provider()?dispatch_fn(context,desc):FFX_API_RETURN_NO_PROVIDER;
}

// Stable native control ABI, for a future SDH FrameGen Linux component.
// Only changes the provider when no upscaler context exists.
__attribute__((visibility("default")))
int SDH_OptiScalerLinux_SetProvider(const char *path) {
    if(!path || !*path)return 0;
    std::lock_guard<std::mutex> guard(lock);
    if(active_contexts)return 0;
    close_provider();
    requested_path=path;
    return ensure_provider()?1:0;
}
__attribute__((visibility("default")))
uint32_t SDH_OptiScalerLinux_ActiveContexts() {
    std::lock_guard<std::mutex> guard(lock);
    return active_contexts;
}
__attribute__((visibility("default")))
int SDH_OptiScalerLinux_GetProvider(char *out,uint32_t capacity) {
    std::lock_guard<std::mutex> guard(lock);
    if(!out||capacity<2||!ensure_provider())return 0;
    if(provider_path.size()+1>capacity)return 0;
    std::memcpy(out,provider_path.c_str(),provider_path.size()+1);
    return 1;
}
}
