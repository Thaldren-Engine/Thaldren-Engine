#include "sim/pose.h"

#include <cstdlib>
#include <utility>

#include "sim/x87_trig.h"

namespace sim {
namespace {

int iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
        if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

std::int32_t add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

} // namespace

int PoseState::clone(int node, int parent) {
    const PoseModelNode& n = model_->nodes[static_cast<std::size_t>(node)];
    const int index = static_cast<int>(pieces.size());
    pieces.emplace_back();
    {
        PosePiece& p = pieces.back();
        p.node = node;
        p.points.reserve(n.vertices.size() * 3);
        for (const auto& v : n.vertices) p.points.insert(p.points.end(), v.begin(), v.end());
        p.flags = pose_flags::kCached | pose_flags::kShaded | (n.vertices.size() >= 3 ? pose_flags::kVisible : 0);
    }
    int child = -1, sibling = -1;
    if (n.child >= 0) child = clone(n.child, index);
    if (n.sibling >= 0) sibling = clone(n.sibling, parent);
    PosePiece& p = pieces[static_cast<std::size_t>(index)];
    p.child = child;
    p.sibling = sibling;
    p.parent = parent;
    return index;
}

int PoseState::link(int node, int parent) {
    // The piece is found by searching from the last index down.
    int index = static_cast<int>(pieces.size()) - 1;
    while (index >= 0 && pieces[static_cast<std::size_t>(index)].node != node) --index;
    if (index < 0) return -1;
    const PoseModelNode& n = model_->nodes[static_cast<std::size_t>(node)];
    pieces[static_cast<std::size_t>(index)].sibling = n.sibling >= 0 ? link(n.sibling, parent) : -1;
    pieces[static_cast<std::size_t>(index)].child = n.child >= 0 ? link(n.child, index) : -1;
    pieces[static_cast<std::size_t>(index)].parent = parent;
    return index;
}

void PoseState::build(const PoseModel& model, const std::vector<std::string>& script_pieces) {
    model_ = &model;
    pieces.clear();
    cache_draw_count = 0;
    base_angle = {};
    fault_.clear();
    root = model.root >= 0 ? clone(model.root, -1) : -1;
    anim_dirty = 1;
    // Piece i becomes the first piece at index >= i whose model name matches script piece i.
    for (std::size_t i = 0; i < script_pieces.size(); ++i) {
        for (std::size_t j = i; j < pieces.size(); ++j) {
            if (iequal(script_pieces[i], model.nodes[static_cast<std::size_t>(pieces[j].node)].name)) {
                if (j != i) std::swap(pieces[i], pieces[j]);
                break;
            }
        }
    }
    root = model.root >= 0 ? link(model.root, -1) : -1;
}

PosePiece* PoseState::at(std::int32_t piece, std::int32_t axis) {
    if (piece < 0 || static_cast<std::size_t>(piece) >= pieces.size() || axis < 0 || axis > 2) {
        if (fault_.empty()) fault_ = "piece or axis out of range (the original corrupts memory)";
        return nullptr;
    }
    return &pieces[static_cast<std::size_t>(piece)];
}

void PoseState::set_move(std::int32_t piece, std::int32_t axis, std::int32_t value) {
    PosePiece* p = at(piece, axis);
    if (!p || p->move[static_cast<std::size_t>(axis)] == value) return;
    p->move[static_cast<std::size_t>(axis)] = value;
    p->skip_transform = 0;
    anim_dirty = 1;
    if (p->flags & pose_flags::kCached) cache_draw_count = 0;
}

void PoseState::set_turn(std::int32_t piece, std::int32_t axis, std::int16_t value) {
    PosePiece* p = at(piece, axis);
    if (!p || p->turn[static_cast<std::size_t>(axis)] == value) return;
    p->turn[static_cast<std::size_t>(axis)] = value;
    p->skip_transform = 0;
    anim_dirty = 1;
    if (p->flags & pose_flags::kCached) cache_draw_count = 0;
}

void PoseState::set_visible(std::int32_t piece, std::uint32_t on) {
    PosePiece* p = at(piece, 0);
    if (!p || static_cast<std::uint32_t>(p->flags & pose_flags::kVisible) == on) return;
    p->flags = static_cast<std::uint16_t>(((on ^ p->flags) & pose_flags::kVisible) ^ p->flags);
    p->skip_transform = 0;
    if (p->flags & pose_flags::kCached) cache_draw_count = 0;
}

void PoseState::set_cached(std::int32_t piece, std::uint32_t on) {
    PosePiece* p = at(piece, 0);
    if (!p) return;
    p->flags = static_cast<std::uint16_t>((p->flags & ~pose_flags::kCached) | ((on & 1u) << 1));
}

void PoseState::set_shaded(std::int32_t piece, std::uint32_t on) {
    PosePiece* p = at(piece, 0);
    if (!p) return;
    p->flags = static_cast<std::uint16_t>((p->flags & ~pose_flags::kShaded) | ((on & 1u) << 2));
}

std::int32_t PoseState::get_move(std::int32_t piece, std::int32_t axis) {
    PosePiece* p = at(piece, axis);
    return p ? p->move[static_cast<std::size_t>(axis)] : 0;
}

std::uint16_t PoseState::get_turn(std::int32_t piece, std::int32_t axis) {
    PosePiece* p = at(piece, axis);
    return p ? static_cast<std::uint16_t>(p->turn[static_cast<std::size_t>(axis)]) : 0;
}

int PoseState::reset_tree(int piece, int force) {
    int result = 0;
    for (int i = piece; i >= 0; i = pieces[static_cast<std::size_t>(i)].sibling) {
        PosePiece& p = pieces[static_cast<std::size_t>(i)];
        if (p.skip_transform == 0 || force != 0) {
            const auto& n = model_->nodes[static_cast<std::size_t>(p.node)];
            for (std::size_t v = 0; v < n.vertices.size(); ++v)
                for (int c = 0; c < 3; ++c) p.points[v * 3 + static_cast<std::size_t>(c)] = n.vertices[v][static_cast<std::size_t>(c)];
            result = 1;
            p.xform = {};
            p.skip_transform = 0;
        } else {
            result = 0;
        }
        if (p.child >= 0) result = reset_tree(p.child, result);
    }
    return result;
}

void PoseState::rotate_translate(int piece, const std::int16_t angles[3], const std::int32_t translate[3], bool walk_siblings) {
    for (int i = piece; i >= 0;) {
        PosePiece& p = pieces[static_cast<std::size_t>(i)];
        if (p.skip_transform == 0) {
            x87::rotate_point_3d(p.xform.data(), p.xform.data(), angles);
            const std::size_t count = p.points.size() / 3;
            for (std::size_t v = count; v-- > 0;) x87::rotate_point_3d(&p.points[v * 3], &p.points[v * 3], angles);
            for (int c = 0; c < 3; ++c) p.xform[static_cast<std::size_t>(c)] = add(p.xform[static_cast<std::size_t>(c)], translate[c]);
            for (std::size_t v = count; v-- > 0;)
                for (int c = 0; c < 3; ++c) p.points[v * 3 + static_cast<std::size_t>(c)] = add(p.points[v * 3 + static_cast<std::size_t>(c)], translate[c]);
        }
        if (p.child >= 0) rotate_translate(p.child, angles, translate, true);
        if (!walk_siblings) break;
        i = pieces[static_cast<std::size_t>(i)].sibling;
    }
}

void PoseState::update_transforms(int piece, bool recurse_siblings) {
    PosePiece& p = pieces[static_cast<std::size_t>(piece)];
    if (p.child >= 0) update_transforms(p.child, true);
    std::int16_t angles[3] = {p.turn[2], p.turn[1], p.turn[0]};  // z, y, x
    if (!recurse_siblings) {
        for (int k = 0; k < 3; ++k)
            angles[k] = static_cast<std::int16_t>(angles[k] + base_angle[static_cast<std::size_t>(k)]);
    }
    const auto& n = model_->nodes[static_cast<std::size_t>(p.node)];
    const std::int32_t translate[3] = {add(p.move[0], n.offset[0]), add(p.move[1], n.offset[1]), add(p.move[2], n.offset[2])};
    rotate_translate(piece, angles, translate, false);
    if (recurse_siblings && pieces[static_cast<std::size_t>(piece)].sibling >= 0)
        update_transforms(pieces[static_cast<std::size_t>(piece)].sibling, true);
}

void PoseState::sync(std::int16_t pitch, std::uint16_t heading, std::int16_t bank) {
    if (root < 0) return;
    auto far = [](std::int16_t a, std::int16_t b) { return std::abs(static_cast<std::int16_t>(a - b)) > 7; };
    if (far(base_angle[2], bank) || far(base_angle[1], static_cast<std::int16_t>(heading)) || far(base_angle[0], pitch)) {
        base_angle[0] = pitch;
        base_angle[1] = static_cast<std::int16_t>(heading);
        base_angle[2] = bank;
        anim_dirty = 1;
        PosePiece& r = pieces[static_cast<std::size_t>(root)];
        r.skip_transform = 0;
        if (r.flags & pose_flags::kCached) cache_draw_count = 0;
    }
    if (anim_dirty == 0) return;
    PosePiece& r = pieces[static_cast<std::size_t>(root)];
    int force = 0;
    if (r.skip_transform == 0) {
        force = 1;
        const auto& n = model_->nodes[static_cast<std::size_t>(r.node)];
        for (std::size_t v = 0; v < n.vertices.size(); ++v)
            for (int c = 0; c < 3; ++c) r.points[v * 3 + static_cast<std::size_t>(c)] = n.vertices[v][static_cast<std::size_t>(c)];
        r.xform = {};
        r.skip_transform = 0;
    }
    if (r.child >= 0) reset_tree(r.child, force);
    if (pieces[static_cast<std::size_t>(root)].sibling >= 0) reset_tree(pieces[static_cast<std::size_t>(root)].sibling, 0);
    update_transforms(root, false);
    anim_dirty = 0;
}

} // namespace sim
