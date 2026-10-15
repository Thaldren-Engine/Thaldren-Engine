#pragma once
// One machine's network session: its transport and the frame sender and receiver the battle
// room, the loading handshake and the battle share (the original keeps one of each for the
// whole game, so frames held back in the room are released by the battle's traffic).

#include "net/frames.h"
#include "net/transport.h"

namespace net {

struct NetSession {
    explicit NetSession(Transport& t, NetTiming timing = {}) : transport(t), sender(timing) {}
    Transport& transport;
    FrameSender sender;
    FrameReceiver receiver;
};

} // namespace net
