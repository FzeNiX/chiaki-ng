# Frame Generation

chiaki-ng can generate extra frames between the frames sent by the console
(for example 60 fps stream → 120 fps on a 120 Hz display). Three backends:

| Backend | Platforms | GPU | Multiplier |
|---|---|---|---|
| NVIDIA DLSS Frame Generation | Windows (VC build) | GeForce RTX 40 series or newer | 2x (3x / 4x on RTX 50 series) |
| AMD FSR 3.1 Frame Generation | Windows (VC build) | any GPU with Vulkan | 2x |
| Flow Frame Generation | Windows, Linux, Steam Deck | any GPU with Vulkan | 2x / 3x / 4x |

The DLSS and FSR runtimes (`nvngx_dlssg.dll`, `amd_fidelityfx_vk.dll`) ship
next to `chiaki.exe`; nothing extra has to be downloaded. Flow is chiaki-ng's
own interpolation and needs no runtime at all.

### Flow

Flow is built from libplacebo compute shaders, so it runs wherever the Vulkan
renderer runs. For each stream frame it

1. builds a luma pyramid (1/4 resolution and coarser),
2. estimates motion coarse to fine with symmetric block matching (seeded by
   the coarser level, neighbouring blocks and the previous frame's motion),
3. removes outliers and checks for scene cuts,
4. warps both stream frames along the motion to each in-between time and
   blends them, picking per pixel the vector that makes both frames agree.
   Where nothing agrees (areas being covered or revealed) it uses the
   temporally nearer frame; across a scene cut it repeats the nearer frame.

**Flow Quality** (Settings → Video): *Performance* analyses motion at 1/4
resolution (≈0.5 ms on an RTX 5070 Ti at 1440p), *Quality* at 1/2 resolution
(roughly 2–4x the analysis time; finer motion around small details). Use
Performance on handhelds such as the Steam Deck.

Static overlays (HUD, subtitles) are detected per pixel and kept in place,
including the anti-aliased edges and outlines of HUD text (a 1/2 resolution
mask of static pixels marks text areas);
next to them the scene's motion is looked up up to ~20 px away so the
background behind HUD text does not smear.

The overlay shows `Flow FG 2x` (`Flow FG HQ 2x` in Quality mode) and its GPU
time. Fast motion around object
edges can show some smearing in the covered/revealed areas.

## Requirements

- Renderer Backend: **Vulkan**
- Settings → Video → Frame Mapping: **Direct Mapping** (the default)
- An SDR stream (HDR streams are shown without frame generation)
- A display refresh rate of at least multiplier × stream fps
  (the multiplier is reduced automatically otherwise)

## Usage

Settings → Video → **Frame Generation**: pick DLSS, FSR or Flow, then the
**Frame Generation Multiplier** (DLSS and Flow). Works together with
[RTX Video Super Resolution](rtxvideo.md): frames are upscaled first, then
generated.

## Latency

Frame generation has to wait for the next stream frame before it can show the
frames in between, so it adds about half a stream frame interval (≈8 ms at
60 fps) plus a few milliseconds of GPU time. Turn it off for latency
sensitive games.

## Limitations

The stream carries no motion vectors or depth, so both backends get zero
motion vectors and a constant depth and rely on their own optical flow. Fast
motion and on-screen UI can show artifacts.

## If it crashes

Frame generation calls into the NVIDIA and AMD runtimes. A crash inside them is
caught where possible and only turns frame generation off for the session. If
chiaki-ng still closes while frame generation starts, it is switched off
automatically on the next launch (the log says so); turn it back on in the
settings once you want to retry.

The first frames log each step (`[framegen] frame N: ...`), which shows where
it stopped.

## Log messages

- `[framegen] ... initialized` and `Frame generation active: ... Nx` → working.
- `DLSS Frame Generation is not available on this GPU/driver` → RTX 40+ GPU and a current driver are needed.
- `Could not load ...amd_fidelityfx_vk.dll` → the DLL is missing next to `chiaki.exe`.
- `Frame generation requires Direct Mapping` → switch Frame Mapping to Direct Mapping.
