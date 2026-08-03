#pragma once

namespace mm::platform {

// Match the Steam Deck detection used by N64Recomp/RecompFrontend: Valve's
// board vendor is authoritative, while SteamDeck=1 supports wrappers and
// development/testing on another Linux machine.
bool is_steam_deck();

} // namespace mm::platform
