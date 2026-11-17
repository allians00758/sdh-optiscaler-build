#!/usr/bin/env python3
"""Native HelixSR 2.3.1 SDH Live API v2: named HLX/FFX backend switching.
Execute AFTER patch_optiscaler_plugin_api.py and patch_helixsr_live231.py.
The running renderer is the only authority for active readback.
"""
from pathlib import Path
import argparse
p=argparse.ArgumentParser()
p.add_argument("upstream")
root=Path(p.parse_args().upstream)
inc=root/"OptiScaler/plugins/SDHLiveControl.inc"
cpp=root/"OptiScaler/plugins/PluginApi.cpp"
s=inc.read_text(encoding="utf-8")
def change(old,new):
    global s
    count=s.count(old)
    if count!=1:
        raise SystemExit(f"HelixSR upscaler patch drift, count={count}, anchor: {old[:85]}")
    s=s.replace(old,new,1)
change(
'const auto& versions=(api==Vulkan)?state.ffxUpscalerVersionsVk:state.ffxUpscalerVersionsDx12;',
'''// Vulkan HelixSR runs via the DX12 FFX interop path, not the Vulkan table.
    const auto& versions=(api==Vulkan && !HelixSR::FsrDllFound()) ?
        state.ffxUpscalerVersionsVk : state.ffxUpscalerVersionsDx12;'''
)
anchor='static bool SdhQueue(OptiPlugin* plugin,uint64_t seq,const char* command){'
change(anchor,'''// Select the actual HLX FSR 4 entry from the fork's FFX enumeration.
// Do not guess "index 0": the vendor may change the list order.
static bool SdhChooseHlxFsr4(const State& state, API api, int& selected) {
    const auto& versions=(api==Vulkan && !HelixSR::FsrDllFound()) ?
        state.ffxUpscalerVersionsVk : state.ffxUpscalerVersionsDx12;
    selected=-1;
    for(size_t i=0;i<versions.names.size();++i){
        const char* name=versions.names[i];
        if(!name)continue;
        const auto lower=SdhLower(name);
        if(lower.rfind("hlx fsr 4",0)==0){
            selected=static_cast<int>(i);
            return true;
        }
    }
    return false;
}

'''+anchor)
# Dedicated HelixSR provider is not an Upscaler enum case.
# It maps to a concrete FFX version slot or DLSS w/ DX12 interop, as in HelixSR's own menu.
change(
'        if(isVersionedFfx) {',
'''        const bool wantsHlx=(norm=="helixsr" || norm=="hlxffx" || norm=="hlxfsr4");
        const bool wantsHlxDlss=(norm=="helixdlss" || norm=="hlxdlss");
        if(wantsHlx || isVersionedFfx) {'''
)
change(
'''            const auto& ffxVersions=(state.api==Vulkan) ?
                state.ffxUpscalerVersionsVk : state.ffxUpscalerVersionsDx12;
            if(ffxVersions.names.empty()) {
                if(state.api==Vulkan) FFXFeature::EnsureVersionsVulkan();
                else if(state.api==DX11 || state.api==DX12) FFXFeature::EnsureVersionsDx12();
                else return "ok=0\\nerror=ffx_graphics_api_unavailable\\n";
            }
            if(ffxVersions.names.empty())
                return "ok=0\\nerror=ffx_version_query_failed\\n";
            if(!SdhChooseFfxVersion(state,state.api,code,ffxVersion))
                return "ok=0\\nerror=ffx_version_unavailable\\n";
            backend=(state.api==DX11)?Upscaler::FFX_on12:Upscaler::FFX;
            ffxSpecified=true;
        } else {
            backend=CodeToUpscalerFfx(code);
        }''',
'''            const bool usesVulkanFFX=state.api==Vulkan && !HelixSR::FsrDllFound();
            const auto& ffxVersions=usesVulkanFFX ?
                state.ffxUpscalerVersionsVk : state.ffxUpscalerVersionsDx12;
            if(ffxVersions.names.empty()){
                if(usesVulkanFFX)FFXFeature::EnsureVersionsVulkan();
                else if(state.api==DX11 || state.api==DX12 || state.api==Vulkan)
                    FFXFeature::EnsureVersionsDx12();
                else return "ok=0\\nerror=ffx_graphics_api_unavailable\\n";
            }
            if(ffxVersions.names.empty())
                return "ok=0\\nerror=ffx_version_query_failed\\n";
            if(wantsHlx){
                if(!HelixSR::FsrDllFound() || !SdhChooseHlxFsr4(state,state.api,ffxVersion))
                    return "ok=0\\nerror=hlx_fsr4_unavailable\\n";
            }else if(!SdhChooseFfxVersion(state,state.api,code,ffxVersion)){
                return "ok=0\\nerror=ffx_version_unavailable\\n";
            }
            backend=(state.api==DX11 || (state.api==Vulkan && !usesVulkanFFX))?
                Upscaler::FFX_on12:Upscaler::FFX;
            ffxSpecified=true;
        } else if(wantsHlxDlss) {
            if(!HelixSR::Found())return "ok=0\\nerror=hlx_dlss_unavailable\\n";
            backend=state.api==DX12?Upscaler::DLSS:Upscaler::DLSS_on12;
        } else {
            backend=CodeToUpscalerFfx(code);
        }'''
)
# Old source checked DLSS path, which can be absent if HelixSR has its own fallback.
change(
'''        if((backend==Upscaler::DLSS || backend==Upscaler::DLSS_on12) &&
           !state.NVNGX_DLSS_Path.has_value())
            return "ok=0\\nerror=dlss_binary_unavailable\\n";''',
'''        if((backend==Upscaler::DLSS || backend==Upscaler::DLSS_on12) &&
           !state.NVNGX_DLSS_Path.has_value() && !(wantsHlxDlss && HelixSR::Found()))
            return "ok=0\\nerror=dlss_binary_unavailable\\n";'''
)

# On HelixSR, the FFX table is NOT just numeric versions: its HLX entry
# needs its raw label for authoritative per-game INI index persistence.
# Keep the same output length as the generic version list: replace rather
# than duplicate the diagnostic line. No guessed FFX indices.
change(
'                snapshot+="ffx_available_versions=";',
'                snapshot+=(HelixSR::Installed()?"ffx_available_names=":"ffx_available_versions=");'
)
change(
'''                    auto version=SdhFfxVersionNumber(name);
                    if(version.empty() || version.size()>32)continue;
                    if(count++)snapshot+=",";
                    snapshot+=version;''',
'''                    std::string version=HelixSR::Installed()?std::string(name):SdhFfxVersionNumber(name);
                    if(version.empty())continue;
                    if(version.size()>34)version.resize(34);
                    if(count++)snapshot+=",";
                    snapshot+=version;'''
)

# Snapshot: distinguish actual HelixSR FFX target from a generic 'ffx'.
# Readback is skipped while a change is pending, to avoid confirming stale backend.
change(
'''            snapshot+="requested_backend="+UpscalerToCode(state.newBackend)+"\\n";''',
'''            bool anyBackendPending=false;
            for(const auto& item:state.changeBackend)
                if(item.second){anyBackendPending=true;break;}
            if(!anyBackendPending && state.currentFeature && state.currentFeature->IsInited()){
                const auto helixActive=HelixSR::ActiveUpscalerName();
                if(!helixActive.empty())
                    snapshot+="active_helix_upscaler="+helixActive+"\\n";
                const char* activeFfx=HelixSR::ActiveFfxName();
                if(activeFfx && std::strlen(activeFfx)<96)
                    snapshot+="active_helix_ffx_name="+std::string(activeFfx)+"\\n";
            }
            snapshot+="requested_backend="+UpscalerToCode(state.newBackend)+"\\n";'''
)
# Ensure namespace and public API declarations in the original release.
inc.write_text(s,encoding="utf-8")
c=cpp.read_text(encoding="utf-8")
anchor='#include "hooks/Streamline_Hooks.h"'
if c.count(anchor)!=1:raise SystemExit("Missing StreamlineHooks include from FG patch")
c=c.replace(anchor,anchor+'\n#include "helixsr/HelixSR.h"',1)
cpp.write_text(c,encoding="utf-8")
print("HelixSR named HLX FSR4 / HLX DLSS / FFX SDK live routing source PASS")
