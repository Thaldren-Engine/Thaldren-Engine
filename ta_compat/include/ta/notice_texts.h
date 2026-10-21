// The console texts the original's order behaviours hand to a unit notice (spec:
// docs/RE_MEMORY.md, "Client", "Unit notices (exact)": Unit_QueueSpeech 0x47F780 uses the
// caller's text, when it gives one, in place of the notice table's text).
#pragma once

#include "sim/missions.h"

namespace ta {

// The original's English text for a behaviour's notice text (null for SpeechText::None: the
// notice keeps its table text). Spelling as in the original (0x5010CC .. 0x501CFC).
const char* speech_text(sim::SpeechText text);

}  // namespace ta
