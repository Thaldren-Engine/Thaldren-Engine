// The texts the original's order behaviours pass to Unit_QueueSpeech (0x47F780) and
// MissionOrder_Acknowledge (0x438880); addresses of the strings in the original.

#include "ta/notice_texts.h"

namespace ta {

const char* speech_text(sim::SpeechText text) {
    using T = sim::SpeechText;
    switch (text) {
    case T::None: return nullptr;
    case T::RepairAborted: return "Repair aborted.";                                  // 0x5012DC
    case T::RepairsUnsuccessful: return "Repairs unsuccessful.";                      // 0x5016AC
    case T::UnitRepaired: return "Unit repaired";                                     // 0x5012CC
    case T::ConstructionStopped: return "Construction stopped";                       // 0x501340
    case T::UnableToCreate: return "Unable to create any more units";                 // 0x501310
    case T::StartingConstruction: return "Starting construction";                     // 0x5012F8
    case T::ConstructionTerminated: return "Construction terminated";                 // 0x5015A4
    case T::CantReachSite: return "I can't reach the construction site";              // 0x501580
    case T::WaitingForArea: return "Waiting for target area to clear";                // 0x50155C
    case T::AreaBlocked: return "Target area was blocked";                            // 0x501544
    case T::BuildingComplete: return "Building complete";                             // 0x501530
    case T::CantGetThere: return "I can't get there";                                 // 0x5015BC
    case T::Repairing: return "Repairing";                                            // 0x5010CC
    case T::ReclamationFailed: return "Reclamation failed";                           // 0x501638
    case T::CannotBeReclaimed: return "That unit cannot be reclaimed";                // 0x50164C
    case T::Reclaiming: return "Reclaiming";                                          // 0x501388
    case T::CaptureFailed: return "Capture failed";                                   // 0x5015D0
    case T::CannotBeCaptured: return "That unit cannot be captured";                  // 0x501618
    case T::CloudOfVapor: return "That unit is a cloud of vapor and cannot be captured";  // 0x5015E0
    case T::Capturing: return "Capturing";                                            // 0x5013B8
    case T::Guarding: return "Guarding";                                              // 0x5014E8
    case T::Attacking: return "Attacking";                                            // 0x50129C
    case T::Patrolling: return "Patrolling";                                          // 0x50145C
    case T::TransportFailed: return "Transport mission failed";                       // 0x5016D8
    case T::TooHeavy: return "Unit is too heavy to transport";                        // 0x501BDC
    case T::Loading: return "Loading";                                                // 0x50144C
    case T::PreparingTransport: return "Preparing for transport";                     // 0x501BC4
    case T::Unloading: return "Unloading";                                            // 0x501430
    case T::UnableToUnload: return "Unable to unload unit";                           // 0x501BFC
    case T::LandingAborted: return "Landing aborted";                                 // 0x501C8C
    case T::Landing: return "Landing";                                                // 0x501AE8
    case T::LandingFailed: return "Landing failed";                                   // 0x501C7C
    case T::PadsOccupied: return "Landing aborted: all pads are occupied";            // 0x501C54
    case T::NoPads: return "Landing aborted: no pads available";                      // 0x501C30
    case T::TooLarge: return "Unit is too large to transport";                        // 0x501714
    case T::LoadingUnit: return "Loading unit";                                       // 0x501704
    case T::UnloadingSlow: return "Unloading process is proceeding non-optimally";    // 0x501744
    case T::Building: return "Building";                                              // 0x501CC4
    case T::HostileTermination: return "Construction terminated by hostile action";   // 0x501CD0
    case T::RepairFailed: return "Repair mission failed";                             // 0x501CFC
    case T::ResurrectionFailed: return "Resurrection failed";                         // 0x501698
    case T::RessurectionFailed: return "Ressurection failed";                         // 0x501684
    case T::ResurrectionComplete: return "Resurrection complete";                     // 0x50166C
    }
    return nullptr;
}

}  // namespace ta
