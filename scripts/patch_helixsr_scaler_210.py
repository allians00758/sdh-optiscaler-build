#!/usr/bin/env python3
"""Fail-closed SDH Live API v2 source port for HelixSR-Scaler v2.1.0.
Patches the vendor checkout in CI only; never rewrites installed binaries.
"""
import argparse
import re
from pathlib import Path
import shutil

p=argparse.ArgumentParser()
p.add_argument("source")
args=p.parse_args()
root=Path(args.source).resolve()
here=Path(__file__).resolve().parent
def read(name):
    f=root/name
    if not f.is_file():raise RuntimeError("Missing pinned source file: "+name)
    return f.read_text(encoding="utf-8")
def replace_once(s,old,new,what):
    count=s.count(old)
    if count!=1:raise RuntimeError(f"{what}: expected 1 location, got {count}")
    return s.replace(old,new,1)
def save(name,s):
    f=root/name;f.parent.mkdir(parents=True,exist_ok=True)
    f.write_text(s,encoding="utf-8")

dll_name="OptiScaler/dllmain.cpp"
dll=read(dll_name)
dll=replace_once(dll,'#include "Config.h"','#include "Config.h"\n#include "plugins/SDHPluginApi.h"',"Loader header")
# Initialize only the SDH ASI, not arbitrary third-party plugins. Both the
# normal and load-late paths have the same plugin-init structure.
pattern=r'(?m)^([ \t]*)if \(init != nullptr\)\s*\n[ \t]*init\(\);'
hits=list(re.finditer(pattern,dll))
if len(hits)!=2:raise RuntimeError(f"Expected two ASI loading paths; got {len(hits)}")
def wrap(m):
    indent=m.group(1)
    return m.group(0)+(
        f'\n{indent}// HelixSR SDH protocol v2 is provided by the adapter in this DLL.\n'
        f'{indent}if (fileName == L"sdh-optiscaler-control.asi")\n'
        f'{indent}{{\n'
        f'{indent}    auto entrypoint = reinterpret_cast<PFN_SdhInit>(\n'
        f'{indent}        KernelBaseProxy::GetProcAddress_()(hMod, "OptiPluginInit"));\n'
        f'{indent}    if (entrypoint) SDHPluginApi::Init(hMod, entrypoint);\n'
        f'{indent}}}'
    )
dll=re.sub(pattern,wrap,dll)

dx_name="OptiScaler/menu/menu_overlay_dx.cpp"
dx=read(dx_name)
dx=replace_once(dx,'#include "menu_common.h"','#include "menu_common.h"\n#include "plugins/SDHPluginApi.h"',"DX header")
dx=replace_once(dx,
    '    if (!Config::Instance()->OverlayMenu.value_or_default())\n    {\n        MenuOverlayBase::Present();',
    '    SDHPluginApi::Tick(1);\n    if (!Config::Instance()->OverlayMenu.value_or_default())\n    {\n        MenuOverlayBase::Present();',
    "DX render tick")
vk_name="OptiScaler/menu/menu_overlay_vk.cpp"
vk=read(vk_name)
vk=replace_once(vk,'#include "menu_common.h"','#include "menu_common.h"\n#include "plugins/SDHPluginApi.h"',"VK header")
vk=replace_once(vk,
    "bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)\n{\n",
    "bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)\n{\n    SDHPluginApi::Tick(2);\n",
    "VK render tick")
proj_name="OptiScaler/OptiScaler.vcxproj"
proj=read(proj_name)
proj=replace_once(proj,
    '<ClCompile Include="menu\\menu_common.cpp" />',
    '<ClCompile Include="plugins\\SDHPluginApi.cpp" />\n    <ClCompile Include="menu\\menu_common.cpp" />',
    "MSBuild registration")
source=(here/"SDHHelixLiveApi.cpp").read_text(encoding="utf-8")
header=(here/"SDHHelixLiveApi.h").read_text(encoding="utf-8")
for marker in ("core_tick=","ffx_available_versions","HLX FSR 4","unsupported_live_flag"):
    if marker not in source:raise RuntimeError("Missing Live provider feature "+marker)
for name,data in [(dll_name,dll),(dx_name,dx),(vk_name,vk),(proj_name,proj),
                  ("OptiScaler/plugins/SDHPluginApi.cpp",source),
                  ("OptiScaler/plugins/SDHPluginApi.h",header)]:
    save(name,data)
print("SDH Live API v2 installed in pinned HelixSR Scaler source (6 files)")
