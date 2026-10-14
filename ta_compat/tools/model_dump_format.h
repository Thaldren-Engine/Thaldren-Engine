#pragma once
// Serializes a 3DO piece tree in the same layout as ta_capture's dump_model,
// so a loaded model can be compared byte for byte with the live game's.

#include <cstdint>
#include <vector>

#include "ta/model3do.h"

namespace tools {

inline void put_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

// Per piece: u32 1, name\0, 7 x u32 (version, vertex count, primitive count, selection
// primitive, offset x/y/z), vertices, per primitive {u32 colour, u32 count, u16 indices},
// then the child chain; u32 0 ends each sibling chain.
inline void serialize_model(std::vector<std::uint8_t>& out, const ta::Model3doPiece* p) {
    for (; p; p = p->sibling.get()) {
        put_u32(out, 1);
        out.insert(out.end(), p->name.begin(), p->name.end());
        out.push_back(0);
        for (std::uint32_t v : {static_cast<std::uint32_t>(p->version), static_cast<std::uint32_t>(p->vertices.size()),
                                static_cast<std::uint32_t>(p->primitives.size()), static_cast<std::uint32_t>(p->selection_primitive),
                                static_cast<std::uint32_t>(p->offset[0]), static_cast<std::uint32_t>(p->offset[1]),
                                static_cast<std::uint32_t>(p->offset[2])}) {
            put_u32(out, v);
        }
        for (const auto& vx : p->vertices)
            for (std::int32_t c : vx) put_u32(out, static_cast<std::uint32_t>(c));
        for (const auto& pr : p->primitives) {
            put_u32(out, static_cast<std::uint32_t>(pr.color_index));
            put_u32(out, static_cast<std::uint32_t>(pr.indices.size()));
            for (std::uint16_t i : pr.indices) {
                out.push_back(static_cast<std::uint8_t>(i));
                out.push_back(static_cast<std::uint8_t>(i >> 8));
            }
        }
        if (p->child) serialize_model(out, p->child.get());
    }
    put_u32(out, 0);
}

} // namespace tools
