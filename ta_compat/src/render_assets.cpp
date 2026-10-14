#include "ta/render_assets.h"

#include <algorithm>
#include <cstring>

#include "ta/gaf.h"
#include "ta/tdf.h"

namespace ta {
namespace {

template <class T>
bool rd(const std::vector<std::uint8_t>& f, std::size_t off, T& v) {
    if (off + sizeof(T) > f.size()) return false;
    std::memcpy(&v, &f[off], sizeof(T));
    return true;
}

// One GAF frame header (24 bytes): u16 width, u16 height, i16 x offset,
// i16 y offset, u8 transparency index, u8 compressed, u16 layer count,
// u32 unused, u32 data offset, u32 unused.
bool decode_frame(const std::vector<std::uint8_t>& f, std::uint32_t at, render::Sprite8& out, int depth) {
    if (depth > 4) return false;
    std::uint16_t w = 0, h = 0, layers = 0;
    std::int16_t xo = 0, yo = 0;
    std::uint8_t key = 0, comp = 0;
    std::uint32_t data = 0;
    if (!rd(f, at, w) || !rd(f, at + 2, h) || !rd(f, at + 4, xo) || !rd(f, at + 6, yo) || !rd(f, at + 8, key) ||
        !rd(f, at + 9, comp) || !rd(f, at + 10, layers) || !rd(f, at + 16, data))
        return false;
    if (layers) {
        // Layered frame: decode each layer, then draw them in order onto their union.
        std::vector<render::Sprite8> parts;
        for (std::uint16_t i = 0; i < layers; ++i) {
            std::uint32_t lp = 0;
            if (!rd(f, data + i * 4u, lp)) return false;
            render::Sprite8 s;
            if (!decode_frame(f, lp, s, depth + 1)) return false;
            parts.push_back(std::move(s));
        }
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool first = true;
        for (const auto& s : parts) {
            const int l = -s.x_offset, t = -s.y_offset;
            if (first) {
                x0 = l, y0 = t, x1 = l + s.width, y1 = t + s.height;
                first = false;
            } else {
                x0 = std::min(x0, l), y0 = std::min(y0, t), x1 = std::max(x1, l + s.width), y1 = std::max(y1, t + s.height);
            }
        }
        render::Image8 img(x1 - x0, y1 - y0, key);
        for (const auto& s : parts) render::blit(img, img.bounds(), s, -x0, -y0);
        out.width = img.width;
        out.height = img.height;
        out.x_offset = -x0;
        out.y_offset = -y0;
        out.key = key;
        out.pixels = std::move(img.pixels);
        return true;
    }
    out.width = w;
    out.height = h;
    out.x_offset = xo;
    out.y_offset = yo;
    out.key = key;
    out.has_key = true;
    out.pixels.assign(static_cast<std::size_t>(w) * h, key);
    if (!comp) {
        if (static_cast<std::size_t>(data) + out.pixels.size() > f.size()) return false;
        std::memcpy(out.pixels.data(), &f[data], out.pixels.size());
        return true;
    }
    // Compressed rows: u16 byte count, then codes. Code bit 0: skip (code >> 1)
    // transparent pixels; bit 1: repeat the next byte (code >> 2) + 1 times;
    // otherwise copy the next (code >> 2) + 1 bytes.
    std::size_t p = data;
    for (int y = 0; y < h; ++y) {
        std::uint16_t n = 0;
        if (!rd(f, p, n)) return false;
        std::size_t q = p + 2;
        const std::size_t end = q + n;
        if (end > f.size()) return false;
        int x = 0;
        std::uint8_t* row = out.pixels.data() + static_cast<std::size_t>(y) * w;
        while (q < end && x < w) {
            const std::uint8_t c = f[q++];
            if (c & 1) {
                x += c >> 1;
            } else if (c & 2) {
                const int count = (c >> 2) + 1;
                const std::uint8_t v = q < end ? f[q++] : key;
                for (int k = 0; k < count && x < w; ++k) row[x++] = v;
            } else {
                const int count = (c >> 2) + 1;
                for (int k = 0; k < count && q < end; ++k, ++x, ++q)
                    if (x < w) row[x] = f[q];
            }
        }
        p = end;
    }
    return true;
}

} // namespace

bool load_palette(Vfs& vfs, render::Palette& out) {
    std::vector<std::uint8_t> d;
    if (!vfs.read("palettes\\PALETTE.PAL", d) || d.size() < 1024) return false;
    for (int i = 0; i < 256; ++i) out[static_cast<std::size_t>(i)] = {d[i * 4u], d[i * 4u + 1], d[i * 4u + 2]};
    return true;
}

bool load_blend_table(Vfs& vfs, render::BlendTable& out) {
    std::vector<std::uint8_t> d;
    if (!vfs.read("palettes\\PALETTE.ALP", d) || d.size() < 65536) return false;
    out.assign(d.begin(), d.begin() + 65536);
    return true;
}

bool gaf_sequence_frames(const std::vector<std::uint8_t>& gaf, int seq, std::vector<render::Sprite8>& out) {
    out.clear();
    std::uint32_t entry = 0;
    if (seq < 0 || !rd(gaf, 12 + static_cast<std::size_t>(seq) * 4, entry)) return false;
    std::uint16_t frames = 0;
    if (!rd(gaf, entry, frames)) return false;
    for (std::uint16_t i = 0; i < frames; ++i) {
        std::uint32_t fp = 0;
        if (!rd(gaf, entry + 40 + i * 8u, fp)) return false;
        render::Sprite8 s;
        if (!decode_frame(gaf, fp, s, 0)) return false;
        out.push_back(std::move(s));
    }
    return true;
}

bool tnt_load_tiles(const std::vector<std::uint8_t>& tnt, TntTiles& out) {
    std::uint32_t h[8] = {};
    for (int i = 0; i < 8; ++i)
        if (!rd(tnt, static_cast<std::size_t>(i) * 4, h[i])) return false;
    if (h[0] != 0x2000) return false;
    out.width = static_cast<int>(h[1] / 2);
    out.height = static_cast<int>(h[2] / 2);
    const std::size_t n = static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height);
    if (static_cast<std::size_t>(h[3]) + n * 2 > tnt.size()) return false;
    out.map.resize(n);
    std::memcpy(out.map.data(), &tnt[h[3]], n * 2);
    const std::uint32_t count = h[6];
    if (static_cast<std::size_t>(h[5]) + static_cast<std::size_t>(count) * 1024 > tnt.size()) return false;
    out.tiles.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        render::Sprite8& s = out.tiles[i];
        s.width = s.height = 32;
        s.has_key = false;
        s.pixels.assign(tnt.begin() + static_cast<std::ptrdiff_t>(h[5] + i * 1024u),
                        tnt.begin() + static_cast<std::ptrdiff_t>(h[5] + i * 1024u + 1024u));
    }
    return true;
}

bool tnt_feature_cells(const std::vector<std::uint8_t>& tnt, std::vector<TntFeatureCell>& out) {
    out.clear();
    std::uint32_t h[5] = {};
    for (int i = 0; i < 5; ++i)
        if (!rd(tnt, static_cast<std::size_t>(i) * 4, h[i])) return false;
    if (h[0] != 0x2000) return false;
    const std::size_t cells = static_cast<std::size_t>(h[1]) * h[2];
    if (static_cast<std::size_t>(h[4]) + cells * 4 > tnt.size()) return false;
    for (std::uint32_t z = 0; z < h[2]; ++z)
        for (std::uint32_t x = 0; x < h[1]; ++x) {
            std::uint16_t f = 0;
            rd(tnt, h[4] + (static_cast<std::size_t>(z) * h[1] + x) * 4 + 1, f);
            if (f < 0xFFFB) out.push_back({static_cast<int>(x), static_cast<int>(z), f});
        }
    return true;
}

void TextureLibrary::load(Vfs& vfs) {
    gafs_.clear();
    cache_.clear();
    auto add = [&](const std::string& path) {
        Gaf g;
        if (!vfs.read(path, g.data)) return false;
        GafDirectory dir;
        if (!gaf_read_directory(g.data, dir)) return false;
        g.names = dir.sequence_names;
        gafs_.push_back(std::move(g));
        return true;
    };
    for (const std::string& name : vfs.enumerate("textures\\*.GAF")) {
        if (stricmp_c(name, "logos.GAF") == 0) continue;
        add("textures\\" + name);
    }
    has_logos_ = add("textures\\logos.GAF");
}

TextureLibrary::Hit TextureLibrary::find(const std::string& name) {
    Hit hit;
    for (std::size_t g = 0; g < gafs_.size(); ++g) {
        const bool logos = has_logos_ && g + 1 == gafs_.size();
        for (std::size_t s = 0; s < gafs_[g].names.size(); ++s) {
            if (stricmp_c(gafs_[g].names[s], name) != 0) continue;
            auto key = std::make_pair(static_cast<int>(g), static_cast<int>(s));
            auto& slot = cache_[key];
            if (!slot) {
                slot = std::make_unique<render::ModelTexture>();
                gaf_sequence_frames(gafs_[g].data, static_cast<int>(s), slot->frames);
            }
            std::uint16_t frames = 0;
            std::uint32_t entry = 0;
            rd(gafs_[g].data, 12 + s * 4, entry);
            rd(gafs_[g].data, entry, frames);
            slot->animated = frames >= 2;
            slot->team_coloured = logos && frames == 10;
            hit.texture = slot.get();
            hit.from_logos = logos;
            hit.frames = frames;
            return hit;
        }
    }
    return hit;
}

namespace {

void add_pieces(const Model3doPiece* p, TextureLibrary& lib, render::Model& m) {
    const std::size_t index = m.pieces.size();
    m.pieces.emplace_back();
    {
        render::ModelPiece piece;
        piece.first_drawn = p->selection_primitive != -1 ? 1 : 0;
        for (const Model3doPrimitive& src : p->primitives) {
            render::ModelPrimitive prim;
            prim.indices = src.indices;
            std::uint32_t flags = 0, name_off = 0;
            std::memcpy(&flags, &src.raw[0x1C], 4);
            std::memcpy(&name_off, &src.raw[0x10], 4);
            std::int32_t colour = src.color_index;
            if (name_off != 0) {
                flags &= ~4u;
                TextureLibrary::Hit hit = lib.find(src.texture);
                if (hit.texture && hit.from_logos && hit.frames == 10) flags |= 4u;
                if (hit.texture) {
                    if (hit.frames < 2) flags &= ~2u;
                    else flags |= 2u;
                    prim.texture = hit.texture;
                } else {
                    colour = 0xD1;
                    flags |= 1u;
                }
            }
            prim.flat = (flags & 1u) != 0;
            prim.colour = static_cast<std::uint8_t>(colour);
            piece.primitives.push_back(std::move(prim));
        }
        m.pieces[index] = std::move(piece);
    }
    if (p->child) add_pieces(p->child.get(), lib, m);
    if (p->sibling) add_pieces(p->sibling.get(), lib, m);
}

} // namespace

render::Model build_render_model(const Model3doPiece* root, TextureLibrary& textures) {
    render::Model m;
    if (root) add_pieces(root, textures, m);
    return m;
}

} // namespace ta
