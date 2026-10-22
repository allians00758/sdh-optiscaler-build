#!/usr/bin/env python3
"""Patch pinned HelixSR-Scaler 1.1.0 from source with SDH v11 ASI exports."""
from pathlib import Path
import argparse, shutil
p=argparse.ArgumentParser()
p.add_argument("source")
a=p.parse_args()
root=Path(a.source)
here=Path(__file__).resolve().parent
files={
 "core":root/"OptiScaler/dllmain.cpp",
 "dx":root/"OptiScaler/menu/menu_overlay_dx.cpp",
 "vk":root/"OptiScaler/menu/menu_overlay_vk.cpp",
 "dx12":root/"OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp",
 "dx11":root/"OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp",
 "vulkan":root/"OptiScaler/inputs/NVNGX_DLSS_Vk.cpp",
}
for path in files.values():
 if not path.is_file():raise RuntimeError("missing source: "+str(path))
 if "SDHLive::RenderTick" in path.read_text(encoding="utf-8"):
  raise RuntimeError("source already patched: "+str(path))
def replace(s,needle,value,what):
 n=s.count(needle)
 if n!=1:raise RuntimeError(what+" expected once; got "+str(n))
 return s.replace(needle,value,1)
mods={}
core=files["core"].read_text(encoding="utf-8")
mods["core"]=replace(core,'#include "helixsr/HelixSR.h" // HelixSR Scaler',
 '#include "helixsr/HelixSR.h" // HelixSR Scaler\n#include "sdh/SDHLiveBridgeImpl.inc"',
 'core include')
for typ,code,fragment in [
 ("dx","1",'    if (!Config::Instance()->OverlayMenu.value_or_default())\n    {\n        MenuOverlayBase::Present();'),
 ("vk","2",'bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)\n{\n    LOG_FUNC();')]:
 s=files[typ].read_text(encoding="utf-8")
 s=replace(s,'#include "menu_common.h"','#include "menu_common.h"\n#include "sdh/SDHLiveTick.h"',typ+" include")
 s=replace(s,fragment,("    SDHLive::RenderTick(1);\n"+fragment) if typ=="dx" else fragment.replace('    LOG_FUNC();','    SDHLive::RenderTick(2);\n    LOG_FUNC();'),typ+" tick")
 mods[typ]=s
for typ,fn,num in [
 ("dx12","NVSDK_NGX_D3D12_EvaluateFeature","3"),
 ("dx11","NVSDK_NGX_D3D11_EvaluateFeature","4"),
 ("vulkan","NVSDK_NGX_VULKAN_EvaluateFeature","5")]:
 s=files[typ].read_text(encoding="utf-8")
 s=replace(s,'#include "Config.h"','#include "Config.h"\n#include "sdh/SDHLiveTick.h"',typ+" include")
 start=s.find("NVSDK_NGX_API NVSDK_NGX_Result "+fn+"(")
 if start<0:raise RuntimeError("missing "+fn)
 brace=s.find("{",start)
 if brace<0 or brace-start>800:raise RuntimeError("unexpected function body "+fn)
 s=s[:brace+1]+"\n    SDHLive::RenderTick("+num+");"+s[brace+1:]
 mods[typ]=s
addon=root/"OptiScaler/sdh"
addon.mkdir(exist_ok=True)
for typ,path in files.items():
 shutil.copy2(path,path.with_suffix(path.suffix+".sdh-backup"))
 path.write_text(mods[typ],encoding="utf-8")
# DEV77: Keep native HelixSR [Sharpening] options written from Decky.
# Stock Scaler 1.1.0 rewrites Sharpener=off and deletes Sharpness
# on every startup, making valid native settings ineffective.
native_path=root/"OptiScaler/helixsr/HelixSR.cpp"
native_code=native_path.read_text(encoding="utf-8")
for original, replacement in (
    ('    ini.Set("Sharpening", "Sharpener", "off");',
     '    // SDH native settings: preserve user Sharpening.Sharpener across launch.'),
    ('const Managed kRetired[] = { { "Sharpening", "Sharpness" },',
     'const Managed kRetired[] = {')
):
 if native_code.count(original)!=1:raise RuntimeError("unexpected upstream native HelixSR scaler source: "+original)
 native_code=native_code.replace(original,replacement,1)
shutil.copy2(native_path,native_path.with_suffix(native_path.suffix+".sdh-backup"))
native_path.write_text(native_code,encoding="utf-8")

for name in ("SDHLiveTick.h","SDHLiveBridgeImpl.inc"):
 shutil.copy2(here/name,addon/name)
print("SDH source bridge injected into pinned HelixSR-Scaler 1.1.0")
