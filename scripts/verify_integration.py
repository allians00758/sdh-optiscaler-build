#!/usr/bin/env python3
"""Check source patch shape before starting costly Windows compilation."""
import sys
from pathlib import Path

src = Path(sys.argv[1])
files = {
    'api': src/'plugin_sdk/optiscaler_plugin.h',
    'provider': src/'OptiScaler/plugins/SDHLiveControl.inc',
    'impl': src/'OptiScaler/plugins/PluginApi.cpp',
    'impl_h': src/'OptiScaler/plugins/PluginApi.h',
    'dx': src/'OptiScaler/menu/menu_overlay_dx.cpp',
    'vk': src/'OptiScaler/menu/menu_overlay_vk.cpp',
    'eval_dx12': src/'OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp',
    'eval_dx11': src/'OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp',
    'eval_vk': src/'OptiScaler/inputs/NVNGX_DLSS_Vk.cpp',
}
contents={k:v.read_text(encoding='utf-8') for k,v in files.items()}
checks={
    'api': ['#define OPTI_PLUGIN_API_VERSION 2','QueueSdhControl','PollSdhControl','SnapshotSdhControl'],
    'provider': ['SdhQueue(', 'SdhPoll(', 'SdhSnapshot(', 'SdhLiveRenderTick(', 'apply_stage=renderer_reinit_requested', 'tick_source_code=', 'active_backend=', 'has_shader', 'SdhChooseFfxVersion', 'has_upscaler', 'requires_restart_fg_pipeline', 'last_request_values=', 'has_latencyflex_mode', 'resource_barrier_requires_per_resource_mapping', 'selected_ffx_version=', 'hasFfx2', 'requestMajor2', 'fg_runtime_active=', 'fg_pipeline_restart_needed=', 'fg_pipeline_matches_config=', 'has_color_space', 'has_fsr4_preset', 'active_fsr4_preset=', 'active_ffx_version=', 'ffx_available_versions=', 'ffx_versions_count=', 'SdhFfxVersionNumber', 'fsr4_color_space,fsr4_preset'],
    'impl': ['#include "SDHLiveControl.inc"', '.QueueSdhControl = SdhQueue', '.PollSdhControl = SdhPoll', '.SnapshotSdhControl = SdhSnapshot', 'void PluginApi::SdhLiveTick(uint32_t source)'],
    'impl_h': ['static void SdhLiveTick(uint32_t source);'],
    'dx': ['#include "plugins/PluginApi.h"', 'PluginApi::SdhLiveTick(1);'],
    'vk': ['#include "plugins/PluginApi.h"', 'PluginApi::SdhLiveTick(2);'],
    'eval_dx12': ['#include "plugins/PluginApi.h"', 'PluginApi::SdhLiveTick(3);'],
    'eval_dx11': ['#include "plugins/PluginApi.h"', 'PluginApi::SdhLiveTick(4);'],
    'eval_vk': ['#include "plugins/PluginApi.h"', 'PluginApi::SdhLiveTick(5);'],
}
for k,needles in checks.items():
    for needle in needles:
        if needle not in contents[k]:
            raise SystemExit(f'Missing {needle!r} in {files[k]}')
p=contents['provider']
assert p.index('std::string snapshot="capabilities=') < p.index('snapshot+="ffx_versions_count="'), 'FFX diagnostics refer to an undeclared snapshot'
assert 'snapshot+="\\\\n";' not in p, 'Incorrect escaped newline in FFX version list'
# Regression cases confirmed by the Deck runtime control-state log:
# capabilities contained ffx2, but upscaler=fsr2.3 failed
# invalid_or_unsupported_backend before reaching the FFX resolver.
import re
dispatch_match = re.search(r'const bool isVersionedFfx\\s*=\\s*\\(.*?\\);', p, re.S)
assert dispatch_match is not None, 'Missing versioned FFX dispatch expression'
dispatch = dispatch_match.group(0)
assert 'code.rfind("fsr2.3",0)==0' in dispatch, 'FSR 2.3 bypasses the versioned FFX resolver'
assert 'code.rfind("fsr3",0)==0' in dispatch, 'FSR 3.x bypasses the versioned FFX resolver'
assert 'code.rfind("fsr4",0)==0' in dispatch, 'FSR 4.x bypasses the versioned FFX resolver'
assert 'code.find(\'.\')==std::string::npos' in p, 'Dotted FSR 3.1 is rewritten as legacy fsr31'
assert 'SdhChooseFfxVersion(state,state.api,code,ffxVersion)' in p, 'Missing FFX index resolution'
assert 'backend=(state.api==DX11)?Upscaler::FFX_on12:Upscaler::FFX' in p, 'Missing FFX backend selection'
print('PASS: SDH Live API v2 integration source markers verified.')
