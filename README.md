![chiaki-ng Logo](gui/res/chiaking-logo.svg)

# chiaki-ng-enhanced

A fork of [chiaki-ng](https://github.com/streetpea/chiaki-ng) (the open source PlayStation 4/5 Remote Play client) that adds video enhancement and smoother streaming on top of upstream: NVIDIA RTX Video Super Resolution, three frame generation backends (DLSS, FSR and its own **Flow** interpolation that also runs on Linux and Steam Deck), an ambient border for ultrawide displays and a set of Windows streaming tweaks and fixes.

Everything else (setup, registration, controllers, Steam Deck usage) works as in upstream chiaki-ng – see its [documentation site](https://streetpea.github.io/chiaki-ng/).

## Downloads

Pre-releases are published on the [Releases](../../releases) page (tags like `1.10.0-rtx.1`):

| File | Platform |
|---|---|
| `chiaki-ng-*-win_x64-setup.exe` | Windows x64 installer |
| `chiaki-ng-*-win_x64-portable.zip` | Windows x64 portable |
| `chiaki-ng-*-linux_x86_64.AppImage` | Linux x86_64 (`chmod +x`, then run) |

Test builds of every commit are available as artifacts of the *Build chiaki-ng Windows x86_64 (VC)* and *Build chiaki-ng Appimage x86_64* workflows in the Actions tab.

## What this fork adds

### Video enhancement

| Feature | Platforms | GPU | Where |
|---|---|---|---|
| **RTX Video Super Resolution** – AI upscaling of the stream to the window size (Low / Medium / High / Ultra) | Windows | NVIDIA RTX 20 series or newer | Settings → Video → RTX Video Super Resolution |
| **DLSS Frame Generation** – 2x, 3x/4x on RTX 50 series | Windows | NVIDIA RTX 40 series or newer | Settings → Video → Frame Generation |
| **FSR 3.1 Frame Generation** – 2x | Windows | any Vulkan GPU | Settings → Video → Frame Generation |
| **Flow Frame Generation** – chiaki-ng's own optical flow interpolation, 2x–4x | Windows, Linux, Steam Deck | any Vulkan GPU | Settings → Video → Frame Generation |
| **AMD RCAS sharpening** – contrast adaptive sharpening of the final picture with a strength slider; works after upscaling, RTX VSR and frame generation, also at 1:1 | all | any | Settings → Video → Sharpening (AMD RCAS) |
| **Ambient border** – fills the bars around the video (e.g. 16:9 on an ultrawide display) with a blurred copy of the picture | all | any | Settings → Video → Border Fill |

Notes:

- RTX VSR needs `nvngx_vsr.dll` (from the NVIDIA RTX Video SDK) next to `chiaki.exe`; it is not bundled. Only the Upscale modes work with the public 1.6.0 DLL; the Artifact Reduction and Streaming modes need 1.8.2+ and are greyed out otherwise. See [RTX Video](docs/setup/rtxvideo.md).
- Frame generation needs the Vulkan renderer, Frame Delivery = Direct Mapping and a display running at least twice the stream frame rate (e.g. 120 Hz for a 60 fps stream). HDR streams are shown without it. It adds about half a stream frame of latency. See [Frame Generation](docs/setup/framegen.md).
- **Flow** needs no vendor runtime: it is built from libplacebo compute shaders (luma pyramid, coarse-to-fine block matching, per-pixel vector selection, occlusion and scene cut handling). HUD and other static overlays, including anti-aliased text, are detected and kept in place. *Flow Quality*: Performance (1/4 resolution analysis, ~0.5 ms on an RTX 5070 Ti) or Quality (1/2 resolution, a few times the GPU time). Performance is recommended.
- The DLSS (`nvngx_dlssg.dll`) and FidelityFX (`amd_fidelityfx_vk.dll`) runtimes ship with the Windows build; their licenses are in the `licenses` folder.

### In-stream controls and stats

- **Stream menu (Ctrl+O)**: switch RTX VSR and Frame Generation on/off while streaming.
- **Stats overlay**: stream fps / display fps, plus RTX VSR / frame generation state and GPU time (e.g. `Flow FG 2x 0.5 ms`).
- **Disconnect dialog**: Cancel button and × to keep streaming instead of always closing the stream.

### Streaming and stability

- Windows tuning while streaming: qWAVE QoS on the stream sockets (audio/video traffic class), MMCSS "Games" priority for the network and render threads, 1 ms timer resolution, no power throttling.
- Fixed the packet loss statistic when sequence numbers wrap around.
- Fixed a swapchain rebuild loop (and log flood) while the window is minimized.
- Crash guard: if the app crashes while frame generation starts, it is turned off on the next launch.
- Upstream fixes picked up: use-after-free crashes on session teardown / controller disconnect / exit, Takion send buffer use after the receive loop ended, reorder queue not clearing dropped entries.

### Builds

- Windows (MSVC/clang-cl) build with the NVIDIA NGX and AMD FidelityFX SDKs, vcpkg binary cache (~12 min per build).
- Linux AppImage build on every push.
- Tag-driven release workflow (`*-rtx.*` tags) publishing Windows and Linux packages with checksums.

## Building

Same as upstream chiaki-ng, plus for the Windows video features:

```
cmake -DCHIAKI_ENABLE_NGX=ON -DCHIAKI_NGX_SDK_DIR=<NVIDIA DLSS SDK> \
      -DCHIAKI_ENABLE_FFX=ON -DCHIAKI_FFX_SDK_DIR=<AMD FidelityFX SDK 1.1.x> ...
```

Flow frame generation and the ambient border need no extra SDK. See `.github/workflows/build-windows-x86.yml` for the complete Windows build.

## Credits

- [chiaki-ng](https://github.com/streetpea/chiaki-ng) by Street Pea and contributors, based on [Chiaki](https://git.sr.ht/~thestr4ng3r/chiaki) by Florian Märkl.
- NVIDIA DLSS / NGX SDK and RTX Video SDK, AMD FidelityFX SDK, [libplacebo](https://code.videolan.org/videolan/libplacebo).

## Disclaimer

This project is not endorsed or certified by Sony Interactive Entertainment LLC.

Chiaki is a Free and Open Source Software Client for PlayStation 4 and PlayStation 5 Remote Play
for Linux, FreeBSD, OpenBSD, Android, macOS, Windows, Nintendo Switch and potentially even more platforms.
