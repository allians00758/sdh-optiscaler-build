#!/usr/bin/env python3
"""SDH HelixSR-Scaler 2.3.1 native Live API adaptation, after base v2 patch.
Pinned source: https://github.com/lonewolf0622/HelixSR-Scaler tag v2.3.1.
No old proprietary/precompiled DLL is modified or bundled.
"""
from pathlib import Path
import argparse

arg=argparse.ArgumentParser()
arg.add_argument("upstream")
src=Path(arg.parse_args().upstream)
inc=src/"OptiScaler/plugins/SDHLiveControl.inc"
if not inc.is_file():
    raise SystemExit("Run patch_optiscaler_plugin_api.py first")
s=inc.read_text(encoding="utf-8")

def once(old,new):
    global s
    n=s.count(old)
    if n!=1:
        raise SystemExit("HelixSR source marker mismatch; matches="+str(n)+": "+old[:90])
    s=s.replace(old,new,1)

# HelixSR fork and OptiScaler plugin API share the v1 ABI.
# The extension is render-thread queued, not a worker-thread direct Config poke.
# The fork uses different reload flags; changing the INI parameter does not require
# fake-NVAPI reloading.
s=s.replace("state.fakenvapiReloadLowLatency=true;", "/* no legacy fake-NVAPI reload flag in HelixSR */")

once(
 '"has_resource_barrier_fix","has_color_space","has_fsr4_preset"};',
 '"has_resource_barrier_fix","has_color_space","has_fsr4_preset","has_helix_multiplier"};'
)

# Command validation before any side effect. Off means disable FG; 2..6
# are total frame-rate multipliers, hence 1..5 interpolated frames.
once(
 "    // All validation precedes mutation: a failed command changes nothing.",
 """    int helixMultiplier=-1;
    if(SdhFlag(vals,"has_helix_multiplier")){
        const auto value=SdhGet(vals,"helix_multiplier");
        if(value=="0")helixMultiplier=0;
        else if(value=="2"||value=="3"||value=="4"||value=="5"||value=="6")
            helixMultiplier=std::stoi(value);
        else return "ok=0\\nerror=invalid_helix_multiplier\\n";
        if(state.activeFgNvngx!=FGNvngxReplacement::HelixSR)
            return "ok=0\\nerror=helixsr_fg_not_active\\n";
        if(state.activeFgInput!=FGInput::NvngxFG &&
           state.activeFgInput!=FGInput::DLSSG)
            return "ok=0\\nerror=helixsr_fg_input_not_active\\n";
    }
    // All validation precedes mutation: a failed command changes nothing."""
)

once(
 "    if(SdhFlag(vals,\"has_latencyflex_mode\")){\n        cfg->FN_LatencyFlexMode=latencyMode;",
 """    if(helixMultiplier>=0){
        cfg->FGDLSSGOverrideInterpolationCount.set_volatile_value(
            helixMultiplier==0?0:helixMultiplier-1);
        cfg->FGEnabled.set_volatile_value(helixMultiplier!=0);
        state.fgChanged=true;
    }
    if(SdhFlag(vals,"has_latencyflex_mode")){
        cfg->FN_LatencyFlexMode=latencyMode;"""
)

# Advertise native support only when an actual HelixSR Nvngx FG source is active,
# not merely because the launcher selected HelixSR in an INI profile.
once(
 '            if(hasFfx2)snapshot+=",ffx2";',
 '''            if(hasFfx2)snapshot+=",ffx2";
            if(current.activeFgNvngx==FGNvngxReplacement::HelixSR &&
               (current.activeFgInput==FGInput::NvngxFG ||
                current.activeFgInput==FGInput::DLSSG))
                snapshot+=",helix_multiplier";'''
)

once(
 '            snapshot+="ffx_versions_count="+std::to_string(ffx.names.size())+"\\n";',
 '''            snapshot+="helix_multiplier_requested="+
                std::to_string(cfg->FGDLSSGOverrideInterpolationCount.has_value()
                    ? cfg->FGDLSSGOverrideInterpolationCount.value()+1:0)+"\\n";
            snapshot+="ffx_versions_count="+std::to_string(ffx.names.size())+"\\n";'''
)

# Include FG command as an explicitly identifiable debug control.
once(
 '                      key.find("fsr4_preset")!=std::string::npos;',
 '                      key.find("fsr4_preset")!=std::string::npos ||\n                      key.find("helix_multiplier")!=std::string::npos;'
)
inc.write_text(s,encoding="utf-8")
print("HelixSR 2.3.1 SDH Live API v2 adaptation PASS")
