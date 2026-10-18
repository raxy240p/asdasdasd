// module/hooks/framestage_hook.hpp — CHLClient::FrameStageNotify inline hook.
//
// Per cs2-sdk.com skin-changer flow step 1: "Hook FrameStageNotify and work
// after the network update." Writes to item-view fields from PresentDetour
// race the game-thread render reads and were the cause of freeze-then-crash
// on the previous build. This hook fires on the engine's own frame-stage
// callback, so our writes land at a point where the game has published
// m_hMyWeapons for this tick but hasn't started rendering yet.
#pragma once

namespace cheat::hooks {

bool InstallFrameStageHook();
void UninstallFrameStageHook();
bool FrameStageInFlight();

} // namespace cheat::hooks
