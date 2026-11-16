#include "rendering/gpu_renderer.h"
#include "rendering/vram_texture_hook.h"

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
constexpr uint32_t kTex = 1, kRaw = 2, kDither = 4, kPassOpaque = 8, kPassSemi = 16, kMaskSet = 32, kDstRead = 64;
constexpr uint32_t kBlend = 1u << 14, kBlit = 1u << 15;

const char* kShaderVs = R"(
struct VIn { float2 pos : TEXCOORD0; float3 col : TEXCOORD1; float2 uv : TEXCOORD2; uint4 attr : TEXCOORD3; uint2 hd : TEXCOORD4; };
struct VOut { float4 pos : SV_Position; float3 col : TEXCOORD0; float2 uv : TEXCOORD1; nointerpolation uint4 attr : TEXCOORD2;
              nointerpolation uint2 hd : TEXCOORD3; };
VOut main(VIn i) { VOut o; o.pos = float4(i.pos, 0, 1); o.col = i.col; o.uv = i.uv; o.attr = i.attr; o.hd = i.hd; return o; }
)";

// Same math as Ps1Gpu::PlotPixel / FetchTexel; textures and CLUTs come from the 1x VRAM (R16_UNORM).
// The target holds 5-bit channels as c5/31 and the mask bit in alpha; fixed-function blending does
// the semi-transparency (mode 0 via blend constant 0.5, mode 2 via reverse subtract) and the mask check
// (destination alpha). Mask-checked B/2+F/2 reads the destination from a copy (dst) instead: kDstRead.
const char* kShaderPs = R"(
struct VOut { float4 pos : SV_Position; float3 col : TEXCOORD0; float2 uv : TEXCOORD1; nointerpolation uint4 attr : TEXCOORD2;
              nointerpolation uint2 hd : TEXCOORD3; };
Texture2D<float> vram : register(t0, space2);
Texture2D<float4> dst : register(t1, space2);
Texture2D<float4> hd : register(t2, space2);
SamplerState smp : register(s0, space2);
SamplerState smp1 : register(s1, space2);
SamplerState smp2 : register(s2, space2);
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
        uint t, ppw = mode == 0u ? 4u : mode == 1u ? 2u : 1u, wx = px + u / ppw, wy = py + v;
        if (mode == 0u) { uint w = rd(wx, wy); t = rd(cx + ((w >> ((u & 3u) * 4u)) & 15u), cy); }
        else if (mode == 1u) { uint w = rd(wx, wy); t = rd(cx + ((w >> ((u & 1u) * 8u)) & 255u), cy); }
        else t = rd(wx, wy);
        if (i.hd.x >> 31) {  // HD replacement, as VramTextureHook::Texel (sub-texel position from the UV)
            uint tx = ((wx - (i.hd.x & 1023u)) & 1023u) * ppw + u % ppw, ty = (wy - ((i.hd.x >> 10) & 511u)) & 511u;
            uint W = (i.hd.y & 0xFFFFu) * ppw, H = i.hd.y >> 16, hw, hh;
            if (tx < W && ty < H) {
                hd.GetDimensions(hw, hh);
                uint hx = min(hw - 1u, (uint)((float(tx) + frac(i.uv.x)) * hw / W));
                uint hy = min(hh - 1u, (uint)((float(ty) + frac(i.uv.y)) * hh / H));
                float4 c = hd.Load(int3(hx, hy, 0));
                if (c.a < 0.5) discard;
                uint3 c5 = uint3(c.rgb * 255.0 + 0.5) >> 3;
                uint n = c5.r | (c5.g << 5) | (c5.b << 10) | (t & 0x8000u);
                t = n != 0u ? n : 0x0421u;
            }
        }
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
        if (fl & 64u) f = float3((int3(floor(dst.Load(int3(i.pos.xy, 0)).rgb * 31.0 + 0.5)) + c5) >> 1) / 31.0;
        else if (m == 0u) f = float3(c5) / 62.0;
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

SDL_GPUShader* CompileShader(SDL_GPUDevice* dev, const char* src, bool vertex, uint32_t samplers, std::string* err) {
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
    info.num_samplers = samplers;
    SDL_GPUShader* sh = SDL_CreateGPUShader(dev, &info);
    code->Release();
    if (!sh) *err = SDL_GetError();
    return sh;
#else
    // shortcut: shaders are runtime-compiled HLSL (D3D12 only); Vulkan/Metal need SPIR-V/MSL builds (docs/GPU_RENDERER.md)
    (void)dev, (void)src, (void)vertex, (void)samplers;
    *err = "no shader compiler on this platform";
    return nullptr;
#endif
}

int32_t SignExtend11(uint32_t v) { return static_cast<int32_t>((v & 0x7FF) << 21) >> 21; }
uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

SDL_GPUTexture* MakeTexture(SDL_GPUDevice* dev, SDL_GPUTextureFormat f, uint32_t usage, uint32_t w, uint32_t h) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = f;
    ti.usage = usage;
    ti.width = w, ti.height = h, ti.layer_count_or_depth = 1, ti.num_levels = 1;
    return SDL_CreateGPUTexture(dev, &ti);
}

} // namespace

bool GpuRenderer::Init(Ps1Gpu& gpu, uint32_t scale, std::string* err, SDL_GPUDevice* device) {
    m_s = std::clamp<uint32_t>(scale, 1, 9);  // 9 x 240 = 2160 lines (4K)
    m_shared = device && (SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_DXBC);
    m_dev = m_shared ? device : SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXBC, false, nullptr);
    if (!m_dev) { *err = std::string("SDL_CreateGPUDevice: ") + SDL_GetError(); return false; }
    SDL_GPUShader* vs = CompileShader(m_dev, kShaderVs, true, 0, err);
    SDL_GPUShader* ps = vs ? CompileShader(m_dev, kShaderPs, false, 3, err) : nullptr;
    SDL_GPUShader* pd = ps ? CompileShader(m_dev, kShaderDisplay, false, 1, err) : nullptr;
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
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT2, offsetof(Vtx, hd)},
    };
    SDL_GPUColorTargetDescription ct{};
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs;
    pi.fragment_shader = ps;
    pi.vertex_input_state = {&vb, 1, va, 5};
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.target_info = {&ct, 1, SDL_GPU_TEXTUREFORMAT_INVALID, false};
    // 0 opaque; 1-4: E1h semi modes B/2+F/2, B+F, B-F, B+F/4. 5-9: the same with the mask check (E6h.1): the
    // destination alpha (its mask bit) keeps the destination, src * (1 - dst_alpha) (+ dst * dst_alpha).
    // Mask-checked B/2+F/2 (6) cannot be expressed so; it draws with 5 and kDstRead.
    for (int i = 0; i < 10; ++i) {
        const int k = i % 5;
        const bool masked = i >= 5;
        SDL_GPUColorTargetBlendState& b = ct.blend_state;
        b = {};
        b.enable_blend = i > 0;
        b.src_color_blendfactor = masked ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
        b.dst_color_blendfactor = k == 0 ? (masked ? SDL_GPU_BLENDFACTOR_DST_ALPHA : SDL_GPU_BLENDFACTOR_ZERO)
                                  : k == 1 && !masked ? SDL_GPU_BLENDFACTOR_CONSTANT_COLOR : SDL_GPU_BLENDFACTOR_ONE;
        b.color_blend_op = k == 3 ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT : SDL_GPU_BLENDOP_ADD;
        // alpha = mask bit, written as is (masked: kept where already set)
        b.src_alpha_blendfactor = masked ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
        b.dst_alpha_blendfactor = masked ? SDL_GPU_BLENDFACTOR_DST_ALPHA : SDL_GPU_BLENDFACTOR_ZERO;
        b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        m_pipe[i] = SDL_CreateGPUGraphicsPipeline(m_dev, &pi);
    }
    ct.blend_state = {};
    pi.fragment_shader = pd;
    m_display = SDL_CreateGPUGraphicsPipeline(m_dev, &pi);
    SDL_ReleaseGPUShader(m_dev, vs);
    SDL_ReleaseGPUShader(m_dev, ps);
    SDL_ReleaseGPUShader(m_dev, pd);

    const uint32_t W = PS1_VRAM_WIDTH * m_s, H = PS1_VRAM_HEIGHT * m_s;
    m_vram1x[0] = MakeTexture(m_dev, SDL_GPU_TEXTUREFORMAT_R16_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, PS1_VRAM_WIDTH, PS1_VRAM_HEIGHT);
    m_tex[kTmp] = MakeTexture(m_dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, W, H);
    m_tex[kHi] = MakeTexture(m_dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                             SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, W, H);
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_sampler = SDL_CreateGPUSampler(m_dev, &si);
    bool ok = m_vram1x[0] && m_tex[kTmp] && m_tex[kHi] && m_sampler && m_display;
    for (auto* p : m_pipe) ok = ok && p;
    if (!ok) { *err = std::string("SDL_gpu resources: ") + SDL_GetError(); Shutdown(); return false; }

    std::printf("[gpu] renderer: SDL_gpu %s, %ux (%ux%u target), %s present\n", SDL_GetGPUDeviceDriver(m_dev), m_s, W, H,
                m_shared ? "direct" : "readback");
    m_attached = &gpu;
    std::memset(m_dirty, 0, sizeof m_dirty);
    m_set = 0;
    m_pending.on = false;
    Upload(gpu, 0, 0, PS1_VRAM_WIDTH, PS1_VRAM_HEIGHT, true);  // start from the current VRAM
    gpu.SetDrawListener(this);
    return true;
}

bool GpuRenderer::EnableInterp(std::string* err) {
    if (!m_dev) return false;
    const uint32_t W = PS1_VRAM_WIDTH * m_s, H = PS1_VRAM_HEIGHT * m_s;
    const uint32_t rt = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    for (Tex t : {kHi2, kSnap0, kSnap1})
        if (!m_tex[t]) m_tex[t] = MakeTexture(m_dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, rt, W, H);
    if (!m_vram1x[1])
        m_vram1x[1] = MakeTexture(m_dev, SDL_GPU_TEXTUREFORMAT_R16_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, PS1_VRAM_WIDTH, PS1_VRAM_HEIGHT);
    if (m_tex[kHi2] && m_tex[kSnap0] && m_tex[kSnap1] && m_vram1x[1]) return true;
    *err = std::string("SDL_gpu interpolation targets: ") + SDL_GetError();
    return false;
}

void GpuRenderer::Shutdown() {
    if (m_attached) m_attached->SetDrawListener(nullptr), m_attached = nullptr;
    if (!m_dev) return;
    if (m_out_tex) SDL_DestroyTexture(m_out_tex), m_out_tex = nullptr;
    for (auto*& p : m_pipe) if (p) SDL_ReleaseGPUGraphicsPipeline(m_dev, p), p = nullptr;
    if (m_display) SDL_ReleaseGPUGraphicsPipeline(m_dev, m_display), m_display = nullptr;
    for (SDL_GPUTexture*& t : m_tex) if (t) SDL_ReleaseGPUTexture(m_dev, t), t = nullptr;
    for (SDL_GPUTexture** t : {&m_vram1x[0], &m_vram1x[1], &m_out})
        if (*t) SDL_ReleaseGPUTexture(m_dev, *t), *t = nullptr;
    for (SDL_GPUTexture* t : m_hd_tex) SDL_ReleaseGPUTexture(m_dev, t);
    m_hd_tex.clear(), m_hd_src.clear();
    if (m_sampler) SDL_ReleaseGPUSampler(m_dev, m_sampler), m_sampler = nullptr;
    if (m_vbuf) SDL_ReleaseGPUBuffer(m_dev, m_vbuf), m_vbuf = nullptr;
    if (m_up) SDL_ReleaseGPUTransferBuffer(m_dev, m_up), m_up = nullptr;
    if (m_down) SDL_ReleaseGPUTransferBuffer(m_dev, m_down), m_down = nullptr;
    if (!m_shared) SDL_DestroyGPUDevice(m_dev);
    m_dev = nullptr, m_shared = false;
    m_vbuf_size = m_up_size = m_down_size = 0;
    m_vtx.clear(), m_staging.clear(), m_ops.clear();
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
        for (int32_t bx = r.x0 >> 5; bx <= r.x1 >> 5; ++bx) m_dirty[m_set][by & 15][bx & 31] = true;
}

// Brings the GPU's 1x texture up to date with the 1x VRAM over r (lazy: only blocks written since).
void GpuRenderer::SyncRegion(const Ps1Gpu& gpu, const Rect& r) {
    int32_t bx0 = r.x0 >> 5, bx1 = r.x1 >> 5;
    if (bx1 - bx0 >= 32) bx0 = 0, bx1 = 31;
    for (int32_t by = r.y0 >> 5; by <= r.y1 >> 5; ++by) {
        bool* row = m_dirty[m_set][by & 15];
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
    m_ops.push_back({Op::Upload, 0, m_set, off, pitch, int32_t(x), int32_t(y), int32_t(w), int32_t(h), 0, 0});
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
    const Op op{Op::Draw, pipe, m_set, uint32_t(m_vtx.size()), uint32_t(n), clip.x0 * s, clip.y0 * s,
                (clip.x1 - clip.x0 + 1) * s, (clip.y1 - clip.y0 + 1) * s, 0, 0, m_draw_hd};
    m_vtx.insert(m_vtx.end(), v, v + n);
    if (!m_ops.empty()) {
        Op& last = m_ops.back();
        if (last.kind == Op::Draw && last.pipe == pipe && last.set == m_set && last.x == op.x && last.y == op.y &&
            last.w == op.w && last.h == op.h && last.hd == op.hd && last.first + last.count == op.first) {
            last.count += uint32_t(n);
            return;
        }
    }
    m_ops.push_back(op);
}

void GpuRenderer::Draw(Vtx* v, size_t n, bool semi, bool textured, bool mask_check, Rect r) {
    if (!n || !Clip(r)) return;
    const uint32_t mode = (v[0].attr[0] >> 5) & 3;
    const uint8_t base = mask_check ? 5 : 0;
    auto pass = [&](uint32_t flags, uint8_t pipe) {
        for (size_t i = 0; i < n; ++i) v[i].attr[3] |= flags;
        Emit(v, n, pipe, r);
        for (size_t i = 0; i < n; ++i) v[i].attr[3] &= ~flags;
    };
    auto blend = [&](uint32_t flags) {
        if (!(mask_check && mode == 0)) return pass(flags | kBlend | mode << 12, uint8_t(base + 1 + mode));
        // B/2+F/2 with the mask check: the shader blends with a copy of the destination (exact, as PlotPixel)
        m_ops.push_back({Op::Snap, 0, m_set, 0, 0, r.x0, r.y0, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1, 0, 0});
        pass(flags | kBlend | kDstRead, base);
    };
    if (!semi) pass(0, base);
    else if (!textured) blend(0);
    else {  // only texels with bit 15 set blend: opaque texels first, then the semi-transparent ones
        pass(kPassOpaque, base);
        blend(kPassSemi);
    }
    MarkDirty(r);
}

void GpuRenderer::Polygon(const Ps1Gpu& gpu, uint8_t cmd, const uint32_t* w) {
    const bool gouraud = cmd & 0x10, quad = cmd & 0x08, tex = cmd & 0x04, raw = tex && (cmd & 1);
    Pending& p = m_pending;
    p = {};
    p.on = true, p.semi = cmd & 0x02, p.tex = tex, p.mask_check = gpu.GetMaskCheck(), p.n = quad ? 4 : 3;
    uint32_t tp = gpu.GetTexpage(), clut = 0, color = w[0];
    int32_t u0 = 255, u1 = 0, v0 = 255, v1 = 0;
    size_t idx = 1;
    for (int i = 0; i < p.n; ++i) {
        if (gouraud && i > 0) color = w[idx++];
        p.x[i] = SignExtend11(w[idx]) + m_area.offset_x;
        p.y[i] = SignExtend11(w[idx] >> 16) + m_area.offset_y;
        ++idx;
        p.r[i] = float(color & 0xFF), p.g[i] = float((color >> 8) & 0xFF), p.b[i] = float((color >> 16) & 0xFF);
        p.u[i] = p.v[i] = 0;
        if (tex) {
            const uint32_t uvw = w[idx++];
            const int32_t u = uvw & 0xFF, v = (uvw >> 8) & 0xFF;
            u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
            p.u[i] = float(u) + 0.5f, p.v[i] = float(v) + 0.5f;  // the software's half-texel shift
            if (i == 0) clut = uvw >> 16;
            if (i == 1) tp = (tp & ~0x9FFu) | ((uvw >> 16) & 0x9FF);
        }
    }
    p.hd_tex = -1;
    if (tex && m_hook) LookupHd(tp, gpu.GetTexWindow(), u0, u1, v0, v1);
    const bool dither = ((tp >> 9) & 1) && (gouraud || (tex && !raw));
    p.attr[0] = tp, p.attr[1] = clut, p.attr[2] = gpu.GetTexWindow();
    p.attr[3] = (tex ? kTex : 0) | (raw ? kRaw : 0) | (dither ? kDither : 0) | (gpu.GetMaskSet() ? kMaskSet : 0) | m_s << 8;
    if (tex) SyncTexture(gpu, tp, clut, p.attr[2], u0, u1, v0, v1);  // now: the packet may draw into its texture
}

void GpuRenderer::Rectangle(const Ps1Gpu& gpu, uint8_t cmd, const uint32_t* w) {
    const bool tex = cmd & 0x04, raw = tex && (cmd & 1);
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
    Pending& p = m_pending;
    p = {};
    p.on = p.rect = true, p.semi = cmd & 0x02, p.tex = tex, p.mask_check = gpu.GetMaskCheck(), p.n = 2;
    p.x[0] = ox, p.x[1] = ox + rw, p.y[0] = oy, p.y[1] = oy + rh;
    // Fine pixel dx samples u0 + (dx + 0.5)/s (flipped: u0 + 1 - (dx + 0.5)/s), as Ps1Gpu::DrawRectangle.
    p.u[0] = float(fx ? u0 + 1 : u0), p.u[1] = float(fx ? u0 + 1 - rw : u0 + rw);
    p.v[0] = float(fy ? v0 + 1 : v0), p.v[1] = float(fy ? v0 + 1 - rh : v0 + rh);
    p.r[0] = float(w[0] & 0xFF), p.g[0] = float((w[0] >> 8) & 0xFF), p.b[0] = float((w[0] >> 16) & 0xFF);
    p.attr[0] = tp, p.attr[1] = clut, p.attr[2] = gpu.GetTexWindow();
    p.attr[3] = (tex ? kTex : 0) | (raw ? kRaw : 0) | (gpu.GetMaskSet() ? kMaskSet : 0) | m_s << 8;
    p.hd_tex = -1;
    if (tex && m_hook)  // rectangle UV box: u0 .. u0 + w - 1 (flipped: u0 - w + 1 .. u0)
        LookupHd(tp, p.attr[2], fx ? u0 - rw + 1 : u0, fx ? u0 : u0 + rw - 1, fy ? v0 - rh + 1 : v0, fy ? v0 : v0 + rh - 1);
    if (tex)
        SyncTexture(gpu, tp, clut, p.attr[2], int32_t(std::min(p.u[0], p.u[1])), int32_t(std::max(p.u[0], p.u[1])),
                    int32_t(std::min(p.v[0], p.v[1])), int32_t(std::max(p.v[0], p.v[1])));
}

void GpuRenderer::OnSqueeze(const int32_t* x, int n) {
    if (!m_pending.on || n != m_pending.n) return;
    std::copy(x, x + n, m_pending.x);
}

void GpuRenderer::FlushPending() {
    if (!m_pending.on) return;
    Pending& p = m_pending;
    p.on = false;
    const float W = float(PS1_VRAM_WIDTH * m_s), H = float(PS1_VRAM_HEIGHT * m_s), s = float(m_s);
    const uint32_t* a = p.attr;
    struct HdScope {  // the primitive's HD texture while it is emitted
        int32_t& cur;
        HdScope(int32_t& c, int32_t v) : cur(c) { cur = v; }
        ~HdScope() { cur = -1; }
    } hd_scope(m_draw_hd, p.hd_tex);
    auto with_hd = [&](Vtx* v, size_t n) { for (size_t i = 0; i < n; ++i) v[i].hd[0] = p.hd[0], v[i].hd[1] = p.hd[1]; };
    if (p.rect) {
        const float X0 = float(p.x[0]) * s / W * 2 - 1, X1 = float(p.x[1]) * s / W * 2 - 1;
        const float Y0 = 1 - float(p.y[0]) * s / H * 2, Y1 = 1 - float(p.y[1]) * s / H * 2;
        const float r = p.r[0], g = p.g[0], b = p.b[0], uL = p.u[0], uR = p.u[1], vT = p.v[0], vB = p.v[1];
        Vtx v[6] = {{X0, Y0, r, g, b, uL, vT, {a[0], a[1], a[2], a[3]}}, {X1, Y0, r, g, b, uR, vT, {a[0], a[1], a[2], a[3]}},
                    {X0, Y1, r, g, b, uL, vB, {a[0], a[1], a[2], a[3]}}, {X1, Y0, r, g, b, uR, vT, {a[0], a[1], a[2], a[3]}},
                    {X1, Y1, r, g, b, uR, vB, {a[0], a[1], a[2], a[3]}}, {X0, Y1, r, g, b, uL, vB, {a[0], a[1], a[2], a[3]}}};
        with_hd(v, 6);
        return Draw(v, 6, p.semi, p.tex, p.mask_check, {p.x[0], p.y[0], p.x[1] - 1, p.y[1] - 1});
    }
    Vtx v[6];
    size_t nv = 0;
    Rect bb{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    const int tri[2][3] = {{0, 1, 2}, {1, 2, 3}};
    for (int t = 0; t < (p.n == 4 ? 2 : 1); ++t) {
        const int *k = tri[t];
        const int32_t x0 = std::min({p.x[k[0]], p.x[k[1]], p.x[k[2]]}), x1 = std::max({p.x[k[0]], p.x[k[1]], p.x[k[2]]});
        const int32_t y0 = std::min({p.y[k[0]], p.y[k[1]], p.y[k[2]]}), y1 = std::max({p.y[k[0]], p.y[k[1]], p.y[k[2]]});
        if (x1 - x0 > 1023 || y1 - y0 > 511) continue;  // psx-spx: oversized polygons are skipped
        bb = {std::min(bb.x0, x0), std::min(bb.y0, y0), std::max(bb.x1, x1), std::max(bb.y1, y1)};
        for (int j = 0; j < 3; ++j) {  // +0.5: the software samples at integer coords, the GPU at pixel centres
            const int q = k[j];
            v[nv++] = {(float(p.x[q]) * s + 0.5f) / W * 2 - 1, 1 - (float(p.y[q]) * s + 0.5f) / H * 2, p.r[q], p.g[q], p.b[q],
                       p.u[q], p.v[q], {a[0], a[1], a[2], a[3]}};
        }
    }
    with_hd(v, nv);
    Draw(v, nv, p.semi, p.tex, p.mask_check, bb);
}

// The HD replacement of the primitive's texture: the upload under the centre of its UV box, else under a
// corner (one per primitive; the CPU path decides per texel). shortcut: a primitive spanning two replaced
// uploads shows only that one's texels replaced; split it per upload if a mod needs it.
void GpuRenderer::LookupHd(uint32_t tp, uint32_t tw, int32_t u0, int32_t u1, int32_t v0, int32_t v1) {
    const uint32_t mode = (tp >> 7) & 3, ppw = mode == 0 ? 4 : mode == 1 ? 2 : 1;
    const uint32_t mx = tw & 31, my = (tw >> 5) & 31, ox = (tw >> 10) & 31, oy = (tw >> 15) & 31;
    uint32_t r[4];
    const HdTexture* hd = nullptr;
    const int32_t probe[5][2] = {{(u0 + u1) / 2, (v0 + v1) / 2}, {u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}};
    for (int k = 0; k < 5 && !hd; ++k) {
        const uint32_t tu = (uint32_t(probe[k][0]) & 255 & ~(mx * 8)) | ((ox & mx) * 8);
        const uint32_t tv = (uint32_t(probe[k][1]) & 255 & ~(my * 8)) | ((oy & my) * 8);
        hd = m_hook->Replacement((tp & 15) * 64 + tu / ppw, ((tp >> 4) & 1) * 256 + tv, r);
    }
    if (!hd || !hd->width || !hd->height) return;
    int32_t idx = int32_t(std::find(m_hd_src.begin(), m_hd_src.end(), hd) - m_hd_src.begin());
    if (idx == int32_t(m_hd_src.size())) {  // first use: create and upload it (kept for the session)
        SDL_GPUTexture* t = MakeTexture(m_dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, hd->width, hd->height);
        if (!t) return;
        m_hd_src.push_back(hd), m_hd_tex.push_back(t);
        const uint32_t pitch = AlignUp(hd->width, 64), off = AlignUp(uint32_t(m_staging.size()), 256);  // pixels, u16
        m_staging.resize(size_t(off) + size_t(pitch) * hd->height * 2);
        for (uint32_t y = 0; y < hd->height; ++y)
            std::memcpy(&m_staging[off + size_t(y) * pitch * 2], &hd->rgba[size_t(y) * hd->width * 4], size_t(hd->width) * 4);
        m_ops.push_back({Op::Upload, 0, 0, off, pitch, 0, 0, int32_t(hd->width), int32_t(hd->height), 0, 0, idx});
    }
    m_pending.hd[0] = (r[0] & 1023) | (r[1] & 511) << 10 | 1u << 31;
    m_pending.hd[1] = (r[2] & 0xFFFF) | r[3] << 16;
    m_pending.hd_tex = idx;
}

// A line as a parallelogram one 1x pixel thick across its minor axis, from the first pixel's outer edge to
// the last one's along the major axis: at s x it covers the pixels the 1x DDA (Ps1Gpu::DrawLineOn) plots,
// with fine edges instead of s x s blocks.
void GpuRenderer::OnLine(const Ps1Gpu& gpu, int32_t ax, int32_t ay, uint32_t ac, int32_t bx, int32_t by, uint32_t bc,
                         bool gouraud, bool semi) {
    FlushPending();
    m_area = gpu.GetDrawingArea();
    if (!gouraud) bc = ac;
    const bool xmajor = std::abs(bx - ax) >= std::abs(by - ay);
    if (xmajor ? bx < ax : by < ay) std::swap(ax, bx), std::swap(ay, by), std::swap(ac, bc);
    const float W = float(PS1_VRAM_WIDTH * m_s), H = float(PS1_VRAM_HEIGHT * m_s), s = float(m_s);
    // Through the end pixels' centres (target pixels), extended along the line by half a 1x pixel on the
    // major axis at both ends.
    const int32_t major = xmajor ? bx - ax : by - ay;
    const float ex = xmajor ? 0.5f : major ? 0.5f * float(bx - ax) / float(major) : 0;
    const float ey = !xmajor ? 0.5f : major ? 0.5f * float(by - ay) / float(major) : 0;
    const float cax = (float(ax) + 0.5f - ex) * s, cay = (float(ay) + 0.5f - ey) * s;
    const float cbx = (float(bx) + 0.5f + ex) * s, cby = (float(by) + 0.5f + ey) * s;
    const float ox = xmajor ? 0 : s / 2, oy = xmajor ? s / 2 : 0;  // half thickness across the minor axis
    const uint32_t tp = gpu.GetTexpage();
    const uint32_t a[4] = {tp, 0, 0, ((((tp >> 9) & 1) && gouraud) ? kDither : 0) | (gpu.GetMaskSet() ? kMaskSet : 0) | m_s << 8};
    auto vtx = [&](float x, float y, uint32_t c) {
        return Vtx{x / W * 2 - 1, 1 - y / H * 2, float(c & 0xFF), float((c >> 8) & 0xFF), float((c >> 16) & 0xFF), 0, 0,
                   {a[0], a[1], a[2], a[3]}};
    };
    Vtx v[6] = {vtx(cax - ox, cay - oy, ac), vtx(cbx - ox, cby - oy, bc), vtx(cax + ox, cay + oy, ac),
                vtx(cbx - ox, cby - oy, bc), vtx(cbx + ox, cby + oy, bc), vtx(cax + ox, cay + oy, ac)};
    Draw(v, 6, semi, false, gpu.GetMaskCheck(), {std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)});
}

void GpuRenderer::OnGp0(const Ps1Gpu& gpu, const uint32_t* w, size_t n) {
    FlushPending();
    m_area = gpu.GetDrawingArea();
    const uint8_t cmd = uint8_t(w[0] >> 24);
    if (cmd >= 0x20 && cmd < 0x40) Polygon(gpu, cmd, w);
    else if (cmd >= 0x60 && cmd < 0x80) Rectangle(gpu, cmd, w);
    else if (cmd >= 0x80 && cmd < 0xA0 && n >= 4) {  // VRAM->VRAM: copy the scaled pixels, as the scaled mirror
        const int32_t sx = w[1] & 0x3FF, sy = (w[1] >> 16) & 0x1FF, dx = w[2] & 0x3FF, dy = (w[2] >> 16) & 0x1FF;
        // shortcut: copies crossing the VRAM edge are clipped there (the CPU path wraps); none seen in P2
        const int32_t cw = std::min<int32_t>(w[3] & 0x3FF, 1024 - std::max(sx, dx));
        const int32_t ch = std::min<int32_t>((w[3] >> 16) & 0x1FF, 512 - std::max(sy, dy));
        if (cw <= 0 || ch <= 0) return;
        m_ops.push_back({Op::Copy, 0, m_set, 0, 0, dx, dy, cw, ch, sx, sy});
        MarkDirty({dx, dy, dx + cw - 1, dy + ch - 1});
    }
    // Lines (40h-5Fh) arrive through OnLine, segment by segment, as the rasterizer draws them.
}

void GpuRenderer::OnVramWrite(const Ps1Gpu& gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    FlushPending();
    if (m_set && x == 0 && y == 0 && w >= PS1_VRAM_WIDTH && h >= PS1_VRAM_HEIGHT) {
        // The replay's SetVram (the latest game frame's 1x VRAM): the replay target already holds its scaled
        // picture (snapshot); only the 1x texture is stale.
        std::memset(m_dirty[1], 1, sizeof m_dirty[1]);
        return;
    }
    Upload(gpu, x, y, w, h, true);
}

// ── Frame interpolation ──────────────────────────────────────────────────────────────────────────────

void GpuRenderer::Flip() {
    if (!m_tex[kSnap0]) return;
    FlushPending();
    m_snap = m_snap == kSnap0 ? kSnap1 : kSnap0;
    m_ops.push_back({Op::Whole, kHi, m_snap, 0, 0, 0, 0, 0, 0, 0, 0});
}

void GpuRenderer::BeginReplay(Ps1Gpu& scratch) {
    if (!m_tex[kHi2] || m_set) return;
    FlushPending();
    m_live_area = m_area;
    m_ops.push_back({Op::Whole, m_snap, kHi2, 0, 0, 0, 0, 0, 0, 0, 0});
    m_set = 1;
    std::memset(m_dirty[1], 1, sizeof m_dirty[1]);
    m_replay_gpu = &scratch;
    scratch.SetDrawListener(this);
}

void GpuRenderer::EndReplay() {
    if (!m_set) return;
    FlushPending();
    m_replay_gpu->SetDrawListener(nullptr);
    m_replay_gpu = nullptr;
    m_set = 0;
    m_area = m_live_area;
}

// ── Submission ────────────────────────────────────────────────────────────────────────────────────

SDL_Texture* GpuRenderer::OutputTexture(SDL_Renderer* r) {
    if (!m_shared || !m_out) return nullptr;
    if (!m_out_tex) {
        const SDL_PropertiesID p = SDL_CreateProperties();
        SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
        SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
        SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, m_out_w);
        SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, m_out_h);
        SDL_SetPointerProperty(p, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, m_out);
        m_out_tex = SDL_CreateTextureWithProperties(r, p);
        SDL_DestroyProperties(p);
        if (m_out_tex) SDL_SetTextureScaleMode(m_out_tex, SDL_SCALEMODE_LINEAR);
    }
    return m_out_tex;
}

bool GpuRenderer::Present(const GpuDisplayArea& d, bool show, std::vector<uint32_t>* rgba, Source which) {
    if (!m_dev) return false;
    FlushPending();
    show = show || rgba;
    SDL_GPUTexture* shown = m_tex[which == kReplay ? kHi2 : which == kPrev ? (m_snap == kSnap0 ? kSnap1 : kSnap0)
                                  : which == kCur ? m_snap : kHi];
    if (!shown) shown = m_tex[kHi];
    const uint32_t ow = (d.width ? d.width : 320) * m_s, oh = (d.height ? d.height : 240) * m_s;
    if (show && (!m_out || ow != m_out_w || oh != m_out_h)) {
        if (m_out_tex) SDL_DestroyTexture(m_out_tex), m_out_tex = nullptr;
        if (m_out) SDL_ReleaseGPUTexture(m_dev, m_out);
        m_out = MakeTexture(m_dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, ow, oh);
        m_out_w = ow, m_out_h = oh;
        if (!m_out) return false;
    }
    const uint32_t disp = uint32_t(m_vtx.size());
    if (show) {
        const uint32_t a[4] = {(d.x & 1023u) * m_s, (d.y & 511u) * m_s, PS1_VRAM_WIDTH * m_s, PS1_VRAM_HEIGHT * m_s};
        const float c[6][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1}, {-1, -1}};
        for (auto& p : c) m_vtx.push_back({p[0], p[1], 0, 0, 0, 0, 0, {a[0], a[1], a[2], a[3]}});
    }
    const uint32_t vbytes = uint32_t(m_vtx.size() * sizeof(Vtx)), soff = AlignUp(vbytes, 512);
    const uint32_t total = soff + uint32_t(m_staging.size() * 2), down = rgba ? ow * oh * 4 : 0;
    auto grow_tb = [&](SDL_GPUTransferBuffer*& b, uint32_t& size, uint32_t need, SDL_GPUTransferBufferUsage u) {
        if (b && size >= need) return;
        if (b) SDL_ReleaseGPUTransferBuffer(m_dev, b);
        size = std::max(need, size * 2);
        const SDL_GPUTransferBufferCreateInfo ci{u, size, 0};
        b = SDL_CreateGPUTransferBuffer(m_dev, &ci);
    };
    if (total) grow_tb(m_up, m_up_size, total, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
    if (rgba) grow_tb(m_down, m_down_size, down, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
    if (vbytes && (!m_vbuf || m_vbuf_size < vbytes)) {
        if (m_vbuf) SDL_ReleaseGPUBuffer(m_dev, m_vbuf);
        m_vbuf_size = std::max(vbytes, m_vbuf_size * 2);
        const SDL_GPUBufferCreateInfo ci{SDL_GPU_BUFFERUSAGE_VERTEX, m_vbuf_size, 0};
        m_vbuf = SDL_CreateGPUBuffer(m_dev, &ci);
    }
    if ((total && !m_up) || (rgba && !m_down) || (vbytes && !m_vbuf)) return false;
    if (total) {
        auto* map = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_dev, m_up, true));
        if (!map) return false;
        std::memcpy(map, m_vtx.data(), vbytes);
        if (!m_staging.empty()) std::memcpy(map + soff, m_staging.data(), m_staging.size() * 2);
        SDL_UnmapGPUTransferBuffer(m_dev, m_up);
    }

    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(m_dev);
    if (!cb) return false;
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    if (vbytes) {
        const SDL_GPUTransferBufferLocation vsrc{m_up, 0};
        const SDL_GPUBufferRegion vdst{m_vbuf, 0, vbytes};
        SDL_UploadToGPUBuffer(cp, &vsrc, &vdst, true);
    }
    SDL_GPURenderPass* rp = nullptr;
    int cur = -1, rp_set = -1;
    int32_t bound_hd = -1;
    const SDL_GPUBufferBinding vbind{m_vbuf, 0};
    auto begin_render = [&](SDL_GPUTexture* target, SDL_GPUTexture* sampled, SDL_GPULoadOp load) {
        SDL_GPUColorTargetInfo ci{};
        ci.texture = target;
        ci.load_op = load;
        ci.store_op = SDL_GPU_STOREOP_STORE;
        rp = SDL_BeginGPURenderPass(cb, &ci, 1, nullptr);
        SDL_BindGPUVertexBuffers(rp, 0, &vbind, 1);
        const SDL_GPUTextureSamplerBinding sb[3] = {{sampled, m_sampler}, {m_tex[kTmp], m_sampler}, {m_tex[kTmp], m_sampler}};
        SDL_BindGPUFragmentSamplers(rp, 0, sb, target == m_out ? 1 : 3);
        bound_hd = -1;
        SDL_SetGPUBlendConstants(rp, SDL_FColor{0.5f, 0.5f, 0.5f, 0.5f});
        cur = -1;
    };
    const uint32_t s = m_s;
    uint32_t draws = 0;
    for (const Op& op : m_ops) {
        if (op.kind == Op::Draw) {
            if (rp && rp_set != op.set) SDL_EndGPURenderPass(rp), rp = nullptr;
            if (cp) SDL_EndGPUCopyPass(cp), cp = nullptr;
            if (!rp) begin_render(m_tex[op.set ? kHi2 : kHi], m_vram1x[op.set], SDL_GPU_LOADOP_LOAD), rp_set = op.set;
            if (cur != op.pipe) SDL_BindGPUGraphicsPipeline(rp, m_pipe[op.pipe]), cur = op.pipe;
            if (bound_hd != op.hd) {
                const SDL_GPUTextureSamplerBinding hb{op.hd >= 0 ? m_hd_tex[size_t(op.hd)] : m_tex[kTmp], m_sampler};
                SDL_BindGPUFragmentSamplers(rp, 2, &hb, 1);
                bound_hd = op.hd;
            }
            const SDL_Rect sc{op.x, op.y, op.w, op.h};
            SDL_SetGPUScissor(rp, &sc);
            SDL_DrawGPUPrimitives(rp, op.count, 1, op.first, 0);
            ++draws;
            continue;
        }
        if (rp) SDL_EndGPURenderPass(rp), rp = nullptr;
        if (!cp) cp = SDL_BeginGPUCopyPass(cb);
        SDL_GPUTexture* hi = m_tex[op.set ? kHi2 : kHi];
        if (op.kind == Op::Upload) {
            const SDL_GPUTextureTransferInfo src{m_up, soff + op.first * 2, op.count, uint32_t(op.h)};
            SDL_GPUTexture* t = op.hd >= 0 ? m_hd_tex[size_t(op.hd)] : m_vram1x[op.set];
            const SDL_GPUTextureRegion dst{t, 0, 0, uint32_t(op.x), uint32_t(op.y), 0, uint32_t(op.w), uint32_t(op.h), 1};
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
        } else if (op.kind == Op::Copy) {  // via the temp texture: source and destination may overlap
            const SDL_GPUTextureLocation a{hi, 0, 0, op.sx * s, op.sy * s, 0}, t{m_tex[kTmp], 0, 0, op.sx * s, op.sy * s, 0},
                b{hi, 0, 0, op.x * s, op.y * s, 0};
            SDL_CopyGPUTextureToTexture(cp, &a, &t, op.w * s, op.h * s, 1, false);
            SDL_CopyGPUTextureToTexture(cp, &t, &b, op.w * s, op.h * s, 1, false);
        } else if (op.kind == Op::Snap) {  // destination copy for a mask-checked B/2+F/2 primitive
            const SDL_GPUTextureLocation a{hi, 0, 0, op.x * s, op.y * s, 0}, t{m_tex[kTmp], 0, 0, op.x * s, op.y * s, 0};
            SDL_CopyGPUTextureToTexture(cp, &a, &t, op.w * s, op.h * s, 1, false);
        } else {  // Whole: texture pipe -> texture set (snapshots)
            const SDL_GPUTextureLocation a{m_tex[op.pipe], 0, 0, 0, 0, 0}, b{m_tex[op.set], 0, 0, 0, 0, 0};
            SDL_CopyGPUTextureToTexture(cp, &a, &b, PS1_VRAM_WIDTH * s, PS1_VRAM_HEIGHT * s, 1, false);
        }
    }
    if (cp) SDL_EndGPUCopyPass(cp), cp = nullptr;
    if (rp) SDL_EndGPURenderPass(rp), rp = nullptr;
    if (show) {
        begin_render(m_out, shown, SDL_GPU_LOADOP_DONT_CARE);
        SDL_BindGPUGraphicsPipeline(rp, m_display);
        SDL_DrawGPUPrimitives(rp, 6, 1, disp, 0);
        SDL_EndGPURenderPass(rp);
    }
    m_vtx.clear(), m_staging.clear(), m_ops.clear();
    m_last_draws = draws;
    if (!rgba) return SDL_SubmitGPUCommandBuffer(cb);  // no fence wait: the presenter's command buffer comes after
    cp = SDL_BeginGPUCopyPass(cb);
    const SDL_GPUTextureRegion oreg{m_out, 0, 0, 0, 0, 0, ow, oh, 1};
    const SDL_GPUTextureTransferInfo odst{m_down, 0, ow, oh};
    SDL_DownloadFromGPUTexture(cp, &oreg, &odst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
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
