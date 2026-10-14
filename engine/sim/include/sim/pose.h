#pragma once
// Articulated model pose: per-piece script-driven move/turn offsets, visibility
// flags, and the lazily recomputed world-space piece transforms and vertex
// positions (see docs/RE_MEMORY.md, "Piece pose").

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sim {

// Static model tree (16.16 coordinates). Nodes may be stored in any order.
struct PoseModelNode {
    std::string name;
    std::array<std::int32_t, 3> offset{};               // from the parent
    std::vector<std::array<std::int32_t, 3>> vertices;
    int sibling = -1, child = -1;
};

struct PoseModel {
    std::vector<PoseModelNode> nodes;
    int root = -1;
};

namespace pose_flags {
constexpr std::uint16_t kVisible = 1, kCached = 2, kShaded = 4;
}

struct PosePiece {
    int node = -1;                                      // model node
    std::array<std::int32_t, 3> move{};                 // script translation
    std::array<std::int16_t, 3> turn{};                 // script rotation (x, y, z)
    std::array<std::int32_t, 3> xform{};                // transformed piece origin (unit space)
    std::vector<std::int32_t> points;                   // transformed vertices (x, y, z per vertex)
    std::int16_t skip_transform = 0;                    // never set by the simulation
    std::uint16_t flags = 0;
    int sibling = -1, child = -1, parent = -1;          // piece indices
};

class PoseState {
public:
    // Clones the model tree depth-first (node, its subtree, then its siblings),
    // reorders pieces so that piece i is the one named script_pieces[i] when
    // found, then relinks the tree.
    void build(const PoseModel& model, const std::vector<std::string>& script_pieces);

    // Script callbacks. An out-of-range piece sets fault() (the original corrupts memory).
    void set_move(std::int32_t piece, std::int32_t axis, std::int32_t value);
    void set_turn(std::int32_t piece, std::int32_t axis, std::int16_t value);
    void set_visible(std::int32_t piece, std::uint32_t on);
    void set_cached(std::int32_t piece, std::uint32_t on);
    void set_shaded(std::int32_t piece, std::uint32_t on);
    std::int32_t get_move(std::int32_t piece, std::int32_t axis);
    // Zero-extended: scripts see piece angles as 0..65535.
    std::uint16_t get_turn(std::int32_t piece, std::int32_t axis);

    // Brings base angles and transforms up to date for the unit's attitude:
    // base angles refresh when any differs by more than 7; transforms are
    // recomputed when dirty.
    void sync(std::int16_t pitch, std::uint16_t heading, std::int16_t bank);

    std::vector<PosePiece> pieces;
    std::int32_t cache_draw_count = 0;
    std::int32_t anim_dirty = 0;
    std::array<std::int16_t, 3> base_angle{};  // z (pitch), y (heading), x (bank)
    int root = -1;
    const std::string& fault() const { return fault_; }

private:
    int clone(int node, int parent);
    int link(int node, int parent);
    int reset_tree(int piece, int force);
    void update_transforms(int piece, bool recurse_siblings);
    void rotate_translate(int piece, const std::int16_t angles[3], const std::int32_t translate[3], bool walk_siblings);
    PosePiece* at(std::int32_t piece, std::int32_t axis);

    const PoseModel* model_ = nullptr;
    std::string fault_;
};

} // namespace sim
