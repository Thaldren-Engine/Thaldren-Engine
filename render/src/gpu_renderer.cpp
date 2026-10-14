#include "render/gpu_renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "generated/shaders.h"

namespace render {
namespace {

std::string g_error;

bool fail(const char* what) {
    g_error = std::string(what) + ": " + SDL_GetError();
    return false;
}

struct Rect {
    int x0, y0, x1, y1;  // inclusive
    bool overlaps(const Rect& o) const { return x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1; }
};

} // namespace

struct GpuRenderer::Impl {
    SDL_GPUDevice* dev = nullptr;
    SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_INVALID;
    SDL_GPUGraphicsPipeline* blit = nullptr;
    SDL_GPUGraphicsPipeline* resolve = nullptr;
    SDL_GPUSampler* sampler = nullptr;
    SDL_GPUTexture* table = nullptr;
    SDL_GPUTexture* light_table = nullptr;  // BlitLight's table (uploaded with the first list using it)
    const BlendTable* light_uploaded = nullptr;
    SDL_GPUTexture* remap_table = nullptr;  // BlitRemap's table
    const BlendTable* remap_uploaded = nullptr;
    SDL_GPUTexture* dummy = nullptr;
    SDL_GPUTexture* palette = nullptr;
    // Per frame size.
    int width = 0, height = 0;
    SDL_GPUTexture* frame = nullptr;
    SDL_GPUTexture* frame_copy = nullptr;
    SDL_GPUTexture* rgba = nullptr;
    // Persistent sprite textures (terrain tiles, feature frames, ...).
    std::unordered_map<const Sprite8*, SDL_GPUTexture*> cache;
    BlendTable blend;
    // Window output.
    SDL_GPUShader* quad_vs = nullptr;
    SDL_GPUShader* resolve_fs = nullptr;
    SDL_Window* window = nullptr;
    SDL_GPUGraphicsPipeline* window_resolve = nullptr;

    ~Impl() {
        if (!dev) return;
        if (window_resolve) SDL_ReleaseGPUGraphicsPipeline(dev, window_resolve);
        if (window) {
            SDL_ReleaseWindowFromGPUDevice(dev, window);
            SDL_DestroyWindow(window);
        }
        if (quad_vs) SDL_ReleaseGPUShader(dev, quad_vs);
        if (resolve_fs) SDL_ReleaseGPUShader(dev, resolve_fs);
        for (auto& [k, t] : cache) SDL_ReleaseGPUTexture(dev, t);
        if (light_table) SDL_ReleaseGPUTexture(dev, light_table);
        if (remap_table) SDL_ReleaseGPUTexture(dev, remap_table);
        for (SDL_GPUTexture* t : {table, dummy, palette, frame, frame_copy, rgba})
            if (t) SDL_ReleaseGPUTexture(dev, t);
        if (sampler) SDL_ReleaseGPUSampler(dev, sampler);
        if (blit) SDL_ReleaseGPUGraphicsPipeline(dev, blit);
        if (resolve) SDL_ReleaseGPUGraphicsPipeline(dev, resolve);
        SDL_DestroyGPUDevice(dev);
    }

    SDL_GPUShader* shader(const unsigned char* dxil, unsigned dxil_n, const unsigned char* spv, unsigned spv_n,
                          const unsigned char* msl, unsigned msl_n, SDL_GPUShaderStage stage, Uint32 samplers) {
        SDL_GPUShaderCreateInfo ci{};
        ci.stage = stage;
        ci.num_samplers = samplers;
        ci.num_uniform_buffers = 1;
        ci.format = format;
        if (format == SDL_GPU_SHADERFORMAT_DXIL) {
            ci.code = dxil;
            ci.code_size = dxil_n;
            ci.entrypoint = "main";
        } else if (format == SDL_GPU_SHADERFORMAT_SPIRV) {
            ci.code = spv;
            ci.code_size = spv_n;
            ci.entrypoint = "main";
        } else {
            ci.code = msl;
            ci.code_size = msl_n;
            ci.entrypoint = "main0";
        }
        return SDL_CreateGPUShader(dev, &ci);
    }

    SDL_GPUGraphicsPipeline* pipeline(SDL_GPUShader* vs, SDL_GPUShader* fs, SDL_GPUTextureFormat target) {
        SDL_GPUColorTargetDescription ct{};
        ct.format = target;
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = vs;
        pi.fragment_shader = fs;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        return SDL_CreateGPUGraphicsPipeline(dev, &pi);
    }

    SDL_GPUTexture* texture(int w, int h, SDL_GPUTextureFormat f, SDL_GPUTextureUsageFlags usage) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = f;
        ti.usage = usage;
        ti.width = static_cast<Uint32>(std::max(w, 1));
        ti.height = static_cast<Uint32>(std::max(h, 1));
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        return SDL_CreateGPUTexture(dev, &ti);
    }

    // Uploads several images in one copy pass. Each entry: texture, w, h, bytes per pixel, data.
    struct Upload {
        SDL_GPUTexture* tex;
        int w, h, bpp;
        const std::uint8_t* data;
    };
    bool upload(const std::vector<Upload>& ups) {
        if (ups.empty()) return true;
        Uint32 total = 0;
        for (const auto& u : ups) total += static_cast<Uint32>(u.w * u.h * u.bpp);
        SDL_GPUTransferBufferCreateInfo bi{};
        bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        bi.size = std::max<Uint32>(total, 4);
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &bi);
        if (!tb) return fail("SDL_CreateGPUTransferBuffer");
        auto* p = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(dev, tb, false));
        Uint32 off = 0;
        for (const auto& u : ups) {
            std::memcpy(p + off, u.data, static_cast<std::size_t>(u.w * u.h * u.bpp));
            off += static_cast<Uint32>(u.w * u.h * u.bpp);
        }
        SDL_UnmapGPUTransferBuffer(dev, tb);
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
        off = 0;
        for (const auto& u : ups) {
            SDL_GPUTextureTransferInfo src{tb, off, static_cast<Uint32>(u.w), static_cast<Uint32>(u.h)};
            SDL_GPUTextureRegion dst{};
            dst.texture = u.tex;
            dst.w = static_cast<Uint32>(u.w);
            dst.h = static_cast<Uint32>(u.h);
            dst.d = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
            off += static_cast<Uint32>(u.w * u.h * u.bpp);
        }
        SDL_EndGPUCopyPass(cp);
        const bool ok = SDL_SubmitGPUCommandBuffer(cb);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        return ok || fail("SDL_SubmitGPUCommandBuffer");
    }

    bool ensure_frame(int w, int h) {
        if (w == width && h == height && frame) return true;
        for (SDL_GPUTexture** t : {&frame, &frame_copy, &rgba})
            if (*t) {
                SDL_ReleaseGPUTexture(dev, *t);
                *t = nullptr;
            }
        width = w;
        height = h;
        frame = texture(w, h, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        frame_copy = texture(w, h, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        rgba = texture(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
        return (frame && frame_copy && rgba) || fail("SDL_CreateGPUTexture (frame)");
    }

    // Records the draw list into `cb`; transient textures are returned for release after submission.
    bool record(SDL_GPUCommandBuffer* cb, const DrawList& list, std::vector<SDL_GPUTexture*>& transient) {
        // The light table (BlitLight) is a second mixing table.
        if (list.light && list.light->size() >= 65536 && list.light != light_uploaded) {
            if (!light_table) light_table = texture(256, 256, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
            if (!light_table || !upload({{light_table, 256, 256, 1, list.light->data()}})) return false;
            light_uploaded = list.light;
        }
        if (list.remap && list.remap->size() >= 65536 && list.remap != remap_uploaded) {
            if (!remap_table) remap_table = texture(256, 256, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
            if (!remap_table || !upload({{remap_table, 256, 256, 1, list.remap->data()}})) return false;
            remap_uploaded = list.remap;
        }
        // Textures for every sprite in the list.
        std::unordered_map<const Sprite8*, SDL_GPUTexture*> frame_tex;
        std::vector<Upload> ups;
        for (const DrawCmd& c : list.cmds) {
            if (!c.sprite || c.sprite->width <= 0 || c.sprite->height <= 0) continue;
            if (frame_tex.count(c.sprite)) continue;
            SDL_GPUTexture* t = nullptr;
            if (c.persistent) {
                auto it = cache.find(c.sprite);
                if (it != cache.end()) {
                    frame_tex[c.sprite] = it->second;
                    continue;
                }
            }
            t = texture(c.sprite->width, c.sprite->height, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
            if (!t) return fail("SDL_CreateGPUTexture (sprite)");
            ups.push_back({t, c.sprite->width, c.sprite->height, 1, c.sprite->pixels.data()});
            frame_tex[c.sprite] = t;
            if (c.persistent) cache[c.sprite] = t;
            else transient.push_back(t);
        }
        if (!upload(ups)) return false;

        SDL_GPUColorTargetInfo target{};
        target.texture = frame;
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        const float clear = static_cast<float>(list.clear_index) / 255.0f;
        target.clear_color = {clear, 0, 0, 1};
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cb, &target, 1, nullptr);
        target.load_op = SDL_GPU_LOADOP_LOAD;
        std::vector<Rect> dirty;  // drawn since the last frame copy
        bool copied_once = false;
        for (const DrawCmd& c : list.cmds) {
            if (c.op == DrawOp::Line || !c.sprite || c.sprite->width <= 0) continue;
            const Sprite8& s = *c.sprite;
            const int left = c.x - s.x_offset, top = c.y - s.y_offset;
            Rect r{std::max({left, c.clip.x0, 0}), std::max({top, c.clip.y0, 0}),
                   std::min({left + s.width - 1, c.clip.x1, width - 1}), std::min({top + s.height - 1, c.clip.y1, height - 1})};
            if (r.x0 > r.x1 || r.y0 > r.y1) continue;
            const bool light_op = c.op == DrawOp::BlitLight;
            if (light_op && !light_table) continue;
            const bool remap_op = c.op == DrawOp::BlitRemap;
            if (remap_op && !remap_table) continue;
            const bool blend_op = c.op == DrawOp::BlitBlend || light_op || remap_op;
            if (blend_op) {
                bool need = !copied_once;
                for (const Rect& d : dirty)
                    if (d.overlaps(r)) need = true;
                if (need) {
                    SDL_EndGPURenderPass(pass);
                    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
                    SDL_GPUTextureLocation from{}, to{};
                    from.texture = frame;
                    to.texture = frame_copy;
                    SDL_CopyGPUTextureToTexture(cp, &from, &to, static_cast<Uint32>(width), static_cast<Uint32>(height), 1, false);
                    SDL_EndGPUCopyPass(cp);
                    pass = SDL_BeginGPURenderPass(cb, &target, 1, nullptr);
                    dirty.clear();
                    copied_once = true;
                }
            }
            dirty.push_back(r);
            SDL_BindGPUGraphicsPipeline(pass, blit);
            const SDL_Rect sc{r.x0, r.y0, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1};
            SDL_SetGPUScissor(pass, &sc);
            const float vu[8] = {static_cast<float>(left), static_cast<float>(top), static_cast<float>(left + s.width),
                                 static_cast<float>(top + s.height), static_cast<float>(width), static_cast<float>(height), 0, 0};
            SDL_PushGPUVertexUniformData(cb, 0, vu, sizeof vu);
            const std::int32_t fu[8] = {left, top, s.has_key ? s.key : -1, blend_op ? 1 : 0, 0, 0, 0, 0};
            SDL_PushGPUFragmentUniformData(cb, 0, fu, sizeof fu);
            const SDL_GPUTextureSamplerBinding b[3] = {{frame_tex[c.sprite], sampler},
                                                       {blend_op ? frame_copy : dummy, sampler},
                                                       {light_op ? light_table : remap_op ? remap_table : table, sampler}};
            SDL_BindGPUFragmentSamplers(pass, 0, b, 3);
            SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
        return true;
    }

    bool finish(SDL_GPUCommandBuffer* cb, SDL_GPUTransferBuffer* tb, std::vector<SDL_GPUTexture*>& transient) {
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
        if (!fence) return fail("SDL_SubmitGPUCommandBufferAndAcquireFence");
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        for (SDL_GPUTexture* t : transient) SDL_ReleaseGPUTexture(dev, t);
        transient.clear();
        (void)tb;
        return true;
    }

    bool download(SDL_GPUCommandBuffer* cb, SDL_GPUTexture* tex, int bpp, SDL_GPUTransferBuffer*& tb) {
        SDL_GPUTransferBufferCreateInfo bi{};
        bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        bi.size = static_cast<Uint32>(width * height * bpp);
        tb = SDL_CreateGPUTransferBuffer(dev, &bi);
        if (!tb) return fail("SDL_CreateGPUTransferBuffer (download)");
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
        SDL_GPUTextureRegion src{};
        src.texture = tex;
        src.w = static_cast<Uint32>(width);
        src.h = static_cast<Uint32>(height);
        src.d = 1;
        SDL_GPUTextureTransferInfo dst{tb, 0, static_cast<Uint32>(width), static_cast<Uint32>(height)};
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        SDL_EndGPUCopyPass(cp);
        return true;
    }
};

const std::string& GpuRenderer::last_error() { return g_error; }

GpuRenderer::GpuRenderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuRenderer::~GpuRenderer() = default;

std::unique_ptr<GpuRenderer> GpuRenderer::create(const BlendTable& blend) {
    if (blend.size() < 65536) {
        g_error = "blend table missing";
        return nullptr;
    }
    // GPU devices live in SDL's video subsystem (no window is needed for offscreen work).
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fail("SDL_InitSubSystem(VIDEO)");
        return nullptr;
    }
    auto im = std::make_unique<Impl>();
    im->blend = blend;
    im->dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (!im->dev) {
        fail("SDL_CreateGPUDevice");
        return nullptr;
    }
    const SDL_GPUShaderFormat f = SDL_GetGPUShaderFormats(im->dev);
    im->format = (f & SDL_GPU_SHADERFORMAT_DXIL) ? SDL_GPU_SHADERFORMAT_DXIL
               : (f & SDL_GPU_SHADERFORMAT_SPIRV) ? SDL_GPU_SHADERFORMAT_SPIRV
                                                  : SDL_GPU_SHADERFORMAT_MSL;
    SDL_GPUShader* vs = im->shader(quad_vert_dxil, quad_vert_dxil_size, quad_vert_spirv, quad_vert_spirv_size,
                                   quad_vert_msl, quad_vert_msl_size, SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* bs = im->shader(blit_frag_dxil, blit_frag_dxil_size, blit_frag_spirv, blit_frag_spirv_size,
                                   blit_frag_msl, blit_frag_msl_size, SDL_GPU_SHADERSTAGE_FRAGMENT, 3);
    SDL_GPUShader* rs = im->shader(resolve_frag_dxil, resolve_frag_dxil_size, resolve_frag_spirv, resolve_frag_spirv_size,
                                   resolve_frag_msl, resolve_frag_msl_size, SDL_GPU_SHADERSTAGE_FRAGMENT, 2);
    if (!vs || !bs || !rs) {
        fail("SDL_CreateGPUShader");
        return nullptr;
    }
    im->blit = im->pipeline(vs, bs, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
    im->resolve = im->pipeline(vs, rs, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    SDL_ReleaseGPUShader(im->dev, bs);
    im->quad_vs = vs;
    im->resolve_fs = rs;
    if (!im->blit || !im->resolve) {
        fail("SDL_CreateGPUGraphicsPipeline");
        return nullptr;
    }
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = SDL_GPU_FILTER_NEAREST;
    si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    im->sampler = SDL_CreateGPUSampler(im->dev, &si);
    im->table = im->texture(256, 256, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    im->dummy = im->texture(1, 1, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    im->palette = im->texture(256, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!im->sampler || !im->table || !im->dummy || !im->palette) {
        fail("SDL GPU resources");
        return nullptr;
    }
    const std::uint8_t zero[4] = {0, 0, 0, 0};
    if (!im->upload({{im->table, 256, 256, 1, im->blend.data()}, {im->dummy, 1, 1, 1, zero}})) return nullptr;
    return std::unique_ptr<GpuRenderer>(new GpuRenderer(std::move(im)));
}

bool GpuRenderer::render(const DrawList& list, Image8& out) {
    Impl& m = *impl_;
    if (!m.ensure_frame(list.width, list.height)) return false;
    std::vector<SDL_GPUTexture*> transient;
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(m.dev);
    if (!cb) return fail("SDL_AcquireGPUCommandBuffer");
    if (!m.record(cb, list, transient)) {
        SDL_CancelGPUCommandBuffer(cb);
        return false;
    }
    SDL_GPUTransferBuffer* tb = nullptr;
    if (!m.download(cb, m.frame, 1, tb)) return false;
    if (!m.finish(cb, tb, transient)) return false;
    out = Image8(m.width, m.height);
    const auto* p = static_cast<const std::uint8_t*>(SDL_MapGPUTransferBuffer(m.dev, tb, false));
    std::memcpy(out.pixels.data(), p, out.pixels.size());
    SDL_UnmapGPUTransferBuffer(m.dev, tb);
    SDL_ReleaseGPUTransferBuffer(m.dev, tb);
    return true;
}

bool GpuRenderer::render_rgba(const DrawList& list, const Palette& palette, std::vector<std::uint8_t>& rgba) {
    Impl& m = *impl_;
    if (!m.ensure_frame(list.width, list.height)) return false;
    std::uint8_t pal[256 * 4];
    for (int i = 0; i < 256; ++i) {
        pal[i * 4] = palette[static_cast<std::size_t>(i)].r;
        pal[i * 4 + 1] = palette[static_cast<std::size_t>(i)].g;
        pal[i * 4 + 2] = palette[static_cast<std::size_t>(i)].b;
        pal[i * 4 + 3] = 255;
    }
    if (!m.upload({{m.palette, 256, 1, 4, pal}})) return false;
    std::vector<SDL_GPUTexture*> transient;
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(m.dev);
    if (!cb) return fail("SDL_AcquireGPUCommandBuffer");
    if (!m.record(cb, list, transient)) {
        SDL_CancelGPUCommandBuffer(cb);
        return false;
    }
    SDL_GPUColorTargetInfo target{};
    target.texture = m.rgba;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cb, &target, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, m.resolve);
    const float vu[8] = {0, 0, static_cast<float>(m.width), static_cast<float>(m.height), static_cast<float>(m.width),
                         static_cast<float>(m.height), 0, 0};
    SDL_PushGPUVertexUniformData(cb, 0, vu, sizeof vu);
    const std::int32_t fu[4] = {1, 1, 0, 0};
    SDL_PushGPUFragmentUniformData(cb, 0, fu, sizeof fu);
    const SDL_GPUTextureSamplerBinding b[2] = {{m.frame, m.sampler}, {m.palette, m.sampler}};
    SDL_BindGPUFragmentSamplers(pass, 0, b, 2);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    SDL_GPUTransferBuffer* tb = nullptr;
    if (!m.download(cb, m.rgba, 4, tb)) return false;
    if (!m.finish(cb, tb, transient)) return false;
    rgba.resize(static_cast<std::size_t>(m.width) * static_cast<std::size_t>(m.height) * 4);
    const auto* p = static_cast<const std::uint8_t*>(SDL_MapGPUTransferBuffer(m.dev, tb, false));
    std::memcpy(rgba.data(), p, rgba.size());
    SDL_UnmapGPUTransferBuffer(m.dev, tb);
    SDL_ReleaseGPUTransferBuffer(m.dev, tb);
    return true;
}

bool GpuRenderer::present_window(const DrawList& list, const Palette& palette, int scale, const char* title) {
    Impl& m = *impl_;
    scale = std::max(scale, 1);
    if (!m.window) {
        m.window = SDL_CreateWindow(title, list.width * scale, list.height * scale, 0);
        if (!m.window) return fail("SDL_CreateWindow");
        if (!SDL_ClaimWindowForGPUDevice(m.dev, m.window)) return fail("SDL_ClaimWindowForGPUDevice");
        m.window_resolve = m.pipeline(m.quad_vs, m.resolve_fs, SDL_GetGPUSwapchainTextureFormat(m.dev, m.window));
        if (!m.window_resolve) return fail("SDL_CreateGPUGraphicsPipeline (window)");
    }
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
    if (!m.ensure_frame(list.width, list.height)) return false;
    std::uint8_t pal[256 * 4];
    for (int i = 0; i < 256; ++i) {
        pal[i * 4] = palette[static_cast<std::size_t>(i)].r;
        pal[i * 4 + 1] = palette[static_cast<std::size_t>(i)].g;
        pal[i * 4 + 2] = palette[static_cast<std::size_t>(i)].b;
        pal[i * 4 + 3] = 255;
    }
    if (!m.upload({{m.palette, 256, 1, 4, pal}})) return false;
    std::vector<SDL_GPUTexture*> transient;
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(m.dev);
    if (!cb) return fail("SDL_AcquireGPUCommandBuffer");
    if (!m.record(cb, list, transient)) {
        SDL_CancelGPUCommandBuffer(cb);
        return false;
    }
    SDL_GPUTexture* swap = nullptr;
    Uint32 sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cb, m.window, &swap, &sw, &sh)) {
        SDL_CancelGPUCommandBuffer(cb);
        return fail("SDL_WaitAndAcquireGPUSwapchainTexture");
    }
    if (swap) {
        SDL_GPUColorTargetInfo target{};
        target.texture = swap;
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        target.clear_color = {0, 0, 0, 1};
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cb, &target, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, m.window_resolve);
        const float w = static_cast<float>(list.width * scale), h = static_cast<float>(list.height * scale);
        const float vu[8] = {0, 0, w, h, static_cast<float>(sw), static_cast<float>(sh), 0, 0};
        SDL_PushGPUVertexUniformData(cb, 0, vu, sizeof vu);
        const std::int32_t fu[4] = {scale, scale, 0, 0};
        SDL_PushGPUFragmentUniformData(cb, 0, fu, sizeof fu);
        const SDL_GPUTextureSamplerBinding b[2] = {{m.frame, m.sampler}, {m.palette, m.sampler}};
        SDL_BindGPUFragmentSamplers(pass, 0, b, 2);
        SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    return m.finish(cb, nullptr, transient);
}

std::string GpuRenderer::driver() const {
    const char* d = SDL_GetGPUDeviceDriver(impl_->dev);
    return d ? d : "";
}

} // namespace render
