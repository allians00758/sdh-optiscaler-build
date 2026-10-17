#!/usr/bin/env python3
"""Apply SDH Live API v2 to a pristine OptiScaler plugin-api branch checkout.
Never patches an opaque PE binary. Requires upstream source; can be rerun safely.
"""
import argparse
from pathlib import Path
import shutil

parser=argparse.ArgumentParser()
parser.add_argument('source', help='OptiScaler source repository root, plugin-api branch')
args=parser.parse_args()
root=Path(args.source).expanduser().resolve()
api=root/'plugin_sdk/optiscaler_plugin.h'
impl=root/'OptiScaler/plugins/PluginApi.cpp'
header=root/'OptiScaler/plugins/PluginApi.h'
dx=root/'OptiScaler/menu/menu_overlay_dx.cpp'
vk=root/'OptiScaler/menu/menu_overlay_vk.cpp'
for p in (api,impl,header,dx,vk):
    if not p.is_file():raise SystemExit(f'Missing file: {p}. Use upstream plugin-api branch, not master.')
if (root/'OptiScaler/plugins/SDHLiveControl.inc').exists():
    raise SystemExit('Already patched or a pre-existing custom SDHLiveControl.inc exists.')

def once(s,needle,replace,p):
    n=s.count(needle)
    if n!=1:raise SystemExit(f'Expected exactly one match in {p}: {needle[:70]!r}, got {n}')
    return s.replace(needle,replace,1)

# Stage all changes before writing. Strict marker checks prevent partial patches.
a=api.read_text(encoding='utf-8')
a=once(a,'#define OPTI_PLUGIN_API_VERSION 1','#define OPTI_PLUGIN_API_VERSION 2',api)
needle='''        void (*Log)(OptiPlugin* plugin, OptiLogLevel level, const char* message);
'''
replacement=needle+'''
        // Version 2: optional render-thread SDH live-control provider.
        // These callbacks are intentionally appended, preserving ABI v1.
        bool (*QueueSdhControl)(OptiPlugin* plugin, uint64_t seq, const char* command);
        uint32_t (*PollSdhControl)(OptiPlugin* plugin, uint64_t seq, char* result, uint32_t size);
        uint32_t (*SnapshotSdhControl)(OptiPlugin* plugin, char* result, uint32_t size);
'''
a=once(a,needle,replacement,api)

c=impl.read_text(encoding='utf-8')
c=once(c,'#include "Config.h"','#include "Config.h"\n#include "State.h"\n#include "upscalers/IFeature.h"\n#include <algorithm>\n#include <atomic>\n#include <deque>\n#include <map>\n#include <cmath>\n#include <cstring>\n#include <string_view>',impl)
c=once(c,'const OptiPluginApi _api = {','#include "SDHLiveControl.inc"\n\nconst OptiPluginApi _api = {',impl)
c=once(c,'    .Log = Api_Log,','    .Log = Api_Log,\n    .QueueSdhControl = SdhQueue,\n    .PollSdhControl = SdhPoll,\n    .SnapshotSdhControl = SdhSnapshot,',impl)
c=once(c,'} // namespace\n\nvoid PluginApi::InitPlugin','} // namespace\n\nvoid PluginApi::SdhLiveTick() { SdhLiveRenderTick(); }\n\nvoid PluginApi::InitPlugin',impl)

h=header.read_text(encoding='utf-8')
h=once(h,'  public:\n    struct TabInfo','  public:\n    static void SdhLiveTick();\n\n    struct TabInfo',header)

# Tick at actual DXGI/Vulkan presentation, even with the overlay enabled.
# MenuCommon::Present() is not used on that active-overlay path.
d=dx.read_text(encoding='utf-8')
d=once(d, '#include "menu_common.h"', '#include "menu_common.h"\n#include "plugins/PluginApi.h"',dx)
d=once(d, '    if (!Config::Instance()->OverlayMenu.value_or_default())\n    {\n        MenuOverlayBase::Present();', '    PluginApi::SdhLiveTick();\n    if (!Config::Instance()->OverlayMenu.value_or_default())\n    {\n        MenuOverlayBase::Present();', dx)
v=vk.read_text(encoding='utf-8')
v=once(v, '#include "menu_common.h"', '#include "menu_common.h"\n#include "plugins/PluginApi.h"',vk)
v=once(v, 'bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)\n{\n    LOG_FUNC();','bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)\n{\n    PluginApi::SdhLiveTick();\n    LOG_FUNC();',vk)

stages={api:a,impl:c,header:h,dx:d,vk:v}
for p in stages:
    shutil.copy2(p,p.with_suffix(p.suffix+'.sdh-live-backup'))
for p,t in stages.items():p.write_text(t,encoding='utf-8')
shutil.copy2(Path(__file__).with_name('sdh_plugin_api_extension.inc'),root/'OptiScaler/plugins/SDHLiveControl.inc')
print('Patched 5 upstream files (DXGI/Vulkan Present); installed SDHLiveControl.inc')
print('Build OptiScaler using its official MSVC/CMake Windows x64 build instructions.')
