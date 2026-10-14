#include "ta/model3do.h"

#include <cstring>

namespace ta {
namespace {

constexpr std::size_t kPieceSize = 0x34;
constexpr std::size_t kPrimSize = 0x20;

struct Reader {
    const std::vector<std::uint8_t>& f;
    std::string error;

    bool u32(std::size_t off, std::uint32_t& v) {
        if (off + 4 > f.size()) {
            error = "read past end of file";
            return false;
        }
        std::memcpy(&v, &f[off], 4);
        return true;
    }

    std::string cstr(std::uint32_t off) {
        std::string s;
        while (off < f.size() && f[off]) s += static_cast<char>(f[off++]);
        return s;
    }
};

std::int32_t average_y(const Model3doPiece& p, const Model3doPrimitive& prim, bool& div_zero) {
    std::uint32_t sum = 0;  // wraps like the original's 32-bit int
    for (std::uint16_t i : prim.indices) sum += static_cast<std::uint32_t>(i < p.vertices.size() ? p.vertices[i][1] : 0);
    if (prim.indices.empty()) {
        div_zero = true;  // the original divides by zero here
        return 0;
    }
    return static_cast<std::int32_t>(sum) / static_cast<std::int32_t>(prim.indices.size());
}

void sort_primitives(Model3doPiece& p, std::string& error) {
    auto& prims = p.primitives;
    if (p.selection_primitive != -1 && !prims.empty()) {
        if (static_cast<std::size_t>(p.selection_primitive) < prims.size()) {
            std::swap(prims[0], prims[static_cast<std::size_t>(p.selection_primitive)]);
        }
        p.selection_primitive = 0;
    }
    const std::size_t n = prims.size();
    bool swapped = true;
    while (swapped) {
        swapped = false;
        if (n < 3) break;
        for (std::size_t i = 1; i + 1 < n; ++i) {
            bool dz = false;
            const std::int32_t a = average_y(p, prims[i], dz);
            const std::int32_t b = average_y(p, prims[i + 1], dz);
            if (dz) {
                error = "primitive with no indices in sorted range (original would crash)";
                return;
            }
            if (b < a) {
                std::swap(prims[i], prims[i + 1]);
                swapped = true;
            }
        }
    }
}

std::unique_ptr<Model3doPiece> read_piece(Reader& r, std::uint32_t off, int depth) {
    if (depth > 256) {
        r.error = "piece tree too deep";
        return nullptr;
    }
    std::uint32_t h[13];
    for (int k = 0; k < 13; ++k)
        if (!r.u32(off + static_cast<std::size_t>(k) * 4, h[k])) return nullptr;
    auto p = std::make_unique<Model3doPiece>();
    p->version = static_cast<std::int32_t>(h[0]);
    const std::uint32_t nverts = h[1], nprims = h[2];
    p->selection_primitive = static_cast<std::int32_t>(h[3]);
    p->offset = {static_cast<std::int32_t>(h[4]), static_cast<std::int32_t>(h[5]), static_cast<std::int32_t>(h[6])};
    p->name = h[7] ? r.cstr(h[7]) : std::string();
    if (static_cast<std::size_t>(h[9]) + static_cast<std::size_t>(nverts) * 12 > r.f.size()) {
        r.error = "vertex data out of range";
        return nullptr;
    }
    p->vertices.resize(nverts);
    for (std::uint32_t v = 0; v < nverts; ++v) std::memcpy(p->vertices[v].data(), &r.f[h[9] + v * 12u], 12);
    if (static_cast<std::size_t>(h[10]) + static_cast<std::size_t>(nprims) * kPrimSize > r.f.size()) {
        r.error = "primitive data out of range";
        return nullptr;
    }
    p->primitives.resize(nprims);
    for (std::uint32_t k = 0; k < nprims; ++k) {
        Model3doPrimitive& pr = p->primitives[k];
        const std::size_t po = h[10] + k * kPrimSize;
        std::memcpy(pr.raw.data(), &r.f[po], kPrimSize);
        std::uint32_t color, nidx, idx_off, tex_off;
        std::memcpy(&color, &r.f[po], 4);
        std::memcpy(&nidx, &r.f[po + 4], 4);
        std::memcpy(&idx_off, &r.f[po + 12], 4);
        std::memcpy(&tex_off, &r.f[po + 16], 4);
        pr.color_index = static_cast<std::int32_t>(color);
        if (static_cast<std::size_t>(idx_off) + static_cast<std::size_t>(nidx) * 2 > r.f.size()) {
            r.error = "index data out of range";
            return nullptr;
        }
        pr.indices.resize(nidx);
        for (std::uint32_t i = 0; i < nidx; ++i) std::memcpy(&pr.indices[i], &r.f[idx_off + i * 2u], 2);
        pr.texture = tex_off ? r.cstr(tex_off) : std::string();
    }
    // The game relocates the sibling, then the child, then sorts this piece's primitives.
    if (h[11]) {
        p->sibling = read_piece(r, h[11], depth + 1);
        if (!r.error.empty()) return nullptr;
    }
    if (h[12]) {
        p->child = read_piece(r, h[12], depth + 1);
        if (!r.error.empty()) return nullptr;
    }
    sort_primitives(*p, r.error);
    return p;
}

void negate_xz(Model3doPiece* p) {
    for (; p; p = p->child.get()) {
        for (auto& v : p->vertices) {
            v[0] = static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(v[0]));
            v[2] = static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(v[2]));
        }
        p->offset[2] = static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(p->offset[2]));
        p->offset[0] = static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(p->offset[0]));
        if (p->sibling) negate_xz(p->sibling.get());
    }
}

} // namespace

Model3doLoadResult load_3do(const std::vector<std::uint8_t>& file, bool negate) {
    Model3doLoadResult res;
    if (file.size() < kPieceSize) {
        res.error = "file too small";
        return res;
    }
    Reader r{file, {}};
    res.root = read_piece(r, 0, 0);
    res.error = r.error;
    if (res.root && negate) negate_xz(res.root.get());
    return res;
}

std::int32_t model_height(const Model3doPiece* p) {
    std::int32_t best = 0;
    for (; p; p = p->sibling.get()) {
        for (const auto& v : p->vertices) {
            const std::int32_t y = static_cast<std::int32_t>(static_cast<std::uint32_t>(v[1]) + static_cast<std::uint32_t>(p->offset[1]));
            if (best < y) best = y;
        }
        if (p->child) {
            const std::int32_t h = static_cast<std::int32_t>(static_cast<std::uint32_t>(model_height(p->child.get())) +
                                                            static_cast<std::uint32_t>(p->offset[1]));
            if (best < h) best = h;
        }
    }
    return best;
}

int model_piece_count(const Model3doPiece* p) {
    if (!p) return 0;
    return 1 + model_piece_count(p->child.get()) + model_piece_count(p->sibling.get());
}

namespace {
int add_pose_nodes(const Model3doPiece* p, sim::PoseModel& m) {
    const int index = static_cast<int>(m.nodes.size());
    m.nodes.emplace_back();
    m.nodes.back().name = p->name;
    m.nodes.back().offset = p->offset;
    m.nodes.back().vertices = p->vertices;
    m.nodes.back().selection = p->selection_primitive;
    for (const Model3doPrimitive& src : p->primitives) {
        sim::PosePrimitive prim;
        prim.indices = src.indices;
        prim.flat = (src.raw[0x1C] & 1) != 0;  // the file's colour flag (texture binding can add it)
        m.nodes.back().primitives.push_back(std::move(prim));
    }
    const int child = p->child ? add_pose_nodes(p->child.get(), m) : -1;
    const int sibling = p->sibling ? add_pose_nodes(p->sibling.get(), m) : -1;
    m.nodes[static_cast<std::size_t>(index)].child = child;
    m.nodes[static_cast<std::size_t>(index)].sibling = sibling;
    return index;
}
} // namespace

sim::PoseModel to_pose_model(const Model3doPiece* root) {
    sim::PoseModel m;
    if (root) m.root = add_pose_nodes(root, m);
    return m;
}

bool selection_footprint(const std::vector<std::uint8_t>& file, const Model3doPiece* root,
                         std::array<std::array<std::int32_t, 2>, 4>& out) {
    if (!root || root->selection_primitive < 0 || root->primitives.empty()) return false;
    const auto& prim = root->primitives[static_cast<std::size_t>(root->selection_primitive)];
    std::uint32_t idx_off = 0, vert_off = 0;
    std::memcpy(&idx_off, &prim.raw[12], 4);
    if (file.size() >= kPieceSize) std::memcpy(&vert_off, &file[0x24], 4);
    auto rd32 = [&](std::size_t off) {
        std::uint32_t v = 0;
        if (off + 4 <= file.size()) std::memcpy(&v, &file[off], 4);
        return static_cast<std::int32_t>(v);
    };
    for (std::size_t k = 0; k < 4; ++k) {
        // The game reads four index slots even when the primitive has fewer.
        std::uint16_t idx = 0;
        if (idx_off + k * 2 + 2 <= file.size()) std::memcpy(&idx, &file[idx_off + k * 2], 2);
        if (idx < root->vertices.size()) {
            out[k] = {root->vertices[idx][0], root->vertices[idx][2]};
        } else {  // past the vertex list: the following file bytes, not negated
            out[k] = {rd32(vert_off + idx * 12u), rd32(vert_off + idx * 12u + 8)};
        }
    }
    return true;
}

} // namespace ta
