#include "steam_deck.h"

#include <cstdlib>
#include <fstream>
#include <string>

namespace mm::platform {

bool is_steam_deck() {
#if defined(__linux__)
    static const bool detected = [] {
        std::ifstream board_vendor_file(
            "/sys/devices/virtual/dmi/id/board_vendor");
        std::string board_vendor;
        if (std::getline(board_vendor_file, board_vendor) &&
            board_vendor == "Valve") {
            return true;
        }

        const char* steam_deck = std::getenv("SteamDeck");
        return steam_deck != nullptr && std::string{steam_deck} == "1";
    }();
    return detected;
#else
    return false;
#endif
}

} // namespace mm::platform
