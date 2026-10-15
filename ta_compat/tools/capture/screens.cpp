// Screen comparison tooling (Windows / 32-bit build next to ta_capture).
//
// The user's rule: the original game is never started as a whole (not even on a hidden
// desktop); frames of the original come from its own drawing routines called in a suspended
// process (see ta_compat/tools/orig_harness/gui_harness.cpp). This tool converts scenarios for
// the client and compares frames.
//
// Usage:
//   ta_screens convert --scenario <file> --script <out.txt>
//       Writes the ta_play script and prints "--frames N --shots a,b,c" and the shot names.
//   ta_screens compare <a.bmp> <b.bmp> [--diff <out.bmp>] [--mask x0,y0,x1,y1 ...]
//       Counts pixels whose palette index differ and pixels whose colour differ; --diff writes
//       a picture of the differences (white) over a darkened copy of the first.
//
// Scenario lines (one action each, '#' starts a comment):
//   key K [+shift|+ctrl|+alt]   K: a character, escape enter tab space pause left right up down f1..f12
//   click X Y [BUTTON]          BUTTON 1 left (default), 3 right
//   move X Y
//   wait N                      N frames
//   load MS                     battle loading (one frame for ta_play)
//   shot NAME
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

// --- frames -------------------------------------------------------------------------------
struct Frame {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;   // w × h indices, top-down
    std::uint8_t pal[256][3] = {};  // r, g, b
};

void put16(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}

bool write_bmp(const std::string& path, const Frame& f) {
    const std::uint32_t stride = (static_cast<std::uint32_t>(f.w) + 3u) & ~3u;
    const std::uint32_t size = stride * static_cast<std::uint32_t>(f.h), off = 14 + 40 + 1024;
    std::vector<std::uint8_t> b;
    b.push_back('B');
    b.push_back('M');
    put32(b, off + size);
    put32(b, 0);
    put32(b, off);
    put32(b, 40);
    put32(b, static_cast<std::uint32_t>(f.w));
    put32(b, static_cast<std::uint32_t>(f.h));
    put16(b, 1);
    put16(b, 8);
    put32(b, 0);
    put32(b, size);
    put32(b, 2835);
    put32(b, 2835);
    put32(b, 256);
    put32(b, 0);
    for (int i = 0; i < 256; ++i) {
        b.push_back(f.pal[i][2]);
        b.push_back(f.pal[i][1]);
        b.push_back(f.pal[i][0]);
        b.push_back(0);
    }
    for (int y = f.h - 1; y >= 0; --y) {
        const std::uint8_t* r = &f.px[static_cast<std::size_t>(y) * f.w];
        b.insert(b.end(), r, r + f.w);
        for (std::uint32_t k = static_cast<std::uint32_t>(f.w); k < stride; ++k) b.push_back(0);
    }
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::fwrite(b.data(), 1, b.size(), fp);
    std::fclose(fp);
    return true;
}

// Reads an 8-bit palettised BMP (as written above and by the client).
bool read_bmp(const std::string& path, Frame& f) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') return false;
    auto u32 = [&](std::size_t o) { return static_cast<std::uint32_t>(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24); };
    const std::uint32_t off = u32(10), hs = u32(14);
    const int w = static_cast<int>(u32(18));
    int h = static_cast<int>(u32(22));
    const int bpp = b[28] | b[29] << 8;
    if (bpp != 8 || w <= 0) return false;
    const bool top_down = h < 0;
    if (top_down) h = -h;
    std::uint32_t colours = u32(46);
    if (colours == 0) colours = 256;
    for (std::uint32_t i = 0; i < 256; ++i) {
        const std::size_t o = 14 + hs + i * 4;
        if (i < colours && o + 2 < b.size()) f.pal[i][0] = b[o + 2], f.pal[i][1] = b[o + 1], f.pal[i][2] = b[o];
    }
    const std::uint32_t stride = (static_cast<std::uint32_t>(w) + 3u) & ~3u;
    if (off + stride * static_cast<std::uint32_t>(h) > b.size()) return false;
    f.w = w;
    f.h = h;
    f.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        const int src = top_down ? y : h - 1 - y;
        std::memcpy(&f.px[static_cast<std::size_t>(y) * w], &b[off + static_cast<std::size_t>(src) * stride], static_cast<std::size_t>(w));
    }
    return true;
}

// --- scenario -----------------------------------------------------------------------------
struct Action {
    std::string op;  // key click move wait load shot
    std::string arg;
    int x = 0, y = 0, button = 1, n = 0;
    bool shift = false, ctrl = false, alt = false;
};

bool parse_scenario(const std::string& path, std::vector<Action>& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (const auto c = line.find('#'); c != std::string::npos) line.resize(c);
        std::istringstream ls(line);
        std::vector<std::string> w;
        for (std::string t; ls >> t;) w.push_back(t);
        if (w.empty()) continue;
        Action a;
        while (w.size() > 1 && w.back()[0] == '+') {
            a.shift |= w.back() == "+shift";
            a.ctrl |= w.back() == "+ctrl";
            a.alt |= w.back() == "+alt";
            w.pop_back();
        }
        a.op = w[0];
        auto num = [&](std::size_t i, int d) { return i < w.size() ? std::atoi(w[i].c_str()) : d; };
        if (a.op == "key" || a.op == "shot" || a.op == "res") a.arg = w.size() > 1 ? w[1] : "";
        else if (a.op == "click" || a.op == "move") a.x = num(1, 0), a.y = num(2, 0), a.button = num(3, 1);
        else if (a.op == "wait" || a.op == "load") a.n = num(1, 0);
        else {
            std::fprintf(stderr, "scenario: unknown action %s\n", a.op.c_str());
            return false;
        }
        out.push_back(a);
    }
    return true;
}

// Virtual key and character for a key name.
void key_codes(const std::string& k, WPARAM& vk, WPARAM& ch) {
    static const std::map<std::string, WPARAM> named = {{"escape", VK_ESCAPE}, {"enter", VK_RETURN}, {"tab", VK_TAB}, {"space", VK_SPACE},
                                                        {"pause", VK_PAUSE},   {"left", VK_LEFT},    {"right", VK_RIGHT}, {"up", VK_UP},
                                                        {"down", VK_DOWN},     {"backspace", VK_BACK}};
    ch = 0;
    if (auto it = named.find(k); it != named.end()) {
        vk = it->second;
        if (vk == VK_ESCAPE) ch = 27;
        if (vk == VK_RETURN) ch = 13;
        if (vk == VK_TAB) ch = 9;
        if (vk == VK_SPACE) ch = ' ';
        if (vk == VK_BACK) ch = 8;
        return;
    }
    if (k.size() >= 2 && (k[0] == 'f' || k[0] == 'F') && std::atoi(k.c_str() + 1) >= 1) {
        vk = VK_F1 + std::atoi(k.c_str() + 1) - 1;
        return;
    }
    const char c = k.empty() ? ' ' : k[0];
    const SHORT s = VkKeyScanA(c);
    vk = static_cast<WPARAM>(s & 0xFF);
    ch = static_cast<WPARAM>(static_cast<unsigned char>(c));
}

// --- converting a scenario for the client -------------------------------------------------
int convert(int argc, char** argv) {
    std::string scen, script;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scenario" && i + 1 < argc) scen = argv[++i];
        else if (a == "--script" && i + 1 < argc) script = argv[++i];
    }
    std::vector<Action> acts;
    if (scen.empty() || script.empty() || !parse_scenario(scen, acts)) {
        std::fprintf(stderr, "convert: need --scenario and --script\n");
        return 2;
    }
    FILE* fp = std::fopen(script.c_str(), "w");
    if (!fp) return 1;
    int f = 1;
    std::string shots, names;
    for (const Action& a : acts) {
        const std::string mods = std::string(a.shift ? " +shift" : "") + (a.ctrl ? " +ctrl" : "") + (a.alt ? " +alt" : "");
        if (a.op == "key") std::fprintf(fp, "%d key %s%s\n", f++, a.arg.c_str(), mods.c_str());
        else if (a.op == "click") std::fprintf(fp, "%d click %d %d %d%s\n", f++, a.x, a.y, a.button, mods.c_str());
        else if (a.op == "move") std::fprintf(fp, "%d move %d %d\n", f++, a.x, a.y);
        else if (a.op == "wait") f += a.n;
        else if (a.op == "load") f += 2;
        else if (a.op == "shot") {
            f += 2;
            shots += (shots.empty() ? "" : ",") + std::to_string(f);
            names += std::to_string(f) + "=" + a.arg + "\n";
            ++f;
        }
    }
    std::fclose(fp);
    std::printf("--frames %d --shots %s\n%s", f + 1, shots.c_str(), names.c_str());
    return 0;
}

// --- comparing two frames -----------------------------------------------------------------
int compare(int argc, char** argv) {
    if (argc < 4) return 2;
    Frame a, b;
    if (!read_bmp(argv[2], a) || !read_bmp(argv[3], b)) {
        std::fprintf(stderr, "compare: cannot read the pictures\n");
        return 2;
    }
    std::string diff_path;
    std::vector<RECT> masks;
    for (int i = 4; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--diff" && i + 1 < argc) diff_path = argv[++i];
        else if (s == "--mask" && i + 1 < argc) {
            RECT r{};
            std::sscanf(argv[++i], "%ld,%ld,%ld,%ld", &r.left, &r.top, &r.right, &r.bottom);
            masks.push_back(r);
        }
    }
    if (a.w != b.w || a.h != b.h) {
        std::printf("size differs: %dx%d vs %dx%d\n", a.w, a.h, b.w, b.h);
        return 1;
    }
    std::size_t idx = 0, rgb = 0, masked = 0;
    int x0 = a.w, y0 = a.h, x1 = -1, y1 = -1;
    Frame d = a;
    for (int i = 0; i < 256; ++i) d.pal[i][0] = d.pal[i][1] = d.pal[i][2] = static_cast<std::uint8_t>(i);  // greys
    d.pal[255][0] = d.pal[255][1] = d.pal[255][2] = 255;
    d.pal[254][0] = 255, d.pal[254][1] = 64, d.pal[254][2] = 64;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * a.w + x;
            const std::uint8_t pa = a.px[i], pb = b.px[i];
            const std::uint8_t* ca = a.pal[pa];
            const std::uint8_t* cb = b.pal[pb];
            // Darkened copy of the first: grey level from its brightness.
            d.px[i] = static_cast<std::uint8_t>((ca[0] + ca[1] + ca[2]) / 9);  // a third of the brightness
            bool in_mask = false;
            for (const RECT& r : masks) in_mask |= x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
            const bool di = pa != pb, dc = ca[0] != cb[0] || ca[1] != cb[1] || ca[2] != cb[2];
            if (in_mask) {
                masked += dc;
                continue;
            }
            idx += di;
            rgb += dc;
            if (dc) {
                d.px[i] = 255;
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            } else if (di) {
                d.px[i] = 254;  // same colour from another index
            }
        }
    std::printf("index differences %zu, colour differences %zu", idx, rgb);
    if (rgb) std::printf(", bounds (%d,%d)-(%d,%d)", x0, y0, x1, y1);
    if (!masks.empty()) std::printf(", %zu masked", masked);
    std::printf("\n");
    if (!diff_path.empty()) write_bmp(diff_path, d);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "convert") return convert(argc, argv);
    if (mode == "compare") return compare(argc, argv);
    std::fprintf(stderr, "usage: ta_screens run|convert|compare ... (see the source's header)\n");
    return 2;
}
