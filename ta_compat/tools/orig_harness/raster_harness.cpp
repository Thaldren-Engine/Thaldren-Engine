// Differential test of polygon scan conversion: the original's flat depth
// polygon (0x004C1000) and depth-tested textured quad (0x004C8760), run inside
// a suspended TotalA.exe, against render::fill_polygon / render::textured_quad
// on random polygons, with and without a depth plane.
//
// Usage: raster_harness <TotalA.exe> [cases] [seed]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "remote.h"
#include "render/raster.h"

namespace {

using u32 = std::uint32_t;
constexpr u32 kFlatPoly = 0x004C1000;     // stdcall (frame*, int* xyz, int count, int colour)
constexpr u32 kTexturedQuad = 0x004C8760; // stdcall (frame*, texframe*, int* xyz[4], int* uv or 0)

// The frame header the routines take: u16 width, u16 height, ..., +0x10 pixels, +0x14 depth (or 0).
void frame_header(std::uint8_t* h, int w, int hgt, u32 pixels, u32 depth) {
    std::memset(h, 0, 0x18);
    h[0] = static_cast<std::uint8_t>(w);
    h[1] = static_cast<std::uint8_t>(w >> 8);
    h[2] = static_cast<std::uint8_t>(hgt);
    h[3] = static_cast<std::uint8_t>(hgt >> 8);
    std::memcpy(h + 0x10, &pixels, 4);
    std::memcpy(h + 0x14, &depth, 4);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: raster_harness <TotalA.exe> [cases] [seed]\n");
        return 2;
    }
    const int cases = argc > 2 ? std::atoi(argv[2]) : 2000;
    const unsigned seed = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 1u;
    harness::Remote r;
    if (!r.start(argv[1])) {
        std::fprintf(stderr, "cannot start %s\n", argv[1]);
        return 1;
    }
    constexpr int W = 72, H = 56, kTexMax = 64 * 64;
    const u32 frame = r.alloc(0x100), tex = r.alloc(0x100), pix = r.alloc(W * H), dep = r.alloc(W * H),
              tpix = r.alloc(kTexMax + 0x1000), verts = r.alloc(0x400);
    std::mt19937 rng(seed);
    auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    int flat_ok = 0, flat_bad = 0, quad_ok = 0, quad_bad = 0, quad_skipped = 0;
    for (int c = 0; c < cases; ++c) {
        const bool textured = (c & 1) != 0;
        const bool with_depth = (c & 2) != 0;
        std::vector<std::uint8_t> pixels(W * H), depth(W * H);
        for (auto& p : pixels) p = static_cast<std::uint8_t>(rnd(0, 255));
        for (auto& d : depth) d = static_cast<std::uint8_t>(rnd(0, 120));
        const int n = textured ? 4 : rnd(1, 7);
        std::vector<render::RasterVertex> v(static_cast<std::size_t>(n));
        const int spread = rnd(0, 3);
        for (auto& p : v) {
            if (spread == 0) p = {rnd(-30, W + 30), rnd(-30, H + 30), rnd(0, 140)};
            else if (spread == 1) p = {rnd(0, W - 1), rnd(0, H - 1), rnd(30, 90)};
            else p = {rnd(10, 40), rnd(10, 40), rnd(0, 255)};
        }
        if (spread == 3 && n >= 4) {  // a proper convex quad like a model face
            const int x = rnd(-10, W), y = rnd(-10, H), a = rnd(1, 30), b = rnd(1, 30), s = rnd(-8, 8);
            v[0] = {x, y, rnd(40, 80)};
            v[1] = {x + a, y + s, rnd(40, 80)};
            v[2] = {x + a - s, y + s + b, rnd(40, 80)};
            v[3] = {x - s, y + b, rnd(40, 80)};
        }
        std::vector<std::int32_t> xyz;
        for (const auto& p : v) xyz.insert(xyz.end(), {p.x, p.y, p.z});
        // Original.
        std::uint8_t hdr[0x18];
        frame_header(hdr, W, H, pix, with_depth ? dep : 0);
        r.write(frame, hdr, sizeof hdr);
        r.write(pix, pixels.data(), pixels.size());
        r.write(dep, depth.data(), depth.size());
        r.write(verts, xyz.data(), xyz.size() * 4);
        render::Sprite8 texture;
        const std::uint8_t colour = static_cast<std::uint8_t>(rnd(0, 255));
        bool oor = false;
        std::vector<std::uint8_t> mine = pixels, mine_depth = depth;
        render::RasterTarget t{mine.data(), with_depth ? mine_depth.data() : nullptr, W, {0, 0, W - 1, H - 1}};
        if (textured) {
            static const int sizes[] = {8, 16, 32, 64, 5, 13, 24, 40};
            texture.width = sizes[rnd(0, 7)];
            texture.height = rnd(1, 64);
            texture.pixels.resize(static_cast<std::size_t>(texture.width * texture.height));
            for (auto& p : texture.pixels) p = static_cast<std::uint8_t>(rnd(0, 255));
            std::uint8_t th[0x18];
            frame_header(th, texture.width, texture.height, tpix, 0);
            r.write(tex, th, sizeof th);
            r.write(tpix, texture.pixels.data(), texture.pixels.size());
            if (!r.call(kTexturedQuad, 0, {frame, tex, verts, 0})) {
                std::fprintf(stderr, "case %d: textured call failed\n", c);
                return 1;
            }
            render::textured_quad(t, v.data(), texture, &oor);
        } else {
            if (!r.call(kFlatPoly, 0, {frame, verts, static_cast<u32>(n), colour})) {
                std::fprintf(stderr, "case %d: flat call failed\n", c);
                return 1;
            }
            render::fill_polygon(t, v.data(), n, colour);
        }
        std::vector<std::uint8_t> theirs(W * H), theirs_depth(W * H);
        r.read(pix, theirs.data(), theirs.size());
        r.read(dep, theirs_depth.data(), theirs_depth.size());
        if (textured && oor) {
            ++quad_skipped;
            continue;
        }
        const bool same = theirs == mine && (!with_depth || theirs_depth == mine_depth);
        int& ok = textured ? quad_ok : flat_ok;
        int& bad = textured ? quad_bad : flat_bad;
        if (same) {
            ++ok;
        } else {
            if (bad < 5) {
                int diff = 0;
                for (int i = 0; i < W * H; ++i) diff += theirs[static_cast<std::size_t>(i)] != mine[static_cast<std::size_t>(i)];
                std::printf("case %d (%s, n=%d, depth=%d): %d pixels differ; verts", c, textured ? "textured" : "flat", n, with_depth, diff);
                for (const auto& p : v) std::printf(" (%d,%d,%d)", p.x, p.y, p.z);
                std::printf("\n");
            }
            ++bad;
        }
    }
    std::printf("flat polygons: %d identical, %d different; textured quads: %d identical, %d different, %d skipped (texel outside the texture)\n",
                flat_ok, flat_bad, quad_ok, quad_bad, quad_skipped);
    return flat_bad || quad_bad ? 1 : 0;
}
