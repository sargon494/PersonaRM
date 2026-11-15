#pragma once
// Host GPU renderer (SDL_gpu; prototype: D3D12 with runtime-compiled HLSL): replays the GP0 primitives at
// s x on the host GPU, display only. The 1x VRAM in Ps1Gpu stays the emulation's source of truth and the
// only texture source. Design: docs/GPU_RENDERER.md. Built into personarm_sdl only (needs SDL3).

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

namespace personarm {

class GpuRenderer : public GpuDrawListener {
public:
    ~GpuRenderer() override { Shutdown(); }

    /// Creates the device and resources and attaches to `gpu` (scale 1-9). False + err: nothing attached.
    bool Init(Ps1Gpu& gpu, uint32_t scale, std::string* err);
    void Shutdown();

    /// Runs the work queued since the last call, then (rgba non-null) reads back the 15bpp display area d
    /// as (d.width*s)x(d.height*s) 0xAABBGGRR pixels (same conversion as host::DisplayToRgba).
    bool Present(const GpuDisplayArea& d, std::vector<uint32_t>* rgba);
    uint32_t Scale() const { return m_s; }
    uint32_t LastDrawCalls() const { return m_last_draws; }

    void OnGp0(const Ps1Gpu& gpu, const uint32_t* w, size_t n) override;
    void OnVramWrite(const Ps1Gpu& gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;

private:
    struct Vtx {
        float x, y, r, g, b, u, v;
        uint32_t attr[4];  // texpage, clut, texwindow, flags (see kShaderPs)
    };
    struct Op {
        enum Kind : uint8_t { Draw, Upload, Copy } kind;
        uint8_t pipe;  // Draw: 0 opaque, 1 + E1h semi mode
        uint32_t first, count;              // Draw: vertices; Upload: staging offset (u16), row pitch (u16)
        int32_t x, y, w, h, sx, sy;         // Draw: scissor (target px); Upload: 1x rect; Copy: 1x rects
    };
    struct Rect { int32_t x0, y0, x1, y1; };  // inclusive, 1x VRAM units

    void Emit(const Vtx* v, size_t n, uint8_t pipe, const Rect& clip);
    void Upload(const Ps1Gpu& gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool blit);
    void MarkDirty(const Rect& r);
    void SyncRegion(const Ps1Gpu& gpu, const Rect& r);
    void SyncTexture(const Ps1Gpu& gpu, uint32_t tp, uint32_t clut, uint32_t tw, int32_t u0, int32_t u1, int32_t v0, int32_t v1);
    void FlushResync();
    void Fallback(const char* what, const Rect& r);
    bool Clip(Rect& r) const;
    void Polygon(const Ps1Gpu& gpu, uint8_t cmd, const uint32_t* w);
    void Rectangle(const Ps1Gpu& gpu, uint8_t cmd, const uint32_t* w);
    void Prim(const Ps1Gpu& gpu, Vtx* v, size_t n, bool semi, bool textured, const Rect& bbox);

    const Ps1Gpu* m_gpu = nullptr;
    Ps1Gpu* m_attached = nullptr;
    uint32_t m_s = 1;
    GpuDrawingArea m_area;
    SDL_GPUDevice* m_dev = nullptr;
    SDL_GPUTexture *m_vram1x = nullptr, *m_hi = nullptr, *m_tmp = nullptr, *m_out = nullptr;
    uint32_t m_out_w = 0, m_out_h = 0;
    SDL_GPUSampler* m_sampler = nullptr;
    SDL_GPUGraphicsPipeline* m_pipe[5] = {};
    SDL_GPUGraphicsPipeline* m_display = nullptr;
    SDL_GPUBuffer* m_vbuf = nullptr;
    uint32_t m_vbuf_size = 0;
    SDL_GPUTransferBuffer *m_up = nullptr, *m_down = nullptr;
    uint32_t m_up_size = 0, m_down_size = 0;

    std::vector<Vtx> m_vtx;
    std::vector<uint16_t> m_staging;
    std::vector<Op> m_ops;
    bool m_dirty[16][32] = {};  // 32x32 blocks of the 1x VRAM newer than the GPU's 1x texture
    bool m_resync = false;      // a fallback primitive's rect: re-upload it from the 1x VRAM once drawn
    Rect m_resync_rect{};
    uint32_t m_logged = 0;      // fallback kinds already logged
    uint32_t m_last_draws = 0;
};

} // namespace personarm
