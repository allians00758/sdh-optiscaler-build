#!/usr/bin/env python3
"""Apply original SDH plugin-api v2 patch with the exact HelixSR-Scaler 2.3.1 Vulkan anchor.
Changes are confined to a temporary copy. The ordinary OptiScaler build is not modified.
"""
import subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
s=(root/"patch_optiscaler_plugin_api.py").read_text(encoding="utf-8")
old="    auto handleId = InFeatureHandle->Id;\\n    if (VkContexts[handleId].feature == nullptr)"
new="    if (VkContexts[handleId].feature == nullptr)"
assert s.count(old)==2, "Unexpected SDH Vulkan anchor shape"
s=s.replace(old,new)
out=root/"_sdh_helixsr231_base_generated.py"
out.write_text(s,encoding="utf-8")
try:
    subprocess.run([sys.executable,str(out),*sys.argv[1:]],check=True)
finally:
    out.unlink(missing_ok=True)
