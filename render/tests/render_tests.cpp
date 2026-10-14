// Small checks of the renderer's integer rules.

#include <cstdio>

#include "render/battle_view.h"
#include "render/draw_list.h"
#include "render/model.h"
#include "render/raster.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
int count(const render::Image8& img, std::uint8_t v) {
    int n = 0;
    for (auto p : img.pixels) n += p == v;
    return n;
}
} // namespace

int main() {
    using namespace render;
    {
        // Axis-aligned square (1,1)-(5,5): rows 1..4 (bottom exclusive), columns 1..4 (right exclusive).
        Image8 img(10, 10, 0);
        RasterTarget t{img.pixels.data(), nullptr, img.width, img.bounds()};
        const RasterVertex v[4] = {{1, 1, 0}, {5, 1, 0}, {5, 5, 0}, {1, 5, 0}};
        fill_polygon(t, v, 4, 7);
        check(count(img, 7) == 16, "square covers 4x4 pixels");
        check(img.row(1)[1] == 7 && img.row(4)[4] == 7 && img.row(5)[5] == 0 && img.row(1)[5] == 0, "square corners");
    }
    {
        // The right clip drops the last column: a quad spanning the whole width leaves column w-1 untouched.
        Image8 img(8, 8, 0);
        RasterTarget t{img.pixels.data(), nullptr, img.width, img.bounds()};
        const RasterVertex v[4] = {{-3, 0, 0}, {20, 0, 0}, {20, 20, 0}, {-3, 20, 0}};
        fill_polygon(t, v, 4, 9);
        check(img.row(0)[7] == 0 && img.row(0)[6] == 9, "right clip quirk");
        check(img.row(7)[0] == 0 && img.row(6)[0] == 9, "bottom clip quirk");
    }
    {
        // Depth: a nearer (higher) polygon wins regardless of order.
        Image8 img(8, 8, 0);
        std::vector<std::uint8_t> depth(64, 0);
        RasterTarget t{img.pixels.data(), depth.data(), img.width, img.bounds()};
        const RasterVertex hi[4] = {{0, 0, 60}, {6, 0, 60}, {6, 6, 60}, {0, 6, 60}};
        const RasterVertex lo[4] = {{0, 0, 50}, {6, 0, 50}, {6, 6, 50}, {0, 6, 50}};
        fill_polygon(t, hi, 4, 3);
        fill_polygon(t, lo, 4, 4);
        check(img.row(2)[2] == 3, "depth test keeps the higher polygon");
    }
    {
        // Textured quad maps texel (0,0) at the top-left and copies the key colour as-is.
        Sprite8 tex;
        tex.width = tex.height = 2;
        tex.pixels = {10, 11, 12, 13};
        Image8 img(8, 8, 0);
        RasterTarget t{img.pixels.data(), nullptr, img.width, img.bounds()};
        const RasterVertex v[4] = {{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}};
        textured_quad(t, v, tex);
        check(img.row(0)[0] == 10, "texture origin");
    }
    {
        // Blend blit uses table[src * 256 + dst]; key pixels are skipped.
        BlendTable table(65536, 0);
        table[5 * 256 + 2] = 99;
        Sprite8 s;
        s.width = 2;
        s.height = 1;
        s.key = 1;
        s.pixels = {5, 1};
        Image8 img(4, 1, 2);
        blit_blend(img, img.bounds(), s, 1, 0, table);
        check(img.row(0)[1] == 99 && img.row(0)[2] == 2, "blend blit");
    }
    {
        // Lines converted to sprites give the same pixels as the reference line.
        DrawList list;
        list.width = 32;
        list.height = 32;
        list.line(3, 4, 27, 19, 200, {0, 0, 31, 31});
        list.line(30, 2, 1, 30, 0, {5, 5, 25, 25});
        const Image8 a = execute(list, {});
        lines_to_sprites(list);
        const Image8 b = execute(list, {});
        check(a.pixels == b.pixels, "line sprites match lines");
    }
    {
        // Sprite projection: x = floor(x), y = floor(-z) - (floor(y) >> 1), depth = floor(y) + bias.
        const std::int32_t p[3] = {(5 << 16) + 0x8000, -(3 << 16), -(7 << 16)};
        const RasterVertex r = project_sprite_point(p, 50);
        check(r.x == 5 && r.y == 7 - (-3 >> 1) && r.z == 47, "sprite projection");
    }
    {
        // Grey remap: a grey palette entry maps to itself; a colour to the nearest grey-ish one.
        Palette pal{};
        for (int i = 0; i < 256; ++i) pal[static_cast<std::size_t>(i)] = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i)};
        pal[200] = {255, 0, 0};  // grey 85
        const BlendTable g = gray_remap_table(pal);
        check(g[7 * 256 + 50] == 50 && g[0 * 256 + 200] == 85, "grey remap table");
        // Fog over a view: 32 x 32 blocks; everything unexplored but the block under the view's
        // second cell row and column; one unseen block. Black cells fill, grey cells remap.
        FogOfWar fog;
        fog.blocks_x = fog.blocks_z = 40;
        fog.unexplored.assign(1600, 1);
        fog.unseen.assign(1600, 0);
        for (int z = 0; z < 8; ++z)
            for (int x = 0; x < 8; ++x) fog.unexplored[static_cast<std::size_t>(z * 40 + x)] = 0;
        for (int z = 0; z < 8; ++z)
            for (int x = 0; x < 8; ++x) fog.unseen[static_cast<std::size_t>(z * 40 + x)] = (x >= 2 && x <= 4 && z >= 2 && z <= 4) ? 1 : 0;
        fog.gray_table = &g;
        fog.fill = 0;
        BattleScene scene;
        scene.camera.scroll_x = 16;  // cells line up with the blocks' centres
        scene.camera.scroll_y = 16;
        scene.fog = &fog;
        scene.clear_index = 200;
        DrawList list = build_draw_list(scene);
        const Image8 img = execute(list, {});
        // Block (3, 3) and its neighbours are unseen: the cell between blocks (2..3, 2..3) is grey
        // (red 200 becomes grey 85); block (10, 10) is unexplored: black.
        const int gx = 128 + 3 * 32 - 16 + 8, gy = 32 + 3 * 32 - 16 + 8;
        check(img.row(gy)[gx] == 85, "fog: grey cell remapped");
        check(img.row(32 + 10 * 32 - 16)[128 + 10 * 32 - 16] == 0, "fog: unexplored cell black");
        check(img.row(32 + 6 * 32 - 16 + 8)[128 + 6 * 32 - 16 + 8] == 200, "fog: seen cell untouched");
    }
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
