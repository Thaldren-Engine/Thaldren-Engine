#include "sim/world.h"

namespace sim {

void World::step() {
    ++tick_;
    for (const Stage s : kStageOrder) {
        if ((s == Stage::NetInput || s == Stage::NetFlush) && !networked_) continue;
        auto& fn = stages_[static_cast<std::size_t>(s)];
        if (fn) fn(*this);
    }
}

} // namespace sim
