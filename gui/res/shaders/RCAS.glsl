// Robust Contrast Adaptive Sharpening (RCAS) for the final output image.
//
// Implements the RCAS algorithm of AMD FidelityFX Super Resolution 1.0 on RGB
// (https://github.com/GPUOpen-Effects/FidelityFX-FSR, MIT licence) as an mpv
// style hook for libplacebo. It runs on the OUTPUT stage, i.e. after scaling,
// RTX Video Super Resolution and frame generation, at display resolution, so
// it also sharpens 1:1 output (no upscaling involved).
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

//!PARAM rcas_stops
//!DESC Sharpness reduction in stops: 0 = strongest, 2 = weak (set by chiaki-ng)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 4.0
1.0

//!HOOK OUTPUT
//!BIND HOOKED
//!DESC AMD FidelityFX RCAS (sharpening)

// Limit of the sharpening lobe; beyond it results look unnatural.
#define RCAS_LIMIT (0.25 - (1.0 / 16.0))

vec4 hook()
{
    //    b
    //  d e f
    //    h
    vec3 b = HOOKED_texOff(vec2( 0.0, -1.0)).rgb;
    vec3 d = HOOKED_texOff(vec2(-1.0,  0.0)).rgb;
    vec4 center = HOOKED_texOff(vec2(0.0, 0.0));
    vec3 e = center.rgb;
    vec3 f = HOOKED_texOff(vec2( 1.0,  0.0)).rgb;
    vec3 h = HOOKED_texOff(vec2( 0.0,  1.0)).rgb;

    // Min and max of the ring, per channel.
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));

    // Limiters: how much the ring may be subtracted before clipping at 0 or 1.
    vec3 hit_min = mn4 / max(4.0 * mx4, vec3(1e-5));
    vec3 hit_max = (1.0 - mx4) / min(4.0 * mn4 - 4.0, vec3(-1e-5));
    vec3 lobe_rgb = max(-hit_min, hit_max);
    float lobe = max(-RCAS_LIMIT, min(max(lobe_rgb.r, max(lobe_rgb.g, lobe_rgb.b)), 0.0))
               * exp2(-rcas_stops);

    // Noise detection: less sharpening where the centre stands out from an
    // otherwise flat neighbourhood (grain, compression noise).
    float bl = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dl = d.b * 0.5 + (d.r * 0.5 + d.g);
    float el = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fl = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hl = h.b * 0.5 + (h.r * 0.5 + h.g);
    float range = max(max(max(bl, dl), max(el, fl)), hl) - min(min(min(bl, dl), min(el, fl)), hl);
    float nz = 0.25 * (bl + dl + fl + hl) - el;
    nz = clamp(abs(nz) / max(range, 1e-5), 0.0, 1.0);
    lobe *= -0.5 * nz + 1.0;

    // Resolve: weighted sum of the ring and the centre.
    vec3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    return vec4(clamp(c, 0.0, 1.0), center.a);
}
