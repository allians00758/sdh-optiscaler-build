#!/usr/bin/env python3
"""Validate SDH Live API master port and Steam Deck FSR4 model policy."""
from pathlib import Path
import sys
root=Path(sys.argv[1]).resolve()
files={
    "vk":root/"OptiScaler/menu/menu_overlay_vk.cpp",
    "dx":root/"OptiScaler/menu/menu_overlay_dx.cpp",
    "bridge":root/"OptiScaler/menu/SDHLiveBridge.inc",
    "config":root/"OptiScaler/Config.h",
}
data={k:p.read_text(encoding="utf-8") for k,p in files.items()}
checks={
"vk":["SDHLiveBridge.inc","SDHLiveTick(2);"],
"dx":["extern void SDHLiveTick","SDHLiveTick(1);"],
"bridge":["QueueSdhControl","SDHPluginApiV2","OptiPluginInit","SdhLiveRenderTick",
          "fsr4_model","has_fsr4_model","FSR4Model::INT8","FSR4Model::INT8_2","FSR4Model::INT8Lite",
          "invalid_or_forbidden_fsr4_model","renderer_reinit_requested","SDHLiveTick(uint32_t source)"],
"config":["Fsr4Model"],
}
for k,needles in checks.items():
    for n in needles:
        if n not in data[k]:raise SystemExit("MISSING "+k+" "+n)
a=data["bridge"]
assert 'else if(model=="1" || model=="int8")' in a
assert 'else if(model=="2" || model=="int8rdna2")' in a
assert 'else if(model=="3" || model=="int8lite")' in a
assert 'model=="0"' not in a and 'FSR4Model::FP8' not in a
assert a.count('SDHLiveTick(uint32_t source)')==1
print("PASS: SDK bridge + all legacy Live API commands + Deck INT8 model guard")
