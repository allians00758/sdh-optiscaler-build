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

# SDH policy: even if the game, Gamescope or a saved MAKO profile requests
# MAILBOX, keep the Vulkan presentation transport FIFO on MAKO FG launches.
create=root/"engine/mako-render/src/swapchain/create.cpp"
c=create.read_text(encoding="utf-8")
old='''    return applySwapchainCreateProvisioning(
        profile, maxImages, createInfo,
        frameGenerationProvisioned, spatialScalingActive,
        orderedFrameGenerationTransport,
        profile.swapchain_image_count_compatibility,
        directSpatialOutput
    );'''
new='''    const bool originalProvisioning = applySwapchainCreateProvisioning(
        profile, maxImages, createInfo,
        frameGenerationProvisioned, spatialScalingActive,
        orderedFrameGenerationTransport,
        profile.swapchain_image_count_compatibility,
        directSpatialOutput
    );
    // SDH Steam Deck policy: FG swapchains must request FIFO, regardless of
    // incoming app present mode or the private transport selection.
    if (frameGenerationProvisioned && createInfo.presentMode != VK_PRESENT_MODE_FIFO_KHR) {
        createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        return true;
    }
    return originalProvisioning;'''
if c.count(old)!=1: raise SystemExit("Unexpected MAKO source: swapchain create provisioning anchor drift")
create.write_text(c.replace(old,new,1),encoding="utf-8")

policy=root/"engine/mako-render/src/swapchain/create_policy.hpp"
p=policy.read_text(encoding="utf-8")
start=p.find('    [[nodiscard]] constexpr GamescopeScalingPresentContract\n    gamescopeScalingPresentContract(')
end=p.find('    [[nodiscard]] inline bool shouldRetrySwapchainWithApplicationMinimum(',start)
if start<0 or end<0 or p.count('gamescopeScalingPresentContract(')!=1:
    raise SystemExit("Unexpected MAKO source: Gamescope presentation contract drift")
replacement='''    [[nodiscard]] constexpr GamescopeScalingPresentContract
    gamescopeScalingPresentContract(const VkPresentModeKHR intendedPresentMode,
            const bool gamescopeScalingSurface,
            const bool privateOrderedTransport,
            const bool lowerMailboxSupported,
            const bool refreshKnown,
            const bool mailboxPresentation = false) noexcept {
        (void)intendedPresentMode;
        (void)privateOrderedTransport;
        (void)lowerMailboxSupported;
        (void)refreshKnown;
        (void)mailboxPresentation;
        // SDH FrameGen: suppress MAILBOX even for intermediate bridge images.
        // Gamescope scaling output and the lower Vulkan swapchain use FIFO.
        return {
            .lowerPresentMode = VK_PRESENT_MODE_FIFO_KHR,
            .compositorPresentMode = gamescopeScalingSurface
                ? std::optional<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_KHR}
                : std::nullopt,
        };
    }

'''
policy.write_text(p[:start]+replacement+p[end:],encoding="utf-8")
print("MAKO 5.0.0 SDH strict FIFO policy: source patched")
