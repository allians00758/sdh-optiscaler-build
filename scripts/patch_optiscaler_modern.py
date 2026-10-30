#!/usr/bin/env python3
"""Port SDH Plugin API v2 to the pinned 2026-10-30 OptiScaler master source.
Does not patch binaries. Fails closed when upstream source shape changes.
"""
import argparse
import subprocess
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument("source")
args=p.parse_args()
root=Path(args.source).resolve()
base="origin/plugin-api"
def gitshow(path):
    return subprocess.check_output(["git","-C",str(root),"show",f"{base}:{path}"],text=True,encoding="utf-8")
def file(path):
    f=root/path
    if not f.is_file(): raise RuntimeError(f"Missing modern upstream file: {path}")
    return f.read_text(encoding="utf-8")
def one(s,old,new,label,count=1):
    if s.count(old)!=count: raise RuntimeError(f"{label}: expected {count} occurrence(s), got {s.count(old)} for {old[:65]!r}")
    return s.replace(old,new,count)
def save(path,content):
    dest=root/path
    dest.parent.mkdir(parents=True,exist_ok=True)
    dest.write_text(content,encoding="utf-8")

api_path="plugin_sdk/optiscaler_plugin.h"
cpp_path="OptiScaler/plugins/PluginApi.cpp"
header_path="OptiScaler/plugins/PluginApi.h"
provider_path="OptiScaler/plugins/SDHLiveControl.inc"
api=gitshow(api_path)
api=one(api,"#define OPTI_PLUGIN_API_VERSION 1","#define OPTI_PLUGIN_API_VERSION 2","SDK version")
marker="        void (*Log)(OptiPlugin* plugin, OptiLogLevel level, const char* message);\n"
api=one(api,marker,marker+"""
        // SDH Live API v2, appended after every v1 field for binary compatibility.
        bool (*QueueSdhControl)(OptiPlugin* plugin, uint64_t seq, const char* command);
        uint32_t (*PollSdhControl)(OptiPlugin* plugin, uint64_t seq, char* result, uint32_t size);
        uint32_t (*SnapshotSdhControl)(OptiPlugin* plugin, char* result, uint32_t size);
""","SDK v2 fields")

impl=gitshow(cpp_path)
impl=one(impl,'#include "Config.h"','#include "Config.h"\n#include "State.h"\n#include "upscalers/IFeature.h"\n#include "upscalers/ffx/FFXFeature.h"\n#include "misc/CustomProviders.h"\n#include <algorithm>\n#include <atomic>\n#include <deque>\n#include <map>\n#include <cmath>\n#include <cstring>\n#include <string_view>\n#include <cctype>\n#include <initializer_list>',"API includes")
impl=one(impl,"const OptiPluginApi _api = {",'#include "SDHLiveControl.inc"\n\nconst OptiPluginApi _api = {',"Provider include")
impl=one(impl,'    .Log = Api_Log,','    .Log = Api_Log,\n    .QueueSdhControl = SdhQueue,\n    .PollSdhControl = SdhPoll,\n    .SnapshotSdhControl = SdhSnapshot,',"ABI callback mapping")
impl=one(impl,"} // namespace\n\nvoid PluginApi::InitPlugin","} // namespace\n\nvoid PluginApi::SdhLiveTick(uint32_t source) { SdhLiveRenderTick(source); }\n\nvoid PluginApi::InitPlugin","Render tick mapping")
header=gitshow(header_path)
header=one(header,"  public:\n    struct TabInfo","  public:\n    static void SdhLiveTick(uint32_t source);\n\n    struct TabInfo","Render tick declaration")

cfg_h=file("OptiScaler/Config.h")
cfg_h=one(cfg_h,"    bool SaveIni();\n    bool SaveXeFG();","""    bool SaveIni();
    bool SaveXeFG();
    // Settings owned by plugins, under their own [Plugin.*] sections.
    std::optional<std::string> GetRawValue(const char* section,const char* key);
    void SetRawValue(const char* section,const char* key,const char* value);
    void DeleteRawValue(const char* section,const char* key);
    bool SaveRawValues();""","Config header")
cfg=file("OptiScaler/Config.cpp")
cfg=one(cfg,"static CSimpleIniA ini;","static CSimpleIniA ini;\nstatic std::recursive_mutex sdh_plugin_ini_mutex;","INI mutex")
cfg=one(cfg,"bool Config::Reload(std::filesystem::path iniPath)\n{","bool Config::Reload(std::filesystem::path iniPath)\n{\n    std::lock_guard<std::recursive_mutex> guard(sdh_plugin_ini_mutex);","Reload lock")
cfg=one(cfg,"bool Config::SaveIni()\n{","bool Config::SaveIni()\n{\n    std::lock_guard<std::recursive_mutex> guard(sdh_plugin_ini_mutex);","Save lock")
cfg=one(cfg,"bool Config::SaveXeFG()\n{","bool Config::SaveXeFG()\n{\n    std::lock_guard<std::recursive_mutex> guard(sdh_plugin_ini_mutex);","XeFG save lock")
config_raw="""
std::optional<std::string> Config::GetRawValue(const char* section,const char* key)
{
    if(!section || !key)return std::nullopt;
    std::lock_guard<std::recursive_mutex> guard(sdh_plugin_ini_mutex);
    const char* v=ini.GetValue(section,key,nullptr);
    return v ? std::optional<std::string>(v) : std::nullopt;
}
void Config::SetRawValue(const char* section,const char* key,const char* value)
{
    if(!section || !key || !value)return;
    std::lock_guard<std::recursive_mutex> guard(sdh_plugin_ini_mutex);
    ini.SetValue(section,key,value);
}
void Config::DeleteRawValue(const char* section,const char* key)
{
    if(!section || !key)return;
    std::lock_guard<std::recursive_mutex> guard(sdh_plugin_ini_mutex);
    ini.Delete(section,key,true);
}
bool Config::SaveRawValues()
{
    std::lock_guard<std::recursive_mutex> guard(sdh_plugin_ini_mutex);
    return ini.SaveFile(absoluteFileName.wstring().c_str())>=0;
}
"""
cfg=one(cfg,"void Config::CheckUpscalerFiles()",config_raw+"\nvoid Config::CheckUpscalerFiles()","Config raw implementations")

dll=file("OptiScaler/dllmain.cpp")
dll=one(dll,'#include "Config.h"','#include "Config.h"\n#include "plugins/PluginApi.h"',"ASI include")
# After InitializeASI, initialize SDH's companion ASI in both normal and late paths.
import re
pattern=r'(?m)^([ \t]*)if \(init != nullptr\)\n[ \t]*init\(\);'
matches=list(re.finditer(pattern,dll))
if len(matches)!=2: raise RuntimeError(f"Expected 2 ASI initialization paths; found {len(matches)}")
def hook(m):
    ind=m.group(1)
    return m.group(0)+f'\n\n{ind}if (auto ext = reinterpret_cast<PFN_OptiPluginInit>(\n{ind}        KernelBaseProxy::GetProcAddress_()(hMod, "OptiPluginInit")))\n{ind}    PluginApi::InitPlugin(hMod, ext, entry.path());'
dll=re.sub(pattern,hook,dll)

dx=file("OptiScaler/menu/menu_overlay_dx.cpp")
dx=one(dx,'#include "menu_common.h"','#include "menu_common.h"\n#include "plugins/PluginApi.h"',"DXGI include")
dx=one(dx,'    if (!Config::Instance()->OverlayMenu.value_or_default())\n    {\n        MenuOverlayBase::Present();','    PluginApi::SdhLiveTick(1);\n    if (!Config::Instance()->OverlayMenu.value_or_default())\n    {\n        MenuOverlayBase::Present();',"DXGI tick")
vk=file("OptiScaler/menu/menu_overlay_vk.cpp")
vk=one(vk,'#include "menu_common.h"','#include "menu_common.h"\n#include "plugins/PluginApi.h"',"Vulkan include")
vk=one(vk,"bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)\n{\n","bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)\n{\n    PluginApi::SdhLiveTick(2);\n","Vulkan tick")
evals={}
for pth,marker,source in [
    ("OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp","    const uint32_t handleId = InFeatureHandle->Id;",3),
    ("OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp","    State& state = State::Instance();\n    auto handleId = InFeatureHandle->Id;",4),
    ("OptiScaler/inputs/NVNGX_DLSS_Vk.cpp","    auto handleId = InFeatureHandle->Id;",5)]:
    data=file(pth)
    data=one(data,'#include "Config.h"','#include "Config.h"\n#include "plugins/PluginApi.h"',pth+" include")
    if source==3:
        if data.count(marker)<1:raise RuntimeError("Missing DX12 render marker")
        # Only first evaluation path, as in the original live API.
        data=data.replace(marker,'    PluginApi::SdhLiveTick(3);\n'+marker,1)
    else:
        data=one(data,marker,f'    PluginApi::SdhLiveTick({source});\n'+marker,pth+" tick")
    evals[pth]=data

proj=file("OptiScaler/OptiScaler.vcxproj")
proj=one(proj,'<ClCompile Include="menu\\menu_common.cpp" />','<ClCompile Include="plugins\\PluginApi.cpp" />\n    <ClCompile Include="menu\\menu_common.cpp" />',"MSBuild source")
# Filter entries optional; compiler does not read this file.
provider_src=Path(__file__).with_name("sdh_plugin_api_extension.inc")
if not provider_src.is_file():raise RuntimeError("Missing checked-in native provider")
provider=provider_src.read_text(encoding="utf-8")
for needle in ['CustomProviders::IsFfx','CustomUpscalerVk','FfxUpscalerIndexFor(customName)','requested_custom_upscaler=']:
    if needle not in provider:raise RuntimeError(f"Missing DEV19 custom support: {needle}")

stage={
    api_path:api, cpp_path:impl, header_path:header, provider_path:provider,
    "OptiScaler/Config.h":cfg_h,"OptiScaler/Config.cpp":cfg,
    "OptiScaler/dllmain.cpp":dll,"OptiScaler/menu/menu_overlay_dx.cpp":dx,
    "OptiScaler/menu/menu_overlay_vk.cpp":vk,
    "OptiScaler/OptiScaler.vcxproj":proj,**evals,
}
for path,data in stage.items():save(path,data)
print("DEV19 patched",len(stage),"files against modern upstream; SDK ABI v2 and custom FFX enabled.")
