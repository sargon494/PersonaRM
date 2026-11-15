#include "rendering/gpu_renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>
#endif

namespace personarm {
namespace {

// Vertex attr[3] flags (shared with the pixel shader).
constexpr uint32_t kTex = 1, kRaw = 2, kDither = 4, kPassOpaque = 8, kPassSemi = 16, kMaskSet = 32;
constexpr uint32_t kBlend = 1u << 14, kBlit = 1u << 15;

const char* kShaderVs = R"(
struct VIn { float2 pos : TEXCOORD0; float3 col : TEXCOORD1; float2 uv : TEXCOORD2; uint4 attr : TEXCOORD3; };
struct VOut { float4 pos : SV_Position; float3 col : TEXCOORD0; float2 uv : TEXCOORD1; nointerpolation uint4 attr : TEXCOORD2; };
VOut main(VIn i) { VOut o; o.pos = float4(i.pos, 0, 1); o.col = i.col; o.uv = i.uv; o.attr = i.attr; return o; }
)";

// Same math as Ps1Gpu::PlotPixel / FetchTexel; textures and CLUTs come from the 1x VRAM (R16_UNORM).
// The target holds 5-bit channels as c5/31 and the mask bit in alpha; fixed-function blending does
// the semi-transparency (mode 0 via blend constant 0.5, mode 2 via reverse subtract).
const char* kShaderPs = R"(
struct VOut { float4 pos : SV_Position; float3 col : TEXCOORD0; float2 uv : TEXCOORD1; nointerpolation uint4 attr : TEXCOORD2; };
Texture2D<float> vram : register(t0, space2);
SamplerState smp : register(s0, space2);
static const int kD[16] = {-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2};
uint rd(uint x, uint y) { return (uint)(vram.Load(int3(x & 1023u, y & 511u, 0)) * 65535.0 + 0.5); }
float4 main(VOut i) : SV_Target {
    uint tp = i.attr.x, clut = i.attr.y, tw = i.attr.z, fl = i.attr.w;
    uint s = (fl >> 8) & 15u;
    if (fl & 0x8000u) {  // blit: nearest upscale of the 1x VRAM
        uint2 p = uint2(i.pos.xy) / s;
        uint t = rd(p.x, p.y);
        return float4(float3(t & 31u, (t >> 5) & 31u, (t >> 10) & 31u) / 31.0, (float)(t >> 15));
    }
    int3 c8 = int3(floor(i.col + 0.5));
    uint stp = 0;
    bool blend = (fl & 0x4000u) != 0;
    if (fl & 1u) {
        uint u = (uint)((int)floor(i.uv.x)) & 255u, v = (uint)((int)floor(i.uv.y)) & 255u;
        uint mx = tw & 31u, my = (tw >> 5) & 31u, ox = (tw >> 10) & 31u, oy = (tw >> 15) & 31u;
        u = (u & ~(mx * 8u)) | ((ox & mx) * 8u);
        v = (v & ~(my * 8u)) | ((oy & my) * 8u);
        uint px = (tp & 15u) * 64u, py = ((tp >> 4) & 1u) * 256u, mode = (tp >> 7) & 3u;
        uint cx = (clut & 63u) * 16u, cy = (clut >> 6) & 511u;
        uint t;
        if (mode == 0u) { uint w = rd(px + u / 4u, py + v); t = rd(cx + ((w >> ((u & 3u) * 4u)) & 15u), cy); }
        else if (mode == 1u) { uint w = rd(px + u / 2u, py + v); t = rd(cx + ((w >> ((u & 1u) * 8u)) & 255u), cy); }
        else t = rd(px + u, py + v);
        if (t == 0u) discard;
        stp = t >> 15;
        if ((fl & 8u) && stp != 0u) discard;
        if ((fl & 16u) && stp == 0u) discard;
        blend = blend && stp != 0u;
        int3 t5 = int3(t & 31u, (t >> 5) & 31u, (t >> 10) & 31u);
        c8 = (fl & 2u) ? (t5 << 3) : ((t5 * c8) >> 4);
    }
    int d = 0;
    if (fl & 4u) { uint2 p = uint2(i.pos.xy) / s; d = kD[(p.y & 3u) * 4u + (p.x & 3u)]; }
    int3 c5 = clamp(c8 + d, 0, 255) >> 3;
    float3 f = float3(c5) / 31.0;
    if (blend) {
        uint m = (fl >> 12) & 3u;
        if (m == 0u) f = float3(c5) / 62.0;
        else if (m == 3u) f = float3(c5 >> 2) / 31.0;
    }
    return float4(f, (stp | ((fl >> 5) & 1u)) != 0u ? 1.0 : 0.0);
}
)";

// Display area of the scaled target -> RGBA8 with the 1x conversion (c5 << 3).
const char* kShaderDisplay = R"(
struct VOut { float4 pos : SV_Position; float3 col : TEXCOORD0; float2 uv : TEXCOORD1; nointerpolation uint4 attr : TEXCOORD2; };
Texture2D<float4> hi : register(t0, space2);
SamplerState smp : register(s0, space2);
float4 main(VOut i) : SV_Target {
    uint2 p = (uint2(i.pos.xy) + i.attr.xy) % i.attr.zw;
    float3 c = floor(hi.Load(int3(p, 0)).rgb * 31.0 + 0.5);
    return float4(c * 8.0 / 255.0, 1.0);
}
)";

SDL_GPUShader* CompileShader(SDL_GPUDevice* dev, const char* src, bool vertex, std::string* err) {
#ifdef _WIN32
    static pD3DCompile compile = [] {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        return m ? reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile"))) : nullptr;
    }();
    if (!compile) { *err = "d3dcompiler_47.dll not found"; return nullptr; }
    ID3DBlob *code = nullptr, *log = nullptr;
    if (FAILED(compile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", vertex ? "vs_5_1" : "ps_5_1",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &log))) {
        *err = log ? std::string(static_cast<const char*>(log->GetBufferPointer()), log->GetBufferSize()) : "D3DCompile failed";
        if (log) log->Release();
        return nullptr;
    }
    if (log) log->Release();
    SDL_GPUShaderCreateInfo info{};
    info.code_size = code->GetBufferSize();
    info.code = static_cast<const Uint8*>(code->GetBufferPointer());
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_DXBC;
    info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = vertex ? 0 : 1;
    SDL_GPUShader* sh = SDL_CreateGPUShader(dev, &info);
    code->Release();
    if (!sh) *err = SDL_GetError();
    return sh;
#else
    // shortcut: shaders are runtime-compiled HLSL (D3D12 only); Vulkan/Metal need SPIR-V/MSL builds (docs/GPU_RENDERER.md)
    (void)dev, (void)src, (void)vertex;
    *err = "no shader compiler on this platform";
    return nullptr;
#endif
}

int32_t SignExtend11(uint32_t v) { return static_cast<int32_t>((v & 0x7FF) << 21) >> 21; }
uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

} // namespace

bool GpuRenderer::Init(Ps1Gpu& gpu, uint32_t scale, std::string* err) {
    m_s = std::clamp<uint32_t>(scale, 1, 9);  // 9 x 240 = 2160 lines (4K)
    m_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXBC, false, nullptr);
    if (!m_dev) { *err = std::string("SDL_CreateGPUDevice: ") + SDL_GetError(); return false; }
    SDL_GPUShader* vs = CompileShader(m_dev, kShaderVs, true, err);
    SDL_GPUShader* ps = vs ? CompileShader(m_dev, kShaderPs, false, err) : nullptr;
    SDL_GPUShader* pd = ps ? CompileShader(m_dev, kShaderDisplay, false, err) : nullptr;
    if (!pd) {
        if (vs) SDL_ReleaseGPUShader(m_dev, vs);
        if (ps) SDL_ReleaseGPUShader(m_dev, ps);
        Shutdown();
        return false;
    }

    SDL_GPUVertexBufferDescription vb{0, sizeof(Vtx), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    const SDL_GPUVertexAttribute va[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vtx, x)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vtx, r)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vtx, u)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, offsetof(Vtx, attr)},
    };
    SDL_GPUColorTargetDescription ct{};
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs;
    pi.fragment_shader = ps;
    pi.vertex_input_state = {&vb, 1, va, 4};
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.target_info = {&ct, 1, SDL_GPU_TEXTUREFORMAT_INVALID, false};
    for (int i = 0; i < 5; ++i) {  // 0 opaque; 1-4: E1h semi modes B/2+F/2, B+F, B-F, B+F/4
        SDL_GPUColorTargetBlendState& b = ct.blend_state;
        b = {};
        b.enable_blend = i > 0;
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_color_blendfactor = i == 1 ? SDL_GPU_BLENDFACTOR_CONSTANT_COLOR : SDL_GPU_BLENDFACTOR_ONE;
        b.color_blend_op = i == 3 ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT : SDL_GPU_BLENDOP_ADD;
        b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;  // alpha = mask bit, written as is
        b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        m_pipe[i] = SDL_CreateGPUGraphicsPipeline(m_dev, &pi);
    }
    ct.blend_state = {};
    pi.fragment_shader = pd;
    m_display = SDL_CreateGPUGraphicsPipeline(m_dev, &pi);
    SDL_ReleaseGPUShader(m_dev, vs);
    SDL_ReleaseGPUShader(m_dev, ps);
    SDL_ReleaseGPUShader(m_dev, pd);

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R16_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = PS1_VRAM_WIDTH, ti.height = PS1_VRAM_HEIGHT, ti.layer_count_or_depth = 1, ti.num_levels = 1;
    m_vram1x = SDL_CreateGPUTexture(m_dev, &ti);
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.width = PS1_VRAM_WIDTH * m_s, ti.height = PS1_VRAM_HEIGHT * m_s;
    m_tmp = SDL_CreateGPUTexture(m_dev, &ti);
    ti.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    m_hi = SDL_CreateGPUTexture(m_dev, &ti);
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_sampler = SDL_CreateGPUSampler(m_dev, &si);
    bool ok = m_vram1x && m_tmp && m_hi && m_sampler && m_display;
    for (auto* p : m_pipe) ok = ok && p;
    if (!ok) { *err = std::string("SDL_gpu resources: ") + SDL_GetError(); Shutdown(); return false; }

    std::printf("[gpu] renderer: SDL_gpu %s, %ux (%ux%u target)\n", SDL_GetGPUDeviceDriver(m_dev), m_s,
                PS1_VRAM_WIDTH * m_s, PS1_VRAM_HEIGHT * m_s);
    m_gpu = &gpu;
    m_attached = &gpu;
    std::memset(m_dirty, 0, sizeof m_dirty);
    Upload(gpu, 0, 0, PS1_VRAM_WIDTH, PS1_VRAM_HEIGHT, true);  // start from the current VRAM
    gpu.SetDrawListener(this);
    return true;
}

void GpuRenderer::Shutdown() {
    if (m_attached) m_attached->SetDrawListener(nullptr), m_attached = nullptr;
    if (!m_dev) return;
    for (auto*& p : m_pipe) if (p) SDL_ReleaseGPUGraphicsPipeline(m_dev, p), p = nullptr;
    if (m_display) SDL_ReleaseGPUGraphicsPipeline(m_dev, m_display), m_display = nullptr;
    for (SDL_GPUTexture** t : {&m_vram1x, &m_hi, &m_tmp, &m_out})
        if (*t) SDL_ReleaseGPUTexture(m_dev, *t), *t = nullptr;
    if (m_sampler) SDL_ReleaseGPUSampler(m_dev, m_sampler), m_sampler = nullptr;
    if (m_vbuf) SDL_ReleaseGPUBuffer(m_dev, m_vbuf), m_vbuf = nullptr;
    if (m_up) SDL_ReleaseGPUTransferBuffer(m_dev, m_up), m_up = nullptr;
    if (m_down) SDL_ReleaseGPUTransferBuffer(m_dev, m_down), m_down = nullptr;
    SDL_DestroyGPUDevice(m_dev);
    m_dev = nullptr;
}

// ── Recording (called from inside the emulated GPU, before each packet executes) ─────────────────────

bool GpuRenderer::Clip(Rect& r) const {
    r.x0 = std::max({r.x0, m_area.x1, 0}), r.y0 = std::max({r.y0, m_area.y1, 0});
    r.x1 = std::min({r.x1, m_area.x2, int32_t(PS1_VRAM_WIDTH) - 1});
    r.y1 = std::min({r.y1, m_area.y2, int32_t(PS1_VRAM_HEIGHT) - 1});
    return r.x0 <= r.x1 && r.y0 <= r.y1;
}

void GpuRenderer::MarkDirty(const Rect& r) {
    for (int32_t by = r.y0 >> 5; by <= r.y1 >> 5; ++by)
        for (int32_t bx = r.x0 >> 5; bx <= r.x1 >> 5; ++bx) m_dirty[by & 15][bx & 31] = true;
}

// Brings the GPU's 1x texture up to date with the 1x VRAM over r (lazy: only blocks written since).
void GpuRenderer::SyncRegion(const Ps1Gpu& gpu, const Rect& r) {
    int32_t bx0 = r.x0 >> 5, bx1 = r.x1 >> 5;
    if (bx1 - bx0 >= 32) bx0 = 0, bx1 = 31;
    for (int32_t by = r.y0 >> 5; by <= r.y1 >> 5; ++by) {
        bool* row = m_dirty[by & 15];
        int32_t run = -1;
        auto flush = [&](int32_t end) {  // blocks [run, end) of this row, never across the VRAM edge
            Upload(gpu, uint32_t(run & 31) * 32, uint32_t(by & 15) * 32, uint32_t(end - run) * 32, 32, false);
            for (int32_t k = run; k < end; ++k) row[k & 31] = false;
            run = -1;
        };
        for (int32_t bx = bx0; bx <= bx1 + 1; ++bx) {
            const bool d = bx <= bx1 && row[bx & 31];
            if (d && run >= 0 && (bx & 31) == 0) flush(bx);
            if (d && run < 0) run = bx;
            if (!d && run >= 0) flush(bx);
        }
    }
}

void GpuRenderer::SyncTexture(const Ps1Gpu& gpu, uint32_t tp, uint32_t clut, uint32_t tw, int32_t u0, int32_t u1,
                              int32_t v0, int32_t v1) {
    const uint32_t mode = (tp >> 7) & 3, shift = mode == 0 ? 2 : mode == 1 ? 1 : 0;
    const int32_t px = int32_t(tp & 15) * 64, py = int32_t((tp >> 4) & 1) * 256;
    if ((tw & 0x1F) || u0 < 0 || u1 > 255) u0 = 0, u1 = 255;
    if ((tw & 0x3E0) || v0 < 0 || v1 > 255) v0 = 0, v1 = 255;
    SyncRegion(gpu, {px + (u0 >> shift), py + v0, px + (u1 >> shift), py + v1});
    if (mode < 2) {
        const int32_t cx = int32_t(clut & 63) * 16, cy = int32_t(clut >> 6) & 511;
        SyncRegion(gpu, {cx, cy, cx + (mode ? 255 : 15), cy});
    }
}

void GpuRenderer::Upload(const Ps1Gpu& gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool blit) {
    x &= PS1_VRAM_WIDTH - 1, y &= PS1_VRAM_HEIGHT - 1;
    w = std::min(w, PS1_VRAM_WIDTH), h = std::min(h, PS1_VRAM_HEIGHT);
    if (!w || !h) return;
    if (x + w > PS1_VRAM_WIDTH) {  // wrap: split at the VRAM edge
        Upload(gpu, x, y, PS1_VRAM_WIDTH - x, h, blit);
        Upload(gpu, 0, y, x + w - PS1_VRAM_WIDTH, h, blit);
        return;
    }
    if (y + h > PS1_VRAM_HEIGHT) {
        Upload(gpu, x, y, w, PS1_VRAM_HEIGHT - y, blit);
        Upload(gpu, x, 0, w, y + h - PS1_VRAM_HEIGHT, blit);
        return;
    }
    const uint32_t pitch = AlignUp(w, 128), off = AlignUp(uint32_t(m_staging.size()), 256);  // D3D12 copy alignment
    m_staging.resize(size_t(off) + size_t(pitch) * h);
    const uint16_t* src = gpu.GetVram().data();
    for (uint32_t r = 0; r < h; ++r)
        std::memcpy(&m_staging[off + size_t(r) * pitch], src + size_t(y + r) * PS1_VRAM_WIDTH + x, w * 2);
    m_ops.push_back({Op::Upload, 0, off, pitch, int32_t(x), int32_t(y), int32_t(w), int32_t(h), 0, 0});
    if (!blit) return;
    const float W = float(PS1_VRAM_WIDTH * m_s), H = float(PS1_VRAM_HEIGHT * m_s);
    const float X0 = float(x * m_s) / W * 2 - 1, X1 = float((x + w) * m_s) / W * 2 - 1;
    const float Y0 = 1 - float(y * m_s) / H * 2, Y1 = 1 - float((y + h) * m_s) / H * 2;
    const uint32_t a[4] = {0, 0, 0, kBlit | m_s << 8};
    const Vtx q[6] = {{X0, Y0, 0, 0, 0, 0, 0, {a[0], a[1], a[2], a[3]}}, {X1, Y0, 0, 0, 0, 0, 0, {a[0], a[1], a[2], a[3]}},
                      {X0, Y1, 0, 0, 0, 0, 0, {a[0], a[1], a[2], a[3]}}, {X1, Y0, 0, 0, 0, 0, 0, {a[0], a[1], a[2], a[3]}},
                      {X1, Y1, 0, 0, 0, 0, 0, {a[0], a[1], a[2], a[3]}}, {X0, Y1, 0, 0, 0, 0, 0, {a[0], a[1], a[2], a[3]}}};
    Emit(q, 6, 0, {int32_t(x), int32_t(y), int32_t(x + w - 1), int32_t(y + h - 1)});
}

void GpuRenderer::Emit(const Vtx* v, size_t n, uint8_t pipe, const Rect& clip) {
    const int32_t s = int32_t(m_s);
    const Op op{Op::Draw, pipe, uint32_t(m_vtx.size()), uint32_t(n), clip.x0 * s, clip.y0 * s,
                (clip.x1 - clip.x0 + 1) * s, (clip.y1 - clip.y0 + 1) * s, 0, 0};
    m_vtx.insert(m_vtx.end(), v, v + n);
    if (!m_ops.empty()) {
        Op& last = m_ops.back();
        if (last.kind == Op::Draw && last.pipe == pipe && last.x == op.x && last.y == op.y && last.w == op.w &&
            last.h == op.h && last.first + last.count == op.first) {
            last.count += uint32_t(n);
            return;
        }
    }
    m_ops.push_back(op);
}

void GpuRenderer::FlushResync() {
    if (!m_resync) return;
    m_resync = false;
    const Rect& r = m_resync_rect;
    Upload(*m_gpu, uint32_t(r.x0), uint32_t(r.y0), uint32_t(r.x1 - r.x0 + 1), uint32_t(r.y1 - r.y0 + 1), true);
}

// Unsupported primitive: the 1x rasterizer draws it as always; its rect is then nearest-upscaled from the
// 1x VRAM (the software path's result) before the next packet.
void GpuRenderer::Fallback(const char* what, const Rect& r) {
    const uint32_t bit = what[0] == 'l' ? 1 : 2;
    if (!(m_logged & bit)) std::printf("[gpu] fallback: %s drawn by the software rasterizer at 1x (nearest upscale)\n", what);
    m_logged |= bit;
    m_resync = true;
    m_resync_rect = r;
    MarkDirty(r);
}

void GpuRenderer::Prim(const Ps1Gpu& gpu, Vtx* v, size_t n, bool semi, bool textured, const Rect& bbox) {
    Rect r = bbox;
    if (!n || !Clip(r)) return;
    if (gpu.GetMaskCheck()) return Fallback("mask-checked primitives", r);
    if (textured) {
        float u0 = v[0].u, u1 = v[0].u, v0 = v[0].v, v1 = v[0].v;
        for (size_t i = 1; i < n; ++i)
            u0 = std::min(u0, v[i].u), u1 = std::max(u1, v[i].u), v0 = std::min(v0, v[i].v), v1 = std::max(v1, v[i].v);
        SyncTexture(gpu, v[0].attr[0], v[0].attr[1], v[0].attr[2], int32_t(std::floor(u0)), int32_t(std::floor(u1)),
                    int32_t(std::floor(v0)), int32_t(std::floor(v1)));
    }
    const uint32_t mode = (v[0].attr[0] >> 5) & 3;
    auto pass = [&](uint32_t flags, uint8_t pipe) {
        for (size_t i = 0; i < n; ++i) v[i].attr[3] |= flags;
        Emit(v, n, pipe, r);
        for (size_t i = 0; i < n; ++i) v[i].attr[3] &= ~flags;
    };
    if (!semi) pass(0, 0);
    else if (!textured) pass(kBlend | mode << 12, uint8_t(1 + mode));
    else {  // only texels with bit 15 set blend: opaque texels first, then the semi-transparent ones
        pass(kPassOpaque, 0);
        pass(kPassSemi | kBlend | mode << 12, uint8_t(1 + mode));
    }
    MarkDirty(r);
}

void GpuRenderer::Polygon(const Ps1Gpu& gpu, uint8_t cmd, const uint32_t* w) {
    const bool gouraud = cmd & 0x10, quad = cmd & 0x08, tex = cmd & 0x04, semi = cmd & 0x02, raw = tex && (cmd & 1);
    const int n = quad ? 4 : 3;
    struct P { int32_t x, y, r, g, b, u, v; } p[4];
    uint32_t tp = gpu.GetTexpage(), clut = 0, color = w[0];
    size_t idx = 1;
    for (int i = 0; i < n; ++i) {
        if (gouraud && i > 0) color = w[idx++];
        p[i].x = SignExtend11(w[idx]) + m_area.offset_x;
        p[i].y = SignExtend11(w[idx] >> 16) + m_area.offset_y;
        ++idx;
        p[i].r = color & 0xFF, p[i].g = (color >> 8) & 0xFF, p[i].b = (color >> 16) & 0xFF;
        p[i].u = p[i].v = 0;
        if (tex) {
            const uint32_t uvw = w[idx++];
            p[i].u = uvw & 0xFF, p[i].v = (uvw >> 8) & 0xFF;
            if (i == 0) clut = uvw >> 16;
            if (i == 1) tp = (tp & ~0x9FFu) | ((uvw >> 16) & 0x9FF);
        }
    }
    const bool dither = ((tp >> 9) & 1) && (gouraud || (tex && !raw));
    const uint32_t flags = (tex ? kTex : 0) | (raw ? kRaw : 0) | (dither ? kDither : 0) |
                           (gpu.GetMaskSet() ? kMaskSet : 0) | m_s << 8;
    const float W = float(PS1_VRAM_WIDTH * m_s), H = float(PS1_VRAM_HEIGHT * m_s), s = float(m_s);
    Vtx v[6];
    size_t nv = 0;
    Rect bb{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    const int tri[2][3] = {{0, 1, 2}, {1, 2, 3}};
    for (int t = 0; t < (quad ? 2 : 1); ++t) {
        const P &a = p[tri[t][0]], &b = p[tri[t][1]], &c = p[tri[t][2]];
        const int32_t x0 = std::min({a.x, b.x, c.x}), x1 = std::max({a.x, b.x, c.x});
        const int32_t y0 = std::min({a.y, b.y, c.y}), y1 = std::max({a.y, b.y, c.y});
        if (x1 - x0 > 1023 || y1 - y0 > 511) continue;  // psx-spx: oversized polygons are skipped
        bb = {std::min(bb.x0, x0), std::min(bb.y0, y0), std::max(bb.x1, x1), std::max(bb.y1, y1)};
        for (int k = 0; k < 3; ++k) {  // +0.5: the software samples at integer coords, the GPU at pixel centres
            const P& q = p[tri[t][k]];
            v[nv++] = {(float(q.x) * s + 0.5f) / W * 2 - 1, 1 - (float(q.y) * s + 0.5f) / H * 2, float(q.r), float(q.g),
                       float(q.b), float(q.u) + 0.5f, float(q.v) + 0.5f, {tp, clut, gpu.GetTexWindow(), flags}};
        }
    }
    Prim(gpu, v, nv, semi, tex, bb);
}

void GpuRenderer::Rectangle(const Ps1Gpu& gpu, uint8_t cmd, const uint32_t* w) {
    const bool tex = cmd & 0x04, semi = cmd & 0x02, raw = tex && (cmd & 1);
    const int32_t ox = SignExtend11(w[1]) + m_area.offset_x, oy = SignExtend11(w[1] >> 16) + m_area.offset_y;
    size_t idx = 2;
    int32_t u0 = 0, v0 = 0;
    uint32_t clut = 0;
    if (tex) {
        const uint32_t uvw = w[idx++];
        u0 = uvw & 0xFF, v0 = (uvw >> 8) & 0xFF, clut = uvw >> 16;
    }
    int32_t rw = 0, rh = 0;
    switch ((cmd >> 3) & 3) {
        case 0: rw = w[idx] & 0x3FF, rh = (w[idx] >> 16) & 0x1FF; break;
        case 1: rw = rh = 1; break;
        case 2: rw = rh = 8; break;
        case 3: rw = rh = 16; break;
    }
    if (!rw || !rh) return;
    const uint32_t tp = gpu.GetTexpage();
    const bool fx = (tp >> 12) & 1, fy = (tp >> 13) & 1;
    // Fine pixel dx samples u0 + (dx + 0.5)/s (flipped: u0 + 1 - (dx + 0.5)/s), as Ps1Gpu::DrawRectangle.
    const float uL = float(fx ? u0 + 1 : u0), uR = float(fx ? u0 + 1 - rw : u0 + rw);
    const float vT = float(fy ? v0 + 1 : v0), vB = float(fy ? v0 + 1 - rh : v0 + rh);
    const float W = float(PS1_VRAM_WIDTH * m_s), H = float(PS1_VRAM_HEIGHT * m_s), s = float(m_s);
    const float X0 = float(ox) * s / W * 2 - 1, X1 = float(ox + rw) * s / W * 2 - 1;
    const float Y0 = 1 - float(oy) * s / H * 2, Y1 = 1 - float(oy + rh) * s / H * 2;
    const float r = float(w[0] & 0xFF), g = float((w[0] >> 8) & 0xFF), b = float((w[0] >> 16) & 0xFF);
    const uint32_t a0 = tp, a1 = clut, a2 = gpu.GetTexWindow(),
                   a3 = (tex ? kTex : 0) | (raw ? kRaw : 0) | (gpu.GetMaskSet() ? kMaskSet : 0) | m_s << 8;
    Vtx v[6] = {{X0, Y0, r, g, b, uL, vT, {a0, a1, a2, a3}}, {X1, Y0, r, g, b, uR, vT, {a0, a1, a2, a3}},
                {X0, Y1, r, g, b, uL, vB, {a0, a1, a2, a3}}, {X1, Y0, r, g, b, uR, vT, {a0, a1, a2, a3}},
                {X1, Y1, r, g, b, uR, vB, {a0, a1, a2, a3}}, {X0, Y1, r, g, b, uL, vB, {a0, a1, a2, a3}}};
    Prim(gpu, v, 6, semi, tex, {ox, oy, ox + rw - 1, oy + rh - 1});
}

void GpuRenderer::OnGp0(const Ps1Gpu& gpu, const uint32_t* w, size_t n) {
    FlushResync();
    m_area = gpu.GetDrawingArea();
    const uint8_t cmd = uint8_t(w[0] >> 24);
    if (cmd >= 0x20 && cmd < 0x40) Polygon(gpu, cmd, w);
    else if (cmd >= 0x60 && cmd < 0x80) Rectangle(gpu, cmd, w);
    else if (cmd >= 0x40 && cmd < 0x60) {  // lines: polylines stream past this packet, so take the draw area
        Rect r{m_area.x1, m_area.y1, m_area.x2, m_area.y2};
        if (!(cmd & 0x08) && n >= 3) {
            const uint32_t a = w[1], b = (cmd & 0x10) ? w[3] : w[2];
            const int32_t ax = SignExtend11(a) + m_area.offset_x, ay = SignExtend11(a >> 16) + m_area.offset_y;
            const int32_t bx = SignExtend11(b) + m_area.offset_x, by = SignExtend11(b >> 16) + m_area.offset_y;
            r = {std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)};
        }
        if (Clip(r)) Fallback("lines", r);
    } else if (cmd >= 0x80 && cmd < 0xA0 && n >= 4) {  // VRAM->VRAM: copy the scaled pixels, as the scaled mirror
        const int32_t sx = w[1] & 0x3FF, sy = (w[1] >> 16) & 0x1FF, dx = w[2] & 0x3FF, dy = (w[2] >> 16) & 0x1FF;
        // shortcut: copies crossing the VRAM edge are clipped there (the CPU path wraps); none seen in P2
        const int32_t cw = std::min<int32_t>(w[3] & 0x3FF, 1024 - std::max(sx, dx));
        const int32_t ch = std::min<int32_t>((w[3] >> 16) & 0x1FF, 512 - std::max(sy, dy));
        if (cw <= 0 || ch <= 0) return;
        m_ops.push_back({Op::Copy, 0, 0, 0, dx, dy, cw, ch, sx, sy});
        MarkDirty({dx, dy, dx + cw - 1, dy + ch - 1});
    }
}

void GpuRenderer::OnVramWrite(const Ps1Gpu& gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    FlushResync();
    Upload(gpu, x, y, w, h, true);
}

// ── Submission ────────────────────────────────────────────────────────────────────────────────────

bool GpuRenderer::Present(const GpuDisplayArea& d, std::vector<uint32_t>* rgba) {
    if (!m_dev) return false;
    FlushResync();
    const uint32_t ow = (d.width ? d.width : 320) * m_s, oh = (d.height ? d.height : 240) * m_s;
    if (rgba && (!m_out || ow != m_out_w || oh != m_out_h)) {
        if (m_out) SDL_ReleaseGPUTexture(m_dev, m_out);
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ti.width = ow, ti.height = oh, ti.layer_count_or_depth = 1, ti.num_levels = 1;
        m_out = SDL_CreateGPUTexture(m_dev, &ti);
        m_out_w = ow, m_out_h = oh;
        if (!m_out) return false;
    }
    const uint32_t disp = uint32_t(m_vtx.size());
    if (rgba) {
        const uint32_t a[4] = {(d.x & 1023u) * m_s, (d.y & 511u) * m_s, PS1_VRAM_WIDTH * m_s, PS1_VRAM_HEIGHT * m_s};
        const float c[6][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1}, {-1, -1}};
        for (auto& p : c) m_vtx.push_back({p[0], p[1], 0, 0, 0, 0, 0, {a[0], a[1], a[2], a[3]}});
    }
    const uint32_t vbytes = uint32_t(m_vtx.size() * sizeof(Vtx)), soff = AlignUp(vbytes, 512);
    const uint32_t total = soff + uint32_t(m_staging.size() * 2), down = ow * oh * 4;
    auto grow_tb = [&](SDL_GPUTransferBuffer*& b, uint32_t& size, uint32_t need, SDL_GPUTransferBufferUsage u) {
        if (b && size >= need) return;
        if (b) SDL_ReleaseGPUTransferBuffer(m_dev, b);
        size = std::max(need, size * 2);
        const SDL_GPUTransferBufferCreateInfo ci{u, size, 0};
        b = SDL_CreateGPUTransferBuffer(m_dev, &ci);
    };
    grow_tb(m_up, m_up_size, total, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
    grow_tb(m_down, m_down_size, down, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
    if (!m_vbuf || m_vbuf_size < vbytes) {
        if (m_vbuf) SDL_ReleaseGPUBuffer(m_dev, m_vbuf);
        m_vbuf_size = std::max(vbytes, m_vbuf_size * 2);
        const SDL_GPUBufferCreateInfo ci{SDL_GPU_BUFFERUSAGE_VERTEX, m_vbuf_size, 0};
        m_vbuf = SDL_CreateGPUBuffer(m_dev, &ci);
    }
    if (!m_up || !m_down || !m_vbuf) return false;
    auto* map = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_dev, m_up, false));
    if (!map) return false;
    std::memcpy(map, m_vtx.data(), vbytes);
    if (!m_staging.empty()) std::memcpy(map + soff, m_staging.data(), m_staging.size() * 2);
    SDL_UnmapGPUTransferBuffer(m_dev, m_up);

    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(m_dev);
    if (!cb) return false;
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    const SDL_GPUTransferBufferLocation vsrc{m_up, 0};
    const SDL_GPUBufferRegion vdst{m_vbuf, 0, vbytes};
    SDL_UploadToGPUBuffer(cp, &vsrc, &vdst, false);
    SDL_GPURenderPass* rp = nullptr;
    int cur = -1;
    const SDL_GPUBufferBinding vbind{m_vbuf, 0};
    auto begin_render = [&](SDL_GPUTexture* target, SDL_GPUTexture* sampled, SDL_GPULoadOp load) {
        SDL_GPUColorTargetInfo ci{};
        ci.texture = target;
        ci.load_op = load;
        ci.store_op = SDL_GPU_STOREOP_STORE;
        rp = SDL_BeginGPURenderPass(cb, &ci, 1, nullptr);
        SDL_BindGPUVertexBuffers(rp, 0, &vbind, 1);
        const SDL_GPUTextureSamplerBinding sb{sampled, m_sampler};
        SDL_BindGPUFragmentSamplers(rp, 0, &sb, 1);
        SDL_SetGPUBlendConstants(rp, SDL_FColor{0.5f, 0.5f, 0.5f, 0.5f});
        cur = -1;
    };
    const uint32_t s = m_s;
    uint32_t draws = 0;
    for (const Op& op : m_ops) {
        if (op.kind == Op::Draw) {
            if (cp) SDL_EndGPUCopyPass(cp), cp = nullptr;
            if (!rp) begin_render(m_hi, m_vram1x, SDL_GPU_LOADOP_LOAD);
            if (cur != op.pipe) SDL_BindGPUGraphicsPipeline(rp, m_pipe[op.pipe]), cur = op.pipe;
            const SDL_Rect sc{op.x, op.y, op.w, op.h};
            SDL_SetGPUScissor(rp, &sc);
            SDL_DrawGPUPrimitives(rp, op.count, 1, op.first, 0);
            ++draws;
            continue;
        }
        if (rp) SDL_EndGPURenderPass(rp), rp = nullptr;
        if (!cp) cp = SDL_BeginGPUCopyPass(cb);
        if (op.kind == Op::Upload) {
            const SDL_GPUTextureTransferInfo src{m_up, soff + op.first * 2, op.count, uint32_t(op.h)};
            const SDL_GPUTextureRegion dst{m_vram1x, 0, 0, uint32_t(op.x), uint32_t(op.y), 0, uint32_t(op.w), uint32_t(op.h), 1};
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
        } else {  // via a temp texture: source and destination may overlap
            const SDL_GPUTextureLocation a{m_hi, 0, 0, op.sx * s, op.sy * s, 0}, t{m_tmp, 0, 0, op.sx * s, op.sy * s, 0},
                b{m_hi, 0, 0, op.x * s, op.y * s, 0};
            SDL_CopyGPUTextureToTexture(cp, &a, &t, op.w * s, op.h * s, 1, false);
            SDL_CopyGPUTextureToTexture(cp, &t, &b, op.w * s, op.h * s, 1, false);
        }
    }
    if (cp) SDL_EndGPUCopyPass(cp), cp = nullptr;
    if (rp) SDL_EndGPURenderPass(rp), rp = nullptr;
    if (!rgba) {  // flush only (24bpp display): no fence wait, the next submit orders after this one
        m_vtx.clear(), m_staging.clear(), m_ops.clear();
        m_last_draws = draws;
        return SDL_SubmitGPUCommandBuffer(cb);
    }
    begin_render(m_out, m_hi, SDL_GPU_LOADOP_DONT_CARE);
    SDL_BindGPUGraphicsPipeline(rp, m_display);
    SDL_DrawGPUPrimitives(rp, 6, 1, disp, 0);
    SDL_EndGPURenderPass(rp);
    cp = SDL_BeginGPUCopyPass(cb);
    const SDL_GPUTextureRegion oreg{m_out, 0, 0, 0, 0, 0, ow, oh, 1};
    const SDL_GPUTextureTransferInfo odst{m_down, 0, ow, oh};
    SDL_DownloadFromGPUTexture(cp, &oreg, &odst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    m_vtx.clear(), m_staging.clear(), m_ops.clear();
    m_last_draws = draws;
    if (!fence) return false;
    SDL_WaitForGPUFences(m_dev, true, &fence, 1);
    SDL_ReleaseGPUFence(m_dev, fence);
    const void* px = SDL_MapGPUTransferBuffer(m_dev, m_down, false);
    if (!px) return false;
    rgba->resize(size_t(ow) * oh);
    std::memcpy(rgba->data(), px, size_t(down));
    SDL_UnmapGPUTransferBuffer(m_dev, m_down);
    return true;
}

} // namespace personarm
