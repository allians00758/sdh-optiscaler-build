#!/usr/bin/env python3
"""Support HelixSR 3.4.1's official custom/HLX FSR 4 Full FFX provider.
Run after patch_helixsr_upscalers231.py. Never touch stock OptiScaler.
Native render-thread changes with actual initialized custom readback.
"""
from pathlib import Path
import argparse

a=argparse.ArgumentParser()
a.add_argument("upstream")
root=Path(a.parse_args().upstream)
bridge=root/"OptiScaler/plugins/SDHLiveControl.inc"
state_file=root/"OptiScaler/State.h"
feature_file=root/"OptiScaler/upscalers/ffx/FFXFeature_Dx12.cpp"
api_file=root/"OptiScaler/plugins/PluginApi.cpp"

def once(src,old,new,label):
    n=src.count(old)
    if n!=1:
        raise SystemExit(f"{label}: required upstream anchor matched {n} times: {old[:95]!r}")
    return src.replace(old,new,1)

s=bridge.read_text(encoding="utf-8")
# The official 3.4.1 ZIP contains a custom FFX provider. It is NOT guaranteed
# to be one of the global FFX version names (which previously caused
# hlx_fsr4_unavailable even with the provider installed).
s=once(s,
'    bool ffxSpecified=false;\n    auto& state=State::Instance();',
'    bool ffxSpecified=false;\n    bool chooseHlxFull=false;\n    auto& state=State::Instance();',
"backend staging")
s=once(s,
'        if(wantsHlx || isVersionedFfx) {',
'        if((wantsHlx && !CustomProviders::IsFfx("HLX FSR 4 Full")) || isVersionedFfx) {',
"native FFX fallback")
s=once(s,
'''        } else if(wantsHlxDlss) {
            if(!HelixSR::Found())''',
'''        } else if(wantsHlx) {
            // Official Steam Deck / AMD 3.4.1: custom/HLX FSR 4 Full/ffx.
            // FFX_on12 is used by Vulkan/DX11; DX12 uses native FFX.
            chooseHlxFull=true;
            backend=state.api==DX12 ? Upscaler::FFX : Upscaler::FFX_on12;
        } else if(wantsHlxDlss) {
            if(!HelixSR::Found())''',
"custom full route")
s=once(s,
'''    if(hasBackend){
        if(ffxSpecified)cfg->FfxUpscalerIndex=ffxVersion;''',
'''    if(hasBackend){
        if(chooseHlxFull){
            cfg->CustomUpscaler=std::string("HLX FSR 4 Full");
            cfg->FfxUpscalerIndexFor("HLX FSR 4 Full")=0;
        }else{
            // Explicit user choice of a named backend exits only this
            // particular HLX custom provider; preserve unrelated custom
            // providers until the user actually chooses another method.
            if(_stricmp(cfg->CustomUpscaler.value_or("").c_str(),"HLX FSR 4 Full")==0)
                cfg->CustomUpscaler.reset();
        }
        if(ffxSpecified)cfg->FfxUpscalerIndex=ffxVersion;''',
"mutate renderer Config")
s=once(s,
'''                const auto helixActive=HelixSR::ActiveUpscalerName();
                if(!helixActive.empty())
                    snapshot+="active_helix_upscaler="+helixActive+"\\n";
                const char* activeFfx=HelixSR::ActiveFfxName();
                if(activeFfx && std::strlen(activeFfx)<96)
                    snapshot+="active_helix_ffx_name="+std::string(activeFfx)+"\\n";''',
'''                const bool customFull=
                    (current.currentFeature->GetUpscalerType()==Upscaler::FFX ||
                     current.currentFeature->GetUpscalerType()==Upscaler::FFX_on12) &&
                    _stricmp(current.sdhActiveCustomUpscaler.c_str(),"HLX FSR 4 Full")==0;
                if(customFull) {
                    snapshot+="active_custom_upscaler=HLX FSR 4 Full\\n";
                    snapshot+="active_helix_upscaler=HLX FSR 4 Full\\n";
                } else {
                    const auto helixActive=HelixSR::ActiveUpscalerName();
                    if(!helixActive.empty())
                        snapshot+="active_helix_upscaler="+helixActive+"\\n";
                    const char* activeFfx=HelixSR::ActiveFfxName();
                    if(activeFfx && std::strlen(activeFfx)<96)
                        snapshot+="active_helix_ffx_name="+std::string(activeFfx)+"\\n";
                }''',
"actual initialized custom readback")

st=state_file.read_text(encoding="utf-8")
st=once(st,
'    Upscaler newBackend = Upscaler::Reset;',
'''    Upscaler newBackend = Upscaler::Reset;
    // Set only after successful initialization by the custom FFX renderer.
    std::string sdhActiveCustomUpscaler;''',
"State")
ft=feature_file.read_text(encoding="utf-8")
ft=once(ft,
'''    SetInit(true);

    return true;
}''',
'''    // Unambiguous actual-provider readback. Do not infer custom success
    // from Config::CustomUpscaler, which changes before FFX is ready.
    State::Instance().sdhActiveCustomUpscaler =
        _customUpscaler != nullptr && _moduleLoaded
            ? _customUpscaler->name : std::string{};
    SetInit(true);

    return true;
}''',
"FFXFeatureDx12 successful init")
api=api_file.read_text(encoding="utf-8")
api=once(api,
'#include "helixsr/HelixSR.h"',
'#include "helixsr/HelixSR.h"\n#include "misc/CustomProviders.h"',
"PluginApi includes")

# Commit only after all source anchors were validated.
bridge.write_text(s,encoding="utf-8")
state_file.write_text(st,encoding="utf-8")
feature_file.write_text(ft,encoding="utf-8")
api_file.write_text(api,encoding="utf-8")
print("PASS: HelixSR 3.4.1 native custom HLX FSR 4 Full selection + active readback")
