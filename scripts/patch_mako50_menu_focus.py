#!/usr/bin/env python3
"""SDH FrameGen: keep MAKO FG active while Gamescope/Steam/Decky menus are focused.
Uses a narrow, fail-closed source patch against MAKO Renderer 5.0.0.
"""
from pathlib import Path
import sys

root=Path(sys.argv[1]).resolve()
p=root/"engine/mako-render/src/swapchain/profile.cpp"
s=p.read_text(encoding="utf-8")
needle="const bool suspended = eligible && focused == false;"
replacement='''// SDH FrameGen policy: do not suspend FG on Gamescope focus changes.
    // Keep the existing focus tracking and lifecycle recovery code intact.
    // Manual frame_generation_enabled and unsupported pipeline checks still apply.
    const bool suspended = false;'''
if s.count(needle)!=1:
    raise SystemExit(f"Unexpected MAKO source: expected 1 focus-suspend site, found {s.count(needle)}")
assert "this->steamMenuSuspended = suspended;" in s
p.write_text(s.replace(needle,replacement,1),encoding="utf-8")
print("MAKO 5.0.0 SDH focus policy patch: applied; fixed/adaptive FG no longer suspended by menu focus")
