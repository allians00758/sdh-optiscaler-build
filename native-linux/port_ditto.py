#!/usr/bin/env python3
"""Stage Vancete/DittoSR v0.1.2 source as a native Linux Vulkan-only FFX .so.

The original sources are MIT licensed. Copies retain the upstream LICENSE and
THIRD-PARTY-NOTICES. This script does not change an installed Decky plugin.
"""
import argparse
from pathlib import Path
import re

p=argparse.ArgumentParser()
p.add_argument("--source", required=True)
a=p.parse_args()
root=Path(a.source)
src=root/"src"

def replace(path,old,new,count=1):
    s=path.read_text()
    n=s.count(old)
    if n!=count:
        raise RuntimeError(f"{path}: expected {count} occurrences of {old[:65]!r}, got {n}")
    path.write_text(s.replace(old,new))

# The upstream header is shared by D3D12 and Vulkan; keep only the API-neutral
# structures and the exported Vulkan-facing function declarations.
header=src/"dittosr.h"
h=header.read_text().replace("#include <d3d12.h>\n","")
start=h.index("// One upscale: the game's resources")
end=h.index("// The FSR 3.1 formulas for the upscale queries")
h=h[:start]+h[end:]
h=h.replace("with the trailing backslash","with a trailing slash")
header.write_text(h)

api=src/"ffx_api/ffx_api.h"
replace(api,"#define FFX_API_ENTRY __declspec(dllexport)",
"""#if defined(_WIN32)
#define FFX_API_ENTRY __declspec(dllexport)
#else
#define FFX_API_ENTRY __attribute__((visibility("default")))
#endif""")

# Vulkan shader execution is API native already. Replace only DLL access.
vk=src/"dittosr_vk.cpp"
s=vk.read_text().replace('#include <windows.h>', '#include <dlfcn.h>')
old='''    HMODULE vk = GetModuleHandleA("vulkan-1.dll");
    if (!vk) vk = LoadLibraryA("vulkan-1.dll");
    if (vk) {
        fn_->vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)GetProcAddress(vk, "vkGetPhysicalDeviceMemoryProperties");
        fn_->vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)GetProcAddress(vk, "vkGetPhysicalDeviceProperties");
    }
    if (!fn_->vkGetPhysicalDeviceMemoryProperties) ditto_log("vulkan: vulkan-1.dll's vkGetPhysicalDeviceMemoryProperties missing\\n"), all = false;'''
new='''    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (vk) {
        fn_->vkGetPhysicalDeviceMemoryProperties =
            (PFN_vkGetPhysicalDeviceMemoryProperties)dlsym(vk, "vkGetPhysicalDeviceMemoryProperties");
        fn_->vkGetPhysicalDeviceProperties =
            (PFN_vkGetPhysicalDeviceProperties)dlsym(vk, "vkGetPhysicalDeviceProperties");
    }
    if (!fn_->vkGetPhysicalDeviceMemoryProperties || !fn_->vkGetPhysicalDeviceProperties)
        ditto_log("vulkan: native Vulkan loader is missing physical-device queries\\n"), all = false;'''
if old not in s:
    raise RuntimeError("Unexpected upstream Vulkan loader")
vk.write_text(s.replace(old,new))

# Native Linux settings and logging; no WinAPI, no keyboard hooking.
common=src/"common.cpp"
common.write_text(r'''#include "dittosr.h"
#include <dlfcn.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <sys/stat.h>

DittoConfig g_cfg;
char g_dir[260] = {};
static FILE *g_log = nullptr;
static std::string g_ini;
static struct timespec g_last_modified = {};

void ditto_log(const char *fmt, ...) {
    if (!g_log) {
        std::string path = std::string(g_dir) + "dittosr.log";
        g_log = fopen(path.c_str(), "a");
        if (!g_log) return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm lt;
    localtime_r(&ts.tv_sec, &lt);
    fprintf(g_log, "%02d:%02d:%02d.%03ld ", lt.tm_hour, lt.tm_min,
            lt.tm_sec, ts.tv_nsec/1000000);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fflush(g_log);
}
static std::string trim(std::string v) {
    const auto p=v.find_first_not_of(" \t\r\n");
    if(p==std::string::npos)return "";
    const auto q=v.find_last_not_of(" \t\r\n");
    return v.substr(p,q-p+1);
}
static std::map<std::string,std::string> load_ini() {
    std::ifstream f(g_ini);
    std::map<std::string,std::string> props;
    std::string section,line;
    while(std::getline(f,line)) {
        auto comment=line.find_first_of(";#");
        if(comment!=std::string::npos)line.resize(comment);
        line=trim(line);
        if(line.empty())continue;
        if(line.front()=='[' && line.back()==']') {
            section=trim(line.substr(1,line.size()-2));
            continue;
        }
        auto eq=line.find('=');
        if(eq!=std::string::npos)props[section+"."+trim(line.substr(0,eq))]=trim(line.substr(eq+1));
    }
    return props;
}
static void load_config() {
    auto props=load_ini();
    auto f=[&](const char *key,float def) {
        auto it=props.find(std::string("dittosr.")+key);
        if(it==props.end())return def;
        char *end=nullptr;
        const float v=strtof(it->second.c_str(),&end);
        return end==it->second.c_str() ? def : v;
    };
    auto n=[&](const char *section,const char *key,int def) {
        auto it=props.find(std::string(section)+"."+key);
        if(it==props.end())return def;
        char *end=nullptr;
        long v=strtol(it->second.c_str(),&end,0);
        return end==it->second.c_str()?def:static_cast<int>(v);
    };
    g_cfg.enabled=n("dittosr","enabled",1)!=0;
    g_cfg.debug=n("debug","enabled",0)!=0;
    g_cfg.toggleKey=0; // No global key interception in a native Vulkan provider.
    g_cfg.reloadKey=0; // Reload controlled by file timestamp.
    g_cfg.logDispatches=n("dittosr","log_dispatches",g_cfg.debug?3:0);
    DittoSettings &s=g_cfg.s;
    s=DittoSettings();
    #define F(key,field) s.field=f(key,s.field)
    F("frame_weight",frameWeight); F("alpha_rest",alphaRest); F("conf",conf);
    F("gamma_rest",gammaRest); F("width",width); F("react",react); F("edge",edge);
    F("lock_lines",lockLines); F("reactive_scale",reactiveScale);
    F("exposure_scale",exposureScale); F("jitter_sign_x",jitterSignX);
    F("jitter_sign_y",jitterSignY); F("mv_sign",mvSign); F("fov_h_deg",fovHDeg);
    F("motion_gamma",motionGamma); F("motion_share",motionShare);
    F("motion_minmax",motionMinMax); F("sharpness",sharpness);
    F("change_scale",changeScale); F("share_far",shareFar);
    F("share_near",shareNear); F("encode_power",encodePower);
    F("motion_width",motionWidth); F("motion_near",motionNear);
    F("split_clip",splitClip);
    #undef F
    s.depthMode=n("dittosr","depth_mode",s.depthMode);
    s.history10=n("dittosr","history_10bit",s.history10);
    s.fp16=n("dittosr","fp16",s.fp16);
    s.indicator=n("debug","indicator",n("dittosr","indicator",0))!=0;
    s.debugView=n("dittosr","debug_view",n("dittosr","debug_motion",0));
    ditto_log("native Linux config: enabled=%d sharpness=%g frame_weight=%g\n",
              g_cfg.enabled,s.sharpness,s.frameWeight);
}
void ditto_init(void *symbol,const char*) {
    Dl_info info={};
    if(dladdr(symbol,&info) && info.dli_fname) {
        std::string path=info.dli_fname;
        auto slash=path.rfind('/');
        if(slash!=std::string::npos)path.resize(slash+1);
        else path="./";
        snprintf(g_dir,sizeof(g_dir),"%s",path.c_str());
    } else {
        snprintf(g_dir,sizeof(g_dir),"./");
    }
    g_ini=std::string(g_dir)+"dittosr.ini";
    load_config();
    struct stat st={};
    if(stat(g_ini.c_str(),&st)==0)g_last_modified=st.st_mtim;
    ditto_log("DittoSR native Vulkan provider initialized in %s\n",g_dir);
}
bool ditto_poll_reload() {
    struct stat st={};
    if(stat(g_ini.c_str(),&st)!=0)return false;
    if(st.st_mtim.tv_sec==g_last_modified.tv_sec &&
       st.st_mtim.tv_nsec==g_last_modified.tv_nsec)return false;
    g_last_modified=st.st_mtim;
    load_config();
    return true;
}
''')

math=src/"linux_math.cpp"
math.write_text(r'''#include "dittosr.h"
float ditto_ratio_from_quality(uint32_t q) {
    switch(q) {
    case FFX_UPSCALE_QUALITY_MODE_NATIVEAA:return 1.0f;
    case FFX_UPSCALE_QUALITY_MODE_QUALITY:return 1.5f;
    case FFX_UPSCALE_QUALITY_MODE_BALANCED:return 1.7f;
    case FFX_UPSCALE_QUALITY_MODE_PERFORMANCE:return 2.0f;
    case FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE:return 3.0f;
    default:return 0.0f;
    }
}
int32_t ditto_jitter_phase_count(uint32_t renderWidth,uint32_t displayWidth) {
    float r=renderWidth ? float(displayWidth)/float(renderWidth):1.0f;
    return int32_t(8.f*r*r);
}
static float halton(int32_t i,int32_t b) {
    float f=1,r=0;
    for(;i>0;i/=b)f/=float(b),r+=f*float(i%b);
    return r;
}
void ditto_jitter_offset(int32_t index,int32_t phaseCount,float *x,float *y) {
    if(phaseCount<=0)phaseCount=1;
    int32_t i=(index%phaseCount+phaseCount)%phaseCount+1;
    if(x)*x=halton(i,2)-.5f;
    if(y)*y=halton(i,3)-.5f;
}
''')

# Reuse DittoSR's original FFX API front end, but remove Windows DLL forwarding
# and D3D12 execution: the native module explicitly supports Vulkan only.
dll=src/"ffx_dll.cpp"
s=dll.read_text()
s=s.replace('#include <windows.h>','#include <mutex>\n#include <dlfcn.h>\n#include <string>')
s=s.replace('    ID3D12Device *device;','    void *device;')
s=s.replace('static HMODULE g_self;','static std::once_flag g_init_once;')
a=s.index('static bool load_original() {')
b=s.index('// ---- our upscale contexts',a)
s=s[:a]+'''static bool load_original() { return false; }
static void initialize() {
    std::call_once(g_init_once, [] {
        ditto_init((void*)&load_original,"dittosr");
    });
}

'''+s[b:]
s=s.replace('    DittoSR *ditto = nullptr;      // (D3D12)\n','')
s=s.replace('    ID3D12Device *device = nullptr;\n','')
a=s.index('    D3D12_RESOURCE_DESC d =')
b=s.index('\n}',a)
s=s[:a]+'''    ditto_log("  %-12s unexpected non-Vulkan resource\\n", name);'''+s[b:]
s=s.replace('    bool amd = load_original();','    initialize();\n    bool amd = load_original();',1)
s=s.replace('    if ((!be || !be->device) && (!bv || !bv->vkDevice || !bv->vkPhysicalDevice || !bv->vkDeviceProcAddr)) {',
'''    if (!bv || !bv->vkDevice || !bv->vkPhysicalDevice || !bv->vkDeviceProcAddr) {''')
s=s.replace('be ? (void *)be->device : (void *)bv->vkDevice','(void *)bv->vkDevice')
a=s.index('    if (be && be->device) {')
b=s.index('    g_ctx.insert(c);',a)
s=s[:a]+'''    c->dittoVk = new DittoSRVk(bv->vkDevice, bv->vkPhysicalDevice,
                                  bv->vkDeviceProcAddr, c->ci, g_cfg.s);
    if(!c->dittoVk->ok()) {
        ditto_log("DittoSR Vulkan pipeline creation failed\\n");
        delete c->dittoVk;
        delete c;
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
    c->useDitto = true;
'''+s[b:]
s=s.replace('c->ditto ? "DittoSR (D3D12)" : c->dittoVk ? "DittoSR (Vulkan)" : "FSR"',
            'c->dittoVk ? "DittoSR (Vulkan)" : "unavailable"')
s=s.replace('    delete c->ditto;\n','')
s=s.replace('(unsigned long long)(c->ditto ? c->ditto->frames() : c->dittoVk ? c->dittoVk->frames() : 0)',
            '(unsigned long long)(c->dittoVk ? c->dittoVk->frames() : 0)')
s=s.replace('    bool amd = load_original();\n    if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;',
            '    initialize();\n    bool amd = load_original();\n    if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;')
a=s.index('    // the toggle (a key press: the other upscaler')
b=s.index('    if (c->dispatches < (uint64_t)g_cfg.logDispatches)',a)
s=s[:a]+'''    // Native Linux has no global Windows F10 hotkey; use INI mtime reload.
    bool switched = false;
    if (ditto_poll_reload()) c->dittoVk->set_settings(g_cfg.s);
'''+s[b:]
a=s.index('    if (c->useDitto && (c->ditto || c->dittoVk))')
b=s.index('    if (!c->real) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;',a)
s=s[:a]+'''    if (c->useDitto && c->dittoVk) {
        DittoFrameVk fr;
        const bool ran = ditto_frame_from_ffx_vk(d,c->ci,&fr) &&
                         c->dittoVk->dispatch(fr,switched);
        uint64_t n=c->dittoVk->frames();
        if(ran) {
            if(n==1 || n%3600==0)
                ditto_log("DittoSR native Vulkan ran (frame %llu)\\n",(unsigned long long)n);
            return FFX_API_RETURN_OK;
        }
        static int fails=0;
        if(fails++ < 5)ditto_log("DittoSR Vulkan dispatch failed\\n");
    }
'''+s[b:]
a=s.index('BOOL WINAPI DllMain(')
s=s[:a] # initialize lazily from ffxQuery/CreateContext
# Ensure unsupported Windows-only variables have not leaked into the native .so
for name in ("ID3D12Device","ID3D12Resource","DittoSR *","DittoFrame fr","GetAsyncKeyState","HMODULE","DllMain"):
    if name in s:
        raise RuntimeError(f"Remaining Windows dependency in native front end: {name}")
dll.write_text(s)
print("Native DittoSR Vulkan source staged")
