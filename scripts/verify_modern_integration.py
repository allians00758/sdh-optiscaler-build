#!/usr/bin/env python3
"""Fail-closed static integration checks before Windows MSBuild. No game simulation."""
import sys
from pathlib import Path
root=Path(sys.argv[1])
checks={
    "plugin_sdk/optiscaler_plugin.h": ["OPTI_PLUGIN_API_VERSION 2","QueueSdhControl","PollSdhControl","SnapshotSdhControl"],
    "OptiScaler/plugins/PluginApi.cpp": ["#include \"SDHLiveControl.inc\"","SdhLiveRenderTick","Api_RegisterTab","InitPlugin","QueueSdhControl = SdhQueue"],
    "OptiScaler/plugins/SDHLiveControl.inc": [
        "CustomProviders::Scan()","custom_upscaler_unavailable","customRequested",
        "cfg->CustomUpscaler=customName","cfg->CustomUpscalerVk=customName",
        "cfg->CustomUpscaler.reset()","cfg->CustomUpscalerVk.reset()",
        "FfxUpscalerIndexFor(customName)","requested_custom_upscaler=",
        "ffx_available_versions=","active_ffx_version=", "active_custom_upscaler=",
        "SdhChooseFfxVersion(state,state.api,code,ffxVersion)",
        "apply_stage=renderer_reinit_requested"],
    "OptiScaler/upscalers/IFeature.h": ["virtual std::string GetCustomUpscalerName() const"],
    "OptiScaler/upscalers/ffx/FFXFeature_Dx12.h": ["GetCustomUpscalerName() const override"],
    "OptiScaler/upscalers/ffx/FFXFeature_Vk.h": ["GetCustomUpscalerName() const override"],
    "OptiScaler/upscalers/ffx/FFXFeature_Dx12.cpp": ["FFXFeatureDx12::GetCustomUpscalerName() const"],
    "OptiScaler/upscalers/ffx/FFXFeature_Vk.cpp": ["FFXFeatureVk::GetCustomUpscalerName() const"],
    "OptiScaler/Config.cpp": ["sdh_plugin_ini_mutex","Config::GetRawValue","Config::SaveRawValues"],
    "OptiScaler/Config.h": ["FfxUpscalerIndexFor(", "GetRawValue(","SaveRawValues("],
    "OptiScaler/dllmain.cpp": ["PluginApi::InitPlugin(hMod, ext, entry.path())"],
    "OptiScaler/menu/menu_overlay_dx.cpp": ["PluginApi::SdhLiveTick(1)"],
    "OptiScaler/menu/menu_overlay_vk.cpp": ["PluginApi::SdhLiveTick(2)"],
    "OptiScaler/menu/menu_common.cpp": [
        "SDH: allow preconfiguring LatencyFlex even when inactive.",
        "LatencyFlex mode is saved but inactive",
        'PopulateCombo("LatencyFlex mode", config->FN_LatencyFlexMode, lfx_modes)',
    ],
    "OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp": ["PluginApi::SdhLiveTick(3)"],
    "OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp": ["PluginApi::SdhLiveTick(4)"],
    "OptiScaler/inputs/NVNGX_DLSS_Vk.cpp": ["PluginApi::SdhLiveTick(5)"],
    "OptiScaler/OptiScaler.vcxproj": ['plugins\\PluginApi.cpp'],
}
for path,markers in checks.items():
    p=root/path
    if not p.exists():raise SystemExit(f"Missing {path}")
    s=p.read_text(encoding="utf-8")
    for m in markers:
        if m not in s:raise SystemExit(f"{path}: missing {m}")
p=(root/"OptiScaler/plugins/SDHLiveControl.inc").read_text(encoding="utf-8")
start=p.index("bool explicitCustom=")
assert start<p.index("if(isVersionedFfx)"),"custom requires resolution before FFX enum"
assert "if(customRequested)" in p and "if(ffxSpecified)cfg->FfxUpscalerIndexFor(customName)=ffxVersion;" in p
assert "code=\"fsr3.1.6\"" in p, "3.1.6 must remain supported when advertised by DLL"
assert "code=\"fsr3.1.5\"" in p
assert "active_ffx_version=" in p, "Separate requested vs active version reporting"
assert "custom_upscaler_unavailable" in p
menu=(root/"OptiScaler/menu/menu_common.cpp").read_text(encoding="utf-8")
# This source legitimately uses the active LatencyFlex condition elsewhere,
# e.g. to show the current output name. Only inspect the settings function.
section_start="void MenuCommon::RenderLowLatencySettings(RenderMenuContext& ctx)"
section_end="void MenuCommon::RenderActiveImageSettings(RenderMenuContext& ctx)"
assert menu.count(section_start)==1 and menu.count(section_end)==1
low_latency_menu=menu.split(section_start,1)[1].split(section_end,1)[0]
assert "if (activeOutput == LowLatencyMode::LatencyFlex)" not in low_latency_menu, "LatencyFlex selector is still gated in its settings"
assert "if (activeOutput != LowLatencyMode::LatencyFlex)" in low_latency_menu
assert 'PopulateCombo("LatencyFlex mode", config->FN_LatencyFlexMode, lfx_modes)' in low_latency_menu
assert "LFXMode::Conservative" in low_latency_menu and "LFXMode::Aggressive" in low_latency_menu and "LFXMode::ReflexIDs" in low_latency_menu
d=(root/"OptiScaler/dllmain.cpp").read_text(encoding="utf-8")
assert d.count("PluginApi::InitPlugin(hMod, ext, entry.path())")==2
c=(root/"OptiScaler/Config.cpp").read_text(encoding="utf-8")
assert c.count("std::lock_guard<std::recursive_mutex> guard(sdh_plugin_ini_mutex)")>=7
print("PASS DEV19: modern source port, custom FFX selection and ABI v2 markers")
