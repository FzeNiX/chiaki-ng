#include "flowfg.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders.h>
#include <libplacebo/shaders/custom.h>

#include <QLoggingCategory>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <vector>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui);

namespace {

// The analysis starts at 1/4 (or 1/2, see setAnalysisScale) of the display
// resolution.
constexpr int kDefaultBaseScale = 4;
constexpr int kMaxLevels = 6;
// The coarsest level is at most this small (in its shorter dimension).
constexpr int kCoarsestMin = 24;
// Exhaustive search radius on the coarsest level, in its texels.
constexpr int kCoarseRadius = 4;
constexpr int kMaxTimers = 32;

// Shared by the analysis passes.
const char *kCommonHeader = R"GLSL(
float flow_luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
)GLSL";

// Color (display size) -> luma at 1/4 size: average of the covered block.
const char *kDownColorBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= int(fgDstSize.x) || p.y >= int(fgDstSize.y))
    return;
vec2 scale = fgSrcSize / fgDstSize;
vec2 base = vec2(p) * scale;
vec2 inv = 1.0 / fgSrcSize;
float s = 0.0;
for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++)
        s += flow_luma(textureLod(fgSrc, (base + scale * (vec2(i, j) * 0.5 + 0.25)) * inv, 0.0).rgb);
imageStore(fgDst, p, vec4(s * 0.25, 0.0, 0.0, 1.0));
)GLSL";

// Luma level -> next coarser level (half size).
const char *kDownLumaBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= int(fgDstSize.x) || p.y >= int(fgDstSize.y))
    return;
vec2 scale = fgSrcSize / fgDstSize;
vec2 base = vec2(p) * scale;
vec2 inv = 1.0 / fgSrcSize;
float s = 0.0;
for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++)
        s += textureLod(fgSrc, (base + scale * (vec2(i, j) * 0.5 + 0.25)) * inv, 0.0).r;
imageStore(fgDst, p, vec4(s * 0.25, 0.0, 0.0, 1.0));
)GLSL";

// Matching cost of the symmetric motion `v` (in level texels) at texel
// centre `c`: mean absolute luma difference over a 3x3 patch.
const char *kSearchHeader = R"GLSL(
float flow_cost(vec2 c, vec2 v)
{
    vec2 inv = 1.0 / fgSize;
    vec2 h = 0.5 * v;
    float s = 0.0;
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            vec2 d = vec2(i, j) * fgPatchStep;
            float a = textureLod(fgPrevL, (c + d - h) * inv, 0.0).r;
            float b = textureLod(fgCurL, (c + d + h) * inv, 0.0).r;
            s += abs(a - b);
        }
    }
    return s * (1.0 / 9.0);
}

// Small penalties keep the field smooth and static areas (HUD, flat
// colour) at zero motion when nothing matches clearly better.
float flow_score(vec2 c, vec2 v, vec2 pred)
{
    float dev = min(length(v - pred), 8.0);
    float still = dot(v, v) > 0.0 ? 0.004 : 0.0;
    return flow_cost(c, v) + 0.003 * dev + still;
}

void flow_try(vec2 c, vec2 v, vec2 pred, inout vec2 best, inout float best_score)
{
    float m = length(v);
    if (m > fgMaxFlow)
        v *= fgMaxFlow / m;
    float s = flow_score(c, v, pred);
    if (s < best_score) {
        best_score = s;
        best = v;
    }
}
)GLSL";

const char *kSearchBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= int(fgSize.x) || p.y >= int(fgSize.y))
    return;
vec2 c = vec2(p) + 0.5;
vec2 best = vec2(0.0);
float best_score = 1e9;
vec2 pred = vec2(0.0);

if (fgHasCoarse > 0.5) {
    // Coarser motion (stored in display pixels) seeds this level.
    vec2 cc = c * (fgCoarseSize / fgSize);
    pred = textureLod(fgCoarse, cc / fgCoarseSize, 0.0).xy / fgToFull;
    ivec2 cp = ivec2(floor(cc));
    ivec2 cmax = ivec2(fgCoarseSize) - 1;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
            flow_try(c, texelFetch(fgCoarse, clamp(cp + ivec2(i, j), ivec2(0), cmax), 0).xy / fgToFull,
                     pred, best, best_score);
} else {
    for (int j = -fgRadius; j <= fgRadius; j++)
        for (int i = -fgRadius; i <= fgRadius; i++)
            flow_try(c, vec2(i, j), pred, best, best_score);
}
flow_try(c, vec2(0.0), pred, best, best_score);
if (fgHasTemporal > 0.5)
    flow_try(c, textureLod(fgTemporal, c / fgSize, 0.0).xy / fgToFull, pred, best, best_score);

// Local refinement down to half a texel.
for (int k = 0; k < 2; k++) {
    float stride = k == 0 ? 1.0 : 0.5;
    vec2 centre = best;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
            if (i != 0 || j != 0)
                flow_try(c, centre + stride * vec2(i, j), pred, best, best_score);
}
imageStore(fgDst, p, vec4(best * fgToFull, flow_cost(c, best), 1.0));
)GLSL";

// Outlier removal on the finest level: each texel re-picks the best of its
// neighbours' vectors (scored at its own position).
const char *kCleanBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= int(fgSize.x) || p.y >= int(fgSize.y))
    return;
vec2 c = vec2(p) + 0.5;
ivec2 pmax = ivec2(fgSize) - 1;
vec2 own = texelFetch(fgCoarse, p, 0).xy / fgToFull;
vec2 best = own;
float best_score = flow_cost(c, own);
for (int j = -1; j <= 1; j++) {
    for (int i = -1; i <= 1; i++) {
        if (i == 0 && j == 0)
            continue;
        vec2 v = texelFetch(fgCoarse, clamp(p + ivec2(i, j), ivec2(0), pmax), 0).xy / fgToFull;
        // A slight preference for keeping the own vector avoids flicker.
        float s = flow_cost(c, v) + 0.002;
        if (s < best_score) {
            best_score = s;
            best = v;
        }
    }
}
imageStore(fgDst, p, vec4(best * fgToFull, best_score, 1.0));
)GLSL";

// Scene cut statistics over the coarsest level, by a single invocation.
const char *kStatsBody = R"GLSL(
if (gl_GlobalInvocationID.x != 0u || gl_GlobalInvocationID.y != 0u)
    return;
ivec2 n = ivec2(fgSize);
float sum = 0.0;
float bad = 0.0;
for (int y = 0; y < n.y; y++) {
    for (int x = 0; x < n.x; x++) {
        float cost = texelFetch(fgCoarse, ivec2(x, y), 0).z;
        sum += cost;
        bad += cost > 0.08 ? 1.0 : 0.0;
    }
}
float count = float(max(n.x * n.y, 1));
imageStore(fgDst, ivec2(0), vec4(sum / count, bad / count, 0.0, 1.0));
)GLSL";

// Per pixel, the vector is picked among the nearest analysis vectors (and
// their bilinear mix) by how well the two warped samples agree, so object
// edges do not inherit a blend of object and background motion.
const char *kSynthHeader = R"GLSL(
float flow_luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float flow_diff(vec3 x, vec3 y)
{
    vec3 d = abs(x - y);
    return (d.r + d.g + d.b) * (1.0 / 3.0);
}

void flow_pick(vec2 c, vec2 v, vec2 vb, vec2 inv, inout vec3 best_a, inout vec3 best_b, inout float best_err,
               inout vec2 best_v)
{
    vec3 a = textureLod(fgPrevC, (c - fgT * v) * inv, 0.0).rgb;
    vec3 b = textureLod(fgCurC, (c + (1.0 - fgT) * v) * inv, 0.0).rgb;
    vec3 d = abs(a - b);
    float err = (d.r + d.g + d.b) * (1.0 / 3.0) + 0.01 * min(length(v - vb) * 0.125, 1.0);
    // A moving vector whose both ends land on content that does not change
    // between the frames (a static overlay such as the HUD) only matches by
    // accident; static content is explained by zero motion.
    if (dot(v, v) > 0.25) {
        float still_a = flow_diff(a, textureLod(fgCurC, (c - fgT * v) * inv, 0.0).rgb);
        float still_b = flow_diff(b, textureLod(fgPrevC, (c + (1.0 - fgT) * v) * inv, 0.0).rgb);
        if (max(still_a, still_b) < 0.008)
            err += 0.15;
    }
    if (err < best_err) {
        best_err = err;
        best_a = a;
        best_b = b;
        best_v = v;
    }
}

// How much the pixel at `q` changes between the two frames; ~0 for HUD and
// other overlays that stay in place.
float flow_change(vec2 q, vec2 inv)
{
    vec3 x = textureLod(fgPrevC, q * inv, 0.0).rgb;
    vec3 y = textureLod(fgCurC, q * inv, 0.0).rgb;
    return flow_diff(x, y);
}
)GLSL";

const char *kSynthBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= int(fgSize.x) || p.y >= int(fgSize.y))
    return;
vec2 inv = 1.0 / fgSize;
vec2 c = vec2(p) + 0.5;
vec4 st = texelFetch(fgStats, ivec2(0), 0);
vec3 res;
if (st.x > 0.12 || st.y > 0.45) {
    // Scene cut: no motion to follow, repeat the nearer frame.
    res = fgT < 0.5 ? texelFetch(fgPrevC, p, 0).rgb : texelFetch(fgCurC, p, 0).rgb;
} else {
    vec2 fc = c * (fgFlowSize / fgSize);
    vec2 vb = textureLod(fgFlow, fc / fgFlowSize, 0.0).xy;
    vec3 a = vec3(0.0);
    vec3 b = vec3(0.0);
    float err = 1e9;
    vec2 bv = vb;
    flow_pick(c, vb, vb, inv, a, b, err, bv);
    ivec2 fp = ivec2(floor(fc - 0.5));
    ivec2 fmax = ivec2(fgFlowSize) - 1;
    for (int j = 0; j <= 1; j++)
        for (int i = 0; i <= 1; i++)
            flow_pick(c, texelFetch(fgFlow, clamp(fp + ivec2(i, j), ivec2(0), fmax), 0).xy, vb, inv, a, b, err, bv);
    // Two outer rings reaching ~6 and ~20 display pixels (whatever the
    // analysis scale): next to a static overlay or a small object the blocks
    // nearby carry its motion, the surface around it is found further out.
    for (int r = 0; r < 2; r++) {
        int reach = int(r == 0 ? fgReach : fgReachFar);
        for (int j = -reach; j <= 1 + reach; j += 1 + 2 * reach)
            for (int i = -reach; i <= 1 + reach; i += 1 + 2 * reach)
                flow_pick(c, texelFetch(fgFlow, clamp(fp + ivec2(i, j), ivec2(0), fmax), 0).xy, vb, inv, a, b, err, bv);
    }
    // Zero motion is always a candidate (static overlays, HUD).
    flow_pick(c, vec2(0.0), vb, inv, a, b, err, bv);
    vec3 blended = mix(a, b, fgT);
    // Where no vector makes both frames agree (occlusions), lean on the
    // temporally nearer frame instead of blending two different surfaces.
    float w = smoothstep(0.06, 0.20, err);
    // If one side of the warp lands on a static overlay (HUD) while this
    // pixel moves, the surface behind it is only visible in the other frame.
    float side_t = fgT;
    if (w > 0.0) {
        float change_a = flow_change(c - fgT * bv, inv);
        float change_b = flow_change(c + (1.0 - fgT) * bv, inv);
        // The sample on the overlay barely changes between the frames.
        if (min(change_a, change_b) < 0.01 && abs(change_a - change_b) > 0.003)
            side_t = change_a < change_b ? 1.0 : 0.0;
    }
    res = mix(blended, side_t < 0.5 ? a : b, w);

    // Pixels that are the same in both frames (HUD, overlays, static
    // scenery) keep their place: the 1/4 resolution analysis would otherwise
    // drag them along with the motion of the surrounding blocks.
    ivec2 pmax = ivec2(fgSize) - 1;
    vec3 p0 = texelFetch(fgPrevC, p, 0).rgb;
    vec3 c0 = texelFetch(fgCurC, p, 0).rgb;
    float still = flow_diff(p0, c0);
    still = max(still, flow_diff(texelFetch(fgPrevC, clamp(p + ivec2(1, 0), ivec2(0), pmax), 0).rgb,
                                 texelFetch(fgCurC, clamp(p + ivec2(1, 0), ivec2(0), pmax), 0).rgb));
    still = max(still, flow_diff(texelFetch(fgPrevC, clamp(p - ivec2(1, 0), ivec2(0), pmax), 0).rgb,
                                 texelFetch(fgCurC, clamp(p - ivec2(1, 0), ivec2(0), pmax), 0).rgb));
    still = max(still, flow_diff(texelFetch(fgPrevC, clamp(p + ivec2(0, 1), ivec2(0), pmax), 0).rgb,
                                 texelFetch(fgCurC, clamp(p + ivec2(0, 1), ivec2(0), pmax), 0).rgb));
    still = max(still, flow_diff(texelFetch(fgPrevC, clamp(p - ivec2(0, 1), ivec2(0), pmax), 0).rgb,
                                 texelFetch(fgCurC, clamp(p - ivec2(0, 1), ivec2(0), pmax), 0).rgb));
    res = mix(mix(p0, c0, fgT), res, smoothstep(0.006, 0.02, still));
}
imageStore(fgOutImg, p, vec4(res, 1.0));
)GLSL";

struct pl_shader_desc sampledDesc(const char *name, pl_tex tex, bool linear = true)
{
    struct pl_shader_desc d = {};
    d.desc.name = name;
    d.desc.type = PL_DESC_SAMPLED_TEX;
    d.binding.object = tex;
    d.binding.sample_mode = linear ? PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
    d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
    return d;
}

struct pl_shader_desc storageDesc(const char *name, pl_tex tex)
{
    struct pl_shader_desc d = {};
    d.desc.name = name;
    d.desc.type = PL_DESC_STORAGE_IMG;
    d.desc.access = PL_DESC_ACCESS_WRITEONLY;
    d.binding.object = tex;
    return d;
}

struct pl_shader_var floatVar(const char *name, const float *data)
{
    struct pl_shader_var v = {};
    v.var = pl_var_float(name);
    v.data = data;
    return v;
}

struct pl_shader_var vec2Var(const char *name, const float *data)
{
    struct pl_shader_var v = {};
    v.var = pl_var_vec2(name);
    v.data = data;
    return v;
}

struct pl_shader_var intVar(const char *name, const int *data)
{
    struct pl_shader_var v = {};
    v.var = pl_var_int(name);
    v.data = data;
    return v;
}

} // namespace

struct FlowInterpolator::Impl
{
    struct Pyramid
    {
        pl_tex source = nullptr; // the colour texture it was built from
        std::array<pl_tex, kMaxLevels> luma = {};
    };

    pl_log log = nullptr;
    pl_gpu gpu = nullptr;
    pl_dispatch dp = nullptr;
    pl_fmt fmt = nullptr;

    int base_scale = kDefaultBaseScale;
    int width = 0;
    int height = 0;
    int levels = 0;
    std::array<int, kMaxLevels> lw = {};
    std::array<int, kMaxLevels> lh = {};
    std::array<Pyramid, 2> pyramids;
    std::array<pl_tex, kMaxLevels> flow = {};
    std::array<pl_tex, 2> final_flow = {};
    int final_index = 0;
    bool have_temporal = false;
    pl_tex stats = nullptr;
    // Bound where a pass needs no input (every descriptor of a pass must be
    // a distinct texture).
    pl_tex dummy = nullptr;
    bool duplicate_warned = false;

    std::vector<pl_timer> timers;
    int timer_cursor = 0;
    float gpu_ms = 0.0f;

    void destroyTextures();
    bool ensureSize(int w, int h);
    pl_tex makeTex(int w, int h);
    pl_timer nextTimer();
    void collectTimers();
    bool run(const char *description, const char *header, const char *body,
             std::initializer_list<struct pl_shader_desc> descs,
             std::initializer_list<struct pl_shader_var> vars,
             int w, int h, int group);
    bool buildPyramid(Pyramid &pyr, pl_tex color);
};

pl_tex FlowInterpolator::Impl::makeTex(int w, int h)
{
    struct pl_tex_params params = {};
    params.w = w;
    params.h = h;
    params.format = fmt;
    params.sampleable = true;
    params.storable = true;
    return pl_tex_create(gpu, &params);
}

void FlowInterpolator::Impl::destroyTextures()
{
    for (auto &pyr : pyramids) {
        for (auto &tex : pyr.luma)
            pl_tex_destroy(gpu, &tex);
        pyr.source = nullptr;
    }
    for (auto &tex : flow)
        pl_tex_destroy(gpu, &tex);
    for (auto &tex : final_flow)
        pl_tex_destroy(gpu, &tex);
    pl_tex_destroy(gpu, &stats);
    pl_tex_destroy(gpu, &dummy);
    width = height = levels = 0;
    have_temporal = false;
}

bool FlowInterpolator::Impl::ensureSize(int w, int h)
{
    if (w == width && h == height && levels > 0)
        return true;
    destroyTextures();

    int cw = std::max(1, (w + base_scale - 1) / base_scale);
    int ch = std::max(1, (h + base_scale - 1) / base_scale);
    int n = 0;
    while (n < kMaxLevels) {
        lw[n] = cw;
        lh[n] = ch;
        n++;
        if (std::min(cw, ch) <= kCoarsestMin)
            break;
        cw = std::max(1, (cw + 1) / 2);
        ch = std::max(1, (ch + 1) / 2);
    }
    for (int l = 0; l < n; l++) {
        for (auto &pyr : pyramids) {
            pyr.luma[l] = makeTex(lw[l], lh[l]);
            if (!pyr.luma[l])
                return false;
        }
        flow[l] = makeTex(lw[l], lh[l]);
        if (!flow[l])
            return false;
    }
    for (auto &tex : final_flow) {
        tex = makeTex(lw[0], lh[0]);
        if (!tex)
            return false;
    }
    stats = makeTex(1, 1);
    dummy = makeTex(1, 1);
    if (!stats || !dummy)
        return false;
    width = w;
    height = h;
    levels = n;
    qCInfo(chiakiGui).nospace() << "[framegen] Flow analysis at " << lw[0] << "x" << lh[0]
                                << " down to " << lw[n - 1] << "x" << lh[n - 1] << " (" << n << " levels)";
    return true;
}

pl_timer FlowInterpolator::Impl::nextTimer()
{
    if (timer_cursor >= static_cast<int>(timers.size()))
        return nullptr;
    return timers[static_cast<size_t>(timer_cursor++)];
}

void FlowInterpolator::Impl::collectTimers()
{
    // Each timer is used at most once per call, so draining all of them gives
    // roughly one call's worth of GPU time.
    uint64_t total_ns = 0;
    bool any = false;
    for (pl_timer timer : timers) {
        if (!timer)
            continue;
        uint64_t ns;
        while ((ns = pl_timer_query(gpu, timer)) != 0) {
            total_ns += ns;
            any = true;
        }
    }
    if (any) {
        const float ms = static_cast<float>(total_ns) / 1e6f;
        gpu_ms = gpu_ms > 0.0f ? gpu_ms * 0.9f + ms * 0.1f : ms;
    }
    timer_cursor = 0;
}

bool FlowInterpolator::Impl::run(const char *description, const char *header, const char *body,
                                 std::initializer_list<struct pl_shader_desc> descs,
                                 std::initializer_list<struct pl_shader_var> vars,
                                 int w, int h, int group)
{
    // libplacebo rejects (and logs with a backtrace, every frame) a pass that
    // binds the same texture twice; catch it here instead.
    for (auto a = descs.begin(); a != descs.end(); ++a) {
        for (auto b = descs.begin(); b != a; ++b) {
            if (a->binding.object == b->binding.object) {
                if (!duplicate_warned)
                    qCWarning(chiakiGui) << "[framegen] Flow shader binds a texture twice:" << description;
                duplicate_warned = true;
                return false;
            }
        }
    }
    pl_shader sh = pl_dispatch_begin(dp);
    struct pl_custom_shader params = {};
    params.description = description;
    params.header = header;
    params.body = body;
    params.input = PL_SHADER_SIG_NONE;
    params.output = PL_SHADER_SIG_NONE;
    params.descriptors = descs.begin();
    params.num_descriptors = static_cast<int>(descs.size());
    params.variables = vars.begin();
    params.num_variables = static_cast<int>(vars.size());
    params.compute = true;
    params.compute_group_size[0] = group;
    params.compute_group_size[1] = group;
    if (!pl_shader_custom(sh, &params)) {
        pl_dispatch_abort(dp, &sh);
        qCWarning(chiakiGui) << "[framegen] Flow shader setup failed:" << description;
        return false;
    }
    struct pl_dispatch_compute_params compute = {};
    compute.shader = &sh;
    compute.dispatch_size[0] = (w + group - 1) / group;
    compute.dispatch_size[1] = (h + group - 1) / group;
    compute.dispatch_size[2] = 1;
    compute.timer = nextTimer();
    if (!pl_dispatch_compute(dp, &compute)) {
        qCWarning(chiakiGui) << "[framegen] Flow shader dispatch failed:" << description;
        return false;
    }
    return true;
}

bool FlowInterpolator::Impl::buildPyramid(Pyramid &pyr, pl_tex color)
{
    pyr.source = nullptr;
    for (int l = 0; l < levels; l++) {
        pl_tex src = l == 0 ? color : pyr.luma[static_cast<size_t>(l - 1)];
        const float src_size[2] = { static_cast<float>(src->params.w), static_cast<float>(src->params.h) };
        const float dst_size[2] = { static_cast<float>(lw[l]), static_cast<float>(lh[l]) };
        if (!run(l == 0 ? "flow luma" : "flow pyramid", kCommonHeader, l == 0 ? kDownColorBody : kDownLumaBody,
                 { sampledDesc("fgSrc", src), storageDesc("fgDst", pyr.luma[static_cast<size_t>(l)]) },
                 { vec2Var("fgSrcSize", src_size), vec2Var("fgDstSize", dst_size) },
                 lw[l], lh[l], 16))
            return false;
    }
    pyr.source = color;
    return true;
}

FlowInterpolator::FlowInterpolator(pl_log log, pl_gpu gpu)
    : impl(std::make_unique<Impl>())
{
    impl->log = log;
    impl->gpu = gpu;
}

FlowInterpolator::~FlowInterpolator()
{
    Impl &d = *impl;
    if (!d.gpu)
        return;
    d.destroyTextures();
    for (pl_timer &timer : d.timers)
        pl_timer_destroy(d.gpu, &timer);
    pl_dispatch_destroy(&d.dp);
}

bool FlowInterpolator::init()
{
    Impl &d = *impl;
    if (d.dp)
        return true;
    if (!d.gpu || !(d.gpu->glsl.compute))
        return false;
    d.fmt = pl_find_named_fmt(d.gpu, "rgba16f");
    const auto needed = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
    if (!d.fmt || (d.fmt->caps & needed) != needed) {
        qCWarning(chiakiGui) << "[framegen] Flow frame generation needs storable, filterable rgba16f textures";
        return false;
    }
    d.dp = pl_dispatch_create(d.log, d.gpu);
    if (!d.dp)
        return false;
    d.timers.assign(kMaxTimers, nullptr);
    for (pl_timer &timer : d.timers)
        timer = pl_timer_create(d.gpu);
    return true;
}

void FlowInterpolator::invalidate()
{
    impl->destroyTextures();
}

void FlowInterpolator::setAnalysisScale(int scale)
{
    scale = scale <= 2 ? 2 : 4;
    if (scale == impl->base_scale)
        return;
    impl->base_scale = scale;
    // The next call rebuilds every analysis texture at the new size.
    impl->destroyTextures();
}

int FlowInterpolator::analysisScale() const
{
    return impl->base_scale;
}

float FlowInterpolator::gpuTimeMs() const
{
    return impl->gpu_ms;
}

bool FlowInterpolator::interpolate(pl_tex prev, pl_tex cur, pl_tex const *outputs, const float *times, int count)
{
    Impl &d = *impl;
    if (!d.dp || !cur)
        return false;
    d.collectTimers();
    if (!d.ensureSize(cur->params.w, cur->params.h))
        return false;

    // Reuse the pyramid of `prev` from the last call when there is one.
    Impl::Pyramid *prev_pyr = nullptr;
    for (auto &pyr : d.pyramids) {
        if (prev && pyr.source == prev)
            prev_pyr = &pyr;
    }
    Impl::Pyramid &cur_pyr = prev_pyr == &d.pyramids[0] ? d.pyramids[1] : d.pyramids[0];
    if (prev && !prev_pyr) {
        Impl::Pyramid &other = &cur_pyr == &d.pyramids[0] ? d.pyramids[1] : d.pyramids[0];
        if (!d.buildPyramid(other, prev))
            return false;
        prev_pyr = &other;
        d.have_temporal = false;
    }
    if (!d.buildPyramid(cur_pyr, cur))
        return false;
    if (!prev) {
        d.have_temporal = false;
        return true;
    }

    // Motion search, coarse to fine.
    const float full[2] = { static_cast<float>(d.width), static_cast<float>(d.height) };
    for (int l = d.levels - 1; l >= 0; l--) {
        const size_t li = static_cast<size_t>(l);
        const bool coarsest = l == d.levels - 1;
        const float size[2] = { static_cast<float>(d.lw[l]), static_cast<float>(d.lh[l]) };
        const float coarse_size[2] = {
            coarsest ? size[0] : static_cast<float>(d.lw[l + 1]),
            coarsest ? size[1] : static_cast<float>(d.lh[l + 1]),
        };
        const float to_full = full[0] / size[0];
        // Motion beyond ~30% of the frame per stream frame is not tracked.
        const float max_flow = 0.3f * size[0];
        const float has_coarse = coarsest ? 0.0f : 1.0f;
        const float has_temporal = (l == 0 && d.have_temporal) ? 1.0f : 0.0f;
        const int radius = kCoarseRadius;
        // The matching patch keeps covering ~10 display pixels when the
        // finest level is at 1/2 resolution; a smaller patch follows noise.
        const float patch_step = (l == 0 && d.base_scale < kDefaultBaseScale) ? 2.0f : 1.0f;
        pl_tex coarse = coarsest ? d.dummy : d.flow[li + 1];
        pl_tex temporal = d.final_flow[static_cast<size_t>(d.final_index)];
        if (!d.run("flow search", kSearchHeader, kSearchBody,
                   { sampledDesc("fgPrevL", prev_pyr->luma[li]), sampledDesc("fgCurL", cur_pyr.luma[li]),
                     sampledDesc("fgCoarse", coarse), sampledDesc("fgTemporal", temporal),
                     storageDesc("fgDst", d.flow[li]) },
                   { vec2Var("fgSize", size), vec2Var("fgCoarseSize", coarse_size), floatVar("fgToFull", &to_full),
                     floatVar("fgMaxFlow", &max_flow), floatVar("fgHasCoarse", &has_coarse),
                     floatVar("fgHasTemporal", &has_temporal), intVar("fgRadius", &radius),
                     floatVar("fgPatchStep", &patch_step) },
                   d.lw[l], d.lh[l], 16))
            return false;
    }

    // Outlier clean-up into the next final flow buffer.
    const int next_final = 1 - d.final_index;
    {
        const float size[2] = { static_cast<float>(d.lw[0]), static_cast<float>(d.lh[0]) };
        const float to_full = full[0] / size[0];
        const float max_flow = 0.3f * size[0];
        const float patch_step = d.base_scale < kDefaultBaseScale ? 2.0f : 1.0f;
        if (!d.run("flow clean-up", kSearchHeader, kCleanBody,
                   { sampledDesc("fgPrevL", prev_pyr->luma[0]), sampledDesc("fgCurL", cur_pyr.luma[0]),
                     sampledDesc("fgCoarse", d.flow[0], false), storageDesc("fgDst", d.final_flow[static_cast<size_t>(next_final)]) },
                   { vec2Var("fgSize", size), floatVar("fgToFull", &to_full), floatVar("fgMaxFlow", &max_flow),
                     floatVar("fgPatchStep", &patch_step) },
                   d.lw[0], d.lh[0], 16))
            return false;
    }
    {
        const size_t last = static_cast<size_t>(d.levels - 1);
        const float size[2] = { static_cast<float>(d.lw[last]), static_cast<float>(d.lh[last]) };
        if (!d.run("flow scene check", nullptr, kStatsBody,
                   { sampledDesc("fgCoarse", d.flow[last], false), storageDesc("fgDst", d.stats) },
                   { vec2Var("fgSize", size) },
                   1, 1, 1))
            return false;
    }
    d.final_index = next_final;
    d.have_temporal = true;

    // Synthesis of each requested frame.
    pl_tex final_tex = d.final_flow[static_cast<size_t>(d.final_index)];
    const float flow_size[2] = { static_cast<float>(d.lw[0]), static_cast<float>(d.lh[0]) };
    const float reach = d.base_scale < kDefaultBaseScale ? 2.0f : 1.0f;
    const float reach_far = d.base_scale < kDefaultBaseScale ? 9.0f : 4.0f;
    for (int i = 0; i < count; i++) {
        const float t = times[i];
        if (!d.run("flow synthesis", kSynthHeader, kSynthBody,
                   { sampledDesc("fgPrevC", prev), sampledDesc("fgCurC", cur), sampledDesc("fgFlow", final_tex),
                     sampledDesc("fgStats", d.stats, false), storageDesc("fgOutImg", outputs[i]) },
                   { vec2Var("fgSize", full), vec2Var("fgFlowSize", flow_size), floatVar("fgT", &t),
                     floatVar("fgReach", &reach), floatVar("fgReachFar", &reach_far) },
                   d.width, d.height, 16))
            return false;
    }
    return true;
}
