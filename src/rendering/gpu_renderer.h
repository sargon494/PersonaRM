#pragma once
// Host GPU renderer (SDL_gpu; D3D12 with runtime-compiled HLSL): replays the GP0 primitives at s x on the
// host GPU, display only. The 1x VRAM in Ps1Gpu stays the emulation's source of truth and the only
// texture source. Design: docs/GPU_RENDERER.md. Built into personarm_sdl only (needs SDL3).

#include <cstdint>
#include <string>
#include <vector>

#include "rendering/ps1_gpu.h"

struct SDL_GPUDevice;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct SDL_GPUBuffer;
struct SDL_GPUTransferBuffer;
struct SDL_GPUGraphicsPipeline;
struct SDL_Renderer;
struct SDL_Texture;

namespace personarm {

class VramTextureHook;
struct HdTexture;

class GpuRenderer : public GpuDrawListener {
public:
    ~GpuRenderer() override { Shutdown(); }

    /// Creates the resources (on `device` if given, e.g. the window's SDL GPU renderer, else on a device of
    /// its own) and attaches to `gpu` (scale 1-9). False + err: nothing attached.
    bool Init(Ps1Gpu& gpu, uint32_t scale, std::string* err, SDL_GPUDevice* device = nullptr);
    void Shutdown();

    /// Picture sources for Present: the live target, or (frame interpolation) the replay target and the
    /// snapshots taken at the last two flips.
    enum Source : uint8_t { kLive, kReplay, kPrev, kCur };
    /// Runs the work queued since the last call. show: also converts the 15bpp display area d of `src` into
    /// the (d.width*s)x(d.height*s) output texture (same conversion as host::DisplayToRgba); rgba non-null:
    /// reads that picture back as 0xAABBGGRR pixels (a fence wait: screenshots and dumps only).
    bool Present(const GpuDisplayArea& d, bool show, std::vector<uint32_t>* rgba = nullptr, Source src = kLive);
    /// The output texture of the last shown picture as a texture of `r`, which must be an SDL GPU renderer
    /// on this device (zero copy); null otherwise. Owned here, valid until the next Present.
    SDL_Texture* OutputTexture(SDL_Renderer* r);
    bool SharedDevice() const { return m_shared; }
    uint32_t Scale() const { return m_s; }
    uint32_t LastDrawCalls() const { return m_last_draws; }

    /// Frame interpolation (rendering/frame_interp.h) on the GPU. EnableInterp allocates the replay target
    /// and two snapshots; each logged flip (OnFlip) snapshots the live target as it is then, as the
    /// interpolator's FlipVram; between BeginReplay(scratch) and EndReplay the interpolator's scratch GPU
    /// replays the lerped packets into the replay target, started from the previous flip's snapshot (what
    /// the latest segment was drawn over), textures from the scratch's 1x VRAM.
    bool EnableInterp(std::string* err);
    void BeginReplay(Ps1Gpu& scratch);
    void EndReplay();

    void OnGp0(const Ps1Gpu& gpu, const uint32_t* w, size_t n) override;
    void OnVramWrite(const Ps1Gpu& gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;
    void OnLine(const Ps1Gpu& gpu, int32_t ax, int32_t ay, uint32_t ac, int32_t bx, int32_t by, uint32_t bc, bool gouraud,
                bool semi) override;
    void OnSqueeze(const int32_t* x, int n) override;
    void OnFlip(const Ps1Gpu& gpu) override;
    /// HD texture replacement (--mods): textured primitives sampling a replaced upload draw from its HD
    /// image (rendering/vram_texture_hook.h). Null = off.
    void SetTextureHook(VramTextureHook* hook) { m_hook = hook; }

private:
    struct Vtx {
        float x, y, r, g, b, u, v;
        uint32_t attr[4];  // texpage, clut, texwindow, flags (see kShaderPs)
        uint32_t hd[2];    // HD replacement: upload x | y << 10 | 1 << 31, w | h << 16 (VRAM words); 0 = none
    };
    // Textures by index (Op::Whole, Present sources).
    enum Tex : uint8_t { kHi, kHi2, kSnap0, kSnap1, kTmp, kTexCount };
    struct Op {
        enum Kind : uint8_t { Draw, Upload, Copy, Snap, Whole } kind;
        uint8_t pipe;  // Draw: 0 opaque, 1 + E1h semi mode; +5 mask-checked. Whole: source Tex
        uint8_t set;   // 0 live (hi, vram1x), 1 replay (hi2, vram1x2); Whole: destination Tex
        uint32_t first, count;              // Draw: vertices; Upload: staging offset (u16), row pitch (u16)
        int32_t x, y, w, h, sx, sy;         // Draw: scissor (target px); Upload: 1x rect; Copy: 1x rects; Snap: 1x rect
        int32_t hd = -1;                    // Draw: HD texture bound (m_hd_tex); Upload: HD texture written (-1: vram1x)
    };
    struct Rect { int32_t x0, y0, x1, y1; };  // inclusive, 1x VRAM units
    // The polygon / rectangle of the last OnGp0, drawn at the next callback so OnSqueeze can still move it.
    struct Pending {
        bool on = false, rect = false, semi = false, tex = false, mask_check = false;
        int n = 0;                                  // polygon vertices (rectangle: 2 corners)
        int32_t x[4], y[4];                         // VRAM space; rectangle: left/right, top/bottom (exclusive)
        float r[4], g[4], b[4], u[4], v[4];         // rectangle: u/v corners in u[0..1], v[0..1]
        uint32_t attr[4], hd[2];
        int32_t hd_tex;
    };

    void Emit(const Vtx* v, size_t n, uint8_t pipe, const Rect& clip);
    void Upload(const Ps1Gpu& gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool blit);
    void LookupHd(uint32_t tp, uint32_t tw, int32_t u0, int32_t u1, int32_t v0, int32_t v1);
    void MarkDirty(const Rect& r);
    void SyncRegion(const Ps1Gpu& gpu, const Rect& r);
    void SyncTexture(const Ps1Gpu& gpu, uint32_t tp, uint32_t clut, uint32_t tw, int32_t u0, int32_t u1, int32_t v0, int32_t v1);
    bool Clip(Rect& r) const;
    void Polygon(const Ps1Gpu& gpu, uint8_t cmd, const uint32_t* w);
    void Rectangle(const Ps1Gpu& gpu, uint8_t cmd, const uint32_t* w);
    void FlushPending();
    void Draw(Vtx* v, size_t n, bool semi, bool textured, bool mask_check, Rect r);
    SDL_GPUTexture* Target() const { return m_tex[m_set ? kHi2 : kHi]; }

    Ps1Gpu* m_attached = nullptr;
    uint32_t m_s = 1;
    GpuDrawingArea m_area;
    SDL_GPUDevice* m_dev = nullptr;
    bool m_shared = false;      // m_dev belongs to the window's renderer
    SDL_Texture* m_out_tex = nullptr;
    SDL_GPUTexture* m_tex[kTexCount] = {};
    SDL_GPUTexture *m_vram1x[2] = {}, *m_out = nullptr;  // per set
    uint32_t m_out_w = 0, m_out_h = 0;
    SDL_GPUSampler* m_sampler = nullptr;
    SDL_GPUGraphicsPipeline* m_pipe[10] = {};
    SDL_GPUGraphicsPipeline* m_display = nullptr;
    SDL_GPUBuffer* m_vbuf = nullptr;
    uint32_t m_vbuf_size = 0;
    SDL_GPUTransferBuffer *m_up = nullptr, *m_down = nullptr;
    uint32_t m_up_size = 0, m_down_size = 0;

    std::vector<Vtx> m_vtx;
    std::vector<uint16_t> m_staging;
    std::vector<Op> m_ops;
    Pending m_pending;
    uint8_t m_set = 0;              // set being recorded (1 between BeginReplay and EndReplay)
    uint8_t m_snap = kSnap0;        // snapshot at the latest flip (the other one: the flip before)
    GpuDrawingArea m_live_area;     // saved over a replay
    Ps1Gpu* m_replay_gpu = nullptr; // the scratch GPU listened to during a replay
    VramTextureHook* m_hook = nullptr;
    std::vector<const HdTexture*> m_hd_src;    // per HD texture: its image
    std::vector<SDL_GPUTexture*> m_hd_tex;
    int32_t m_draw_hd = -1;                    // HD texture of the primitive being emitted
    bool m_dirty[2][16][32] = {};   // per set: 32x32 blocks of the 1x VRAM newer than the GPU's 1x texture
    uint32_t m_last_draws = 0;
};

} // namespace personarm
