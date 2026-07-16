// Host-side debug overlay: launcher-gated activation, paged keyboard/gamepad
// navigation, and game-thread stage warps. The renderer calls draw_overlay()
// inside RT64's post-present ImGui pass; the translated game calls
// mm_debug_menu_tick() from its state-dispatch hook so all RDRAM writes stay
// on the game thread.
#pragma once

#include <SDL2/SDL.h>

namespace mm::debug_menu {

void configure(bool enabled);

// Returns true when the event belongs to the overlay and should not be
// processed as a regular host/game hotkey.
bool handle_event(const SDL_Event& event);

// Called from the translated game's state dispatcher to publish live state and
// apply a queued warp on the game thread.
extern "C" void mm_debug_menu_tick(unsigned char* rdram);

#ifdef MM_HAS_GRAPHICS
void draw_overlay();
#endif

} // namespace mm::debug_menu
