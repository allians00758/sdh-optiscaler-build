# SDH DittoSR Native Linux Vulkan — experimental

A proof-of-concept native Linux port of [Vancete/DittoSR 0.1.2](https://github.com/Vancete/DittoSR) (source commit `d053cd55891cdead6c306c9b282be7715bba7333`).

The build produces an **ELF x86_64** `libdittosr_vk.so`, with native Vulkan rendering and the same upstream DittoSR compute shader passes. No Wine or Proton is used. Exports: `ffxCreateContext`, `ffxDestroyContext`, `ffxQuery`, `ffxConfigure`, `ffxDispatch`.

**This is not a native OptiScaler or a Vulkan layer.** A Linux game must have a native FidelityFX API loader capable of loading this shared library. Simply copying it into a Windows game's directory will NOT make it work.

This experimental port supports Vulkan upscaling only, not D3D12, AMD DLL passthrough, DX11/DX12 game interception or Steam Deck FrameGen integration. Windows F9/F10 hotkeys are absent: `dittosr.ini` is reloaded when its mtime changes. Do not replace installed SDH DLLs or enable this untested module in DEV166.

Test with `./ffx_vk_smoke ./libdittosr_vk.so`; this checks exports and FFX version/quality queries, **not real GPU rendering**.

Upstream DittoSR is MIT-licensed. Retain LICENSE and THIRD-PARTY-NOTICES.md. This prototype compiles original Vulkan shaders into SPIR-V through Linux DXC.

The rest of OptiScaler's native Linux port needs a separate Vulkan interception/input adapter; existing Windows DLL hooks cannot be reused directly.
