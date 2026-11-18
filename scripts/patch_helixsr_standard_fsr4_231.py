#!/usr/bin/env python3
"""HelixSR-Scaler 2.3.1: native HLX FSR 4 (Vulkan FFX_vk) separate
from HLX FSR 4 Full (DX12 custom FFX), with accurate live confirmation.
Run after patch_helixsr_hlxfull231.py, before verify.
"""
from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('upstream')
root=Path(p.parse_args().upstream)
inc=root/'OptiScaler/plugins/SDHLiveControl.inc'
vk=root/'OptiScaler/upscalers/ffx/FFXFeature_Vk.cpp'
s=inc.read_text(encoding='utf8')
def once(src,old,new,name):
    count=src.count(old)
    if count!=1:raise SystemExit(f'{name}: expected once, got {count}: {old[:80]}')
    return src.replace(old,new,1)

s=once(s,
'    bool chooseHlxFull=false;\n    auto& state=State::Instance();',
'    bool chooseHlxFull=false;\n    bool chooseHlxVk=false;\n    auto& state=State::Instance();',
'state intent flags')
s=once(s,
'        const bool wantsHlx=(norm=="helixsr" || norm=="hlxffx" || norm=="hlxfsr4");',
'''        // Explicitly distinguish upstream native Vulkan HLX FSR 4 from
        // the distinct 3.4.1 DirectX12 full-network provider.
        const bool wantsHlx=(norm=="helixsr" || norm=="hlxffx" || norm=="hlxfsr4");
        const bool wantsHlxStandard=(norm=="hlxfsr4standard" || norm=="helixsrfsr4");''',
'separate command mapping')
s=once(s,
'        if((wantsHlx && !CustomProviders::IsFfx("HLX FSR 4 Full")) || isVersionedFfx) {',
'''        if((wantsHlx && !CustomProviders::IsFfx("HLX FSR 4 Full")) ||
           (wantsHlxStandard && state.api!=Vulkan) || isVersionedFfx) {''',
'FFX table request')
s=once(s,
'            if(wantsHlx){\n                if(!HelixSR::FsrDllFound() || !SdhChooseHlxFsr4(state,state.api,ffxVersion))',
'''            if(wantsHlx || wantsHlxStandard){
                if((wantsHlx && !HelixSR::FsrDllFound()) ||
                   !SdhChooseHlxFsr4(state,state.api,ffxVersion))''',
'native DX12 version index')
s=once(s,
'''        } else if(wantsHlx) {
            // Official Steam Deck / AMD 3.4.1: custom/HLX FSR 4 Full/ffx.''',
'''        } else if(wantsHlxStandard) {
            // Official HelixSR 3.4.1: custom/HLX FSR 4/ffx_vk.
            // Never silently substitute the Full DX12 provider.
            if(!CustomProviders::IsFfxVk("HLX FSR 4"))
                return "ok=0\\nerror=hlx_fsr4_vk_provider_unavailable\\n";
            chooseHlxVk=true;
            backend=Upscaler::FFX;
        } else if(wantsHlx) {
            // Official Steam Deck / AMD 3.4.1: custom/HLX FSR 4 Full/ffx.''',
'Vulkan custom provider')
s=once(s,
'''    if(hasBackend){
        if(chooseHlxFull){''',
'''    if(hasBackend){
        if(chooseHlxVk){
            cfg->CustomUpscalerVk=std::string("HLX FSR 4");
            cfg->FfxUpscalerIndexFor("HLX FSR 4")=0;
        }else if(_stricmp(cfg->CustomUpscalerVk.value_or("").c_str(),"HLX FSR 4")==0){
            cfg->CustomUpscalerVk.reset();
        }
        if(chooseHlxFull){''',
'separate Vulkan Config')
# Expose both the recognized custom provider and version list; the Renderer
# itself must report an initialized provider, never just a Config name.
s=once(s,
'                if(customFull) {',
'''                const bool customVk=
                    (current.api==Vulkan &&
                     current.currentFeature->GetUpscalerType()==Upscaler::FFX &&
                     _stricmp(current.sdhActiveCustomUpscaler.c_str(),"HLX FSR 4")==0);
                if(customVk) {
                    snapshot+="active_custom_upscaler=HLX FSR 4\\n";
                    snapshot+="active_helix_upscaler=HLX FSR 4\\n";
                } else if(customFull) {''',
'actual Vulkan readback')
v=vk.read_text(encoding='utf8')
v=once(v,
'''    SetInit(true);

    return true;
}

bool FFXFeatureVk::InitInternal''',
'''    // Only after the selected Vulkan FFX context initialized successfully.
    State::Instance().sdhActiveCustomUpscaler =
        _customUpscaler != nullptr && _moduleLoaded
            ? _customUpscaler->name : std::string{};
    SetInit(true);

    return true;
}

bool FFXFeatureVk::InitInternal''',
'Vulkan renderer initialization')
inc.write_text(s,encoding='utf8')
vk.write_text(v,encoding='utf8')
print("PASS: HLX FSR 4 native Vulkan and Full DX12 are separate live providers")
