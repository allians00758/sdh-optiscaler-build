#!/usr/bin/env python3
"""Check fork-specific HelixSR 2.3.1 Live API integration and run the upstream
SDH v2 static verification with fork-specific native route anchors.
"""
from pathlib import Path
import sys
root=Path(sys.argv[1]).resolve()
inc=(root/"OptiScaler/plugins/SDHLiveControl.inc").read_text(encoding="utf-8")
cpp=(root/"OptiScaler/plugins/PluginApi.cpp").read_text(encoding="utf-8")
required=[
    'SdhChooseHlxFsr4(', 'hlx_fsr4_unavailable',
    'wantsHlxDlss', 'hlx_dlss_unavailable', 'HelixSR::FsrDllFound()',
    'active_helix_upscaler=', 'active_helix_ffx_name=',
    'ffx_available_names=', 'fg_pipeline_restart_needed=',
    'has_helix_multiplier', 'StreamlineHooks::updateDlssgOptions();'
]
for token in required:
    if token not in inc:
        raise SystemExit(f"HelixSR Live verification failed: missing {token}")
if '#include "helixsr/HelixSR.h"' not in cpp:
    raise SystemExit("HelixSR bridge header missing")
if 'backend=(state.api==DX11 || (state.api==Vulkan && !usesVulkanFFX))?' not in inc:
    raise SystemExit("No DX12 FFX interop routing for HelixSR")
# Existing integration static asserts were written for stock API v2.
base=(Path(__file__).parent/"verify_integration.py").read_text(encoding="utf-8")
a='backend=(state.api==DX11)?Upscaler::FFX_on12:Upscaler::FFX'
b='backend=(state.api==DX11 || (state.api==Vulkan && !usesVulkanFFX))?'
if base.count(a)!=1:raise SystemExit("Unknown SDH verify_integration.py backend anchor")
base=base.replace(a,b,1)
a='if(isVersionedFfx) {'
b='if(wantsHlx || isVersionedFfx) {'
if base.count(a)!=1:raise SystemExit("Unknown SDH verify_integration.py FFX anchor")
base=base.replace(a,b,1)
exec(compile(base,"verify_integration.py","exec"),{"__name__":"__main__","__file__":str(Path(__file__).parent/"verify_integration.py")})
print("PASS: HelixSR 2.3.1 DX12/Vulkan FFX / HLX native command bindings verified")
