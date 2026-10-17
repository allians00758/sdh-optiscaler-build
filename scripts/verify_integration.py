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
    'menu': src/'OptiScaler/menu/menu_common.cpp',
}
contents={k:v.read_text(encoding='utf-8') for k,v in files.items()}
checks={
    'api': ['#define OPTI_PLUGIN_API_VERSION 2','QueueSdhControl','PollSdhControl','SnapshotSdhControl'],
    'provider': ['SdhQueue(', 'SdhPoll(', 'SdhSnapshot(', 'SdhLiveRenderTick(', 'apply_stage=renderer_reinit_requested'],
    'impl': ['#include "SDHLiveControl.inc"', '.QueueSdhControl = SdhQueue', '.PollSdhControl = SdhPoll', '.SnapshotSdhControl = SdhSnapshot', 'void PluginApi::SdhLiveTick()'],
    'impl_h': ['static void SdhLiveTick();'],
    'menu': ['PluginApi::SdhLiveTick();'],
}
for k,needles in checks.items():
    for needle in needles:
        if needle not in contents[k]:
            raise SystemExit(f'Missing {needle!r} in {files[k]}')
print('PASS: SDH Live API v2 integration source markers verified.')
