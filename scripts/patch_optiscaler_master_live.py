#!/usr/bin/env python3
"""Port SDH Live API v2 to OptiScaler 10 master, fail closed on source drift."""
from pathlib import Path
import argparse

parser=argparse.ArgumentParser()
parser.add_argument("source")
args=parser.parse_args()
root=Path(args.source).resolve()
vk=root/"OptiScaler/menu/menu_overlay_vk.cpp"
dx=root/"OptiScaler/menu/menu_overlay_dx.cpp"
control=root/"OptiScaler/menu/SDHLiveBridge.inc"
inc=Path(__file__).with_name("sdh_plugin_api_extension_master.inc")
if not inc.is_file(): raise SystemExit("Missing staged SDH provider")
for p in (vk, dx, root/"OptiScaler/Config.h",root/"OptiScaler/State.h"):
    if not p.is_file(): raise SystemExit(f"Missing upstream source: {p}")
if control.exists():raise SystemExit("Already patched; refusing to overwrite bridge")
def once(s,needle,replacement,filename):
    n=s.count(needle)
    if n!=1:raise SystemExit(f"{filename}: expected one {needle[:65]!r}, found {n}")
    return s.replace(needle,replacement,1)

v=vk.read_text(encoding="utf-8")
d=dx.read_text(encoding="utf-8")
v=once(v,'#include "menu_common.h"','#include "menu_common.h"\n#include "SDHLiveBridge.inc"',vk)
v=once(v,'bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)\n{',
    'bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)\n{\n    SDHLiveTick(2);',vk)
d=once(d,'#include "menu_common.h"','#include "menu_common.h"\nextern void SDHLiveTick(uint32_t source);',dx)
d=once(d,'void MenuOverlayDx::Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,\n                            const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP)\n{',
    'void MenuOverlayDx::Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,\n                            const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP)\n{\n    SDHLiveTick(1);',dx)

head=r'''// SDH Live API v2 - OptiScaler Nightly 2026-10-21
// Use native render thread for all Config/State mutations.
// ABI: companion ASI exposes OptiPluginInit; 30 stable v1 slots, append-only v2.
#include <State.h>
#include <Config.h>
#include <upscalers/IFeature.h>
#include <upscalers/ffx/FFXFeature.h>
#include <fsr4/FSR4Models.h>
#include <helixsr/HelixSR.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <cctype>
#include <initializer_list>
#include <sstream>
#include <cstdint>

'''
tail=r'''
struct SDHPluginApiV2 {
    uint32_t size;
    uint32_t version;
    void* v1[30];
    int (__cdecl* queue)(void*,uint64_t,const char*);
    uint32_t (__cdecl* poll)(void*,uint64_t,char*,uint32_t);
    uint32_t (__cdecl* snapshot)(void*,char*,uint32_t);
};
static int __cdecl SDHQueueBridge(void* p,uint64_t seq,const char* cmd) {return SdhQueue(p,seq,cmd)?1:0;}
static uint32_t __cdecl SDHPollBridge(void* p,uint64_t seq,char* out,uint32_t n){return SdhPoll(p,seq,out,n);}
static uint32_t __cdecl SDHSnapshotBridge(void* p,char* out,uint32_t n){return SdhSnapshot(p,out,n);}
static SDHPluginApiV2 sdh_api_v2={sizeof(SDHPluginApiV2),2,{},SDHQueueBridge,SDHPollBridge,SDHSnapshotBridge};
static int sdh_plugin_identity=1;
static bool sdh_asi_registered=false;
static uint64_t sdh_next_discovery=0;

void SDHLiveTick(uint32_t source) {
    SdhLiveRenderTick(source);
    if(sdh_asi_registered)return;
    // Do not scan modules every frame when the companion ASI is missing.
    auto now=GetTickCount64();
    if(now < sdh_next_discovery)return;
    sdh_next_discovery=now+1000;
    auto module=GetModuleHandleW(L"sdh-optiscaler-control.asi");
    if(!module)return;
    using Init=BOOL (__cdecl*)(const SDHPluginApiV2*,void*);
    auto init=reinterpret_cast<Init>(GetProcAddress(module,"OptiPluginInit"));
    if(!init)return;
    sdh_asi_registered=(init(&sdh_api_v2,&sdh_plugin_identity)!=FALSE);
}
'''
# Stage reads complete before committing any changes.
if 'has_fsr4_model' not in inc.read_text(encoding="utf-8"):
    raise SystemExit("FSR4 model command missing in provider")
vk.write_text(v,encoding="utf-8")
dx.write_text(d,encoding="utf-8")
control.write_text(head+inc.read_text(encoding="utf-8")+tail,encoding="utf-8")
print("SDH Live API v2 integrated with DXGI/Vulkan render hooks.")
