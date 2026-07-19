#include "telemetry.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace {
int check(bool condition, const char* message) {
    if (condition) return 0;
    std::fprintf(stderr, "telemetry test failed: %s\n", message);
    return 1;
}
} // namespace

int main() {
    using namespace mm::telemetry;
    int result = 0;
    start(25);
    result |= check(running(), "reporter starts");
    set_phase(Phase::GameStarting, "unit test");
    record_vi();
    record_display_list(1200);
    record_screen_update(2300);
    std::this_thread::sleep_for(std::chrono::milliseconds(17));
    record_screen_update(2500);
    record_audio_task(true);
    set_audio_rates(32000, 48000);
    record_audio_buffer(600, 1400, 48000, 800, 20000, 0, 0, 1600, false);
    set_display_state(1920, 1080, 144, 4.0f);
    set_display_target(120);
    record_scene(6, 3, 12);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    result |= check(current_phase() == Phase::Running,
                    "scene transition marks running phase");
    stop();
    result |= check(!running(), "reporter stops");
    return result == 0 ? 0 : 1;
}
