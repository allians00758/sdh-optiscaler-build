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
    'provider': ['SdhQueue(', 'SdhPoll(', 'SdhSnapshot(', 'SdhLiveRenderTick(', 'apply_stage=renderer_reinit_requested', 'tick_source_code='],
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
print('PASS: SDH Live API v2 integration source markers verified.')
