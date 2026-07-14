#!/usr/bin/env bash
# Capture a short frame sequence from one progression stage. Unlike the full
# playable-stage sweep, this keeps rendering long enough to expose transient
# geometry loss, z-fighting, and actor pop-in.

set -u

usage() {
    echo "usage: $0 GAME_BINARY ROM OUTPUT_DIR STAGE_INDEX" >&2
    echo "env: MM_CAPTURE_FRAMES=12 MM_CAPTURE_INTERVAL=0.25 MM_CAPTURE_DELAY=60 MM_CAPTURE_WARMUP=0 MM_CAPTURE_CAMERA_X= MM_CAPTURE_EARLY=0 MM_CAPTURE_GEOMETRY=1602x1022+39+59 MM_WINDOW=1600x900 MM_WIDESCREEN=1 MM_AUTO_ADVANCE=1 MM_FAST_FORWARD=1 MM_TEST_MOVE=1|jet-right|jet-hop-right|jet-left|jet-bounce MM_TEST_ACTOR_TRACE=1 MM_TEST_STREAM_TRACE=1 MM_TEST_STREAM_CAMERA=x[,y]" >&2
}

if (( $# != 4 )); then
    usage
    exit 2
fi

game=$1
rom=$2
output_dir=$3
stage=$4

capture_frames=${MM_CAPTURE_FRAMES:-12}
capture_interval=${MM_CAPTURE_INTERVAL:-0.25}
capture_delay=${MM_CAPTURE_DELAY:-60}
capture_warmup=${MM_CAPTURE_WARMUP:-0}
capture_camera_x=${MM_CAPTURE_CAMERA_X:-}
capture_early=${MM_CAPTURE_EARLY:-0}
capture_geometry=${MM_CAPTURE_GEOMETRY:-1602x1022+39+59}
window_size=${MM_WINDOW:-1600x900}
widescreen=${MM_WIDESCREEN:-1}
auto_advance=${MM_AUTO_ADVANCE:-1}
fast_forward=${MM_FAST_FORWARD:-1}
test_move=${MM_TEST_MOVE:-1}
stream_trace=${MM_TEST_STREAM_TRACE:-0}
stream_camera=${MM_TEST_STREAM_CAMERA:-}

if [[ -n $capture_camera_x && ! $capture_camera_x =~ ^-?[0-9]+$ ]]; then
    echo "MM_CAPTURE_CAMERA_X must be an integer: $capture_camera_x" >&2
    exit 2
fi

if [[ ! -x "$game" ]]; then
    echo "not executable: $game" >&2
    exit 2
fi
if [[ ! -f "$rom" ]]; then
    echo "ROM not found: $rom" >&2
    exit 2
fi
if ! command -v magick >/dev/null; then
    echo "ImageMagick 'magick' is required" >&2
    exit 2
fi
if [[ -z ${DISPLAY:-} ]]; then
    echo "DISPLAY is not set; burst capture requires X11/XWayland" >&2
    exit 2
fi

mkdir -p "$output_dir"
log="$output_dir/stage-$(printf '%02d' "$stage").log"
args=("$game" "$rom" --window "$window_size")
if [[ $widescreen == 1 ]]; then
    args+=(--widescreen)
else
    args+=(--no-widescreen)
fi

setsid env MM_WARP_STAGE="$stage" MM_WARP_AT=1 MM_WARP_DELAY=1 \
    MM_TEST_AUTO_ADVANCE="$auto_advance" MM_TEST_MOVE="$test_move" \
    MM_TEST_STREAM_TRACE="$stream_trace" \
    MM_TEST_STREAM_CAMERA="$stream_camera" \
    MM_WIN_POS=40,40 SDL_VIDEODRIVER=x11 \
    "${args[@]}" >"$log" 2>&1 &
pid=$!
fast_window=
game_window=

find_game_window() {
    local window
    game_window=
    command -v xdotool >/dev/null || return 1
    window=$(xdotool search --onlyvisible --pid "$pid" 2>/dev/null | tail -1)
    if [[ -n $window ]] &&
        xdotool getwindowgeometry "$window" >/dev/null 2>&1; then
        game_window=$window
        return 0
    fi
    return 1
}

capture_game_frame() {
    local output=$1
    rm -f -- "$output"
    if [[ -n $game_window ]] &&
        magick "x:$game_window" +repage "$output" 2>/dev/null; then
        return 0
    fi
    if find_game_window &&
        magick "x:$game_window" +repage "$output" 2>/dev/null; then
        return 0
    fi
    game_window=
    magick x:root -crop "$capture_geometry" +repage "$output"
}

if [[ $fast_forward == 1 ]] && command -v xdotool >/dev/null; then
    for ((attempt = 0; attempt < 50; ++attempt)); do
        if find_game_window; then
            xdotool windowactivate --sync "$game_window" 2>/dev/null || true
            xdotool keydown --window "$game_window" Tab 2>/dev/null || true
            fast_window=$game_window
            break
        fi
        sleep 0.1
    done
fi

cleanup() {
    if [[ -n $fast_window ]]; then
        xdotool keyup --window "$fast_window" Tab 2>/dev/null || true
    fi
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL -- "-$pid" 2>/dev/null || true
    fi
    { wait "$pid"; } 2>/dev/null || true
}
trap cleanup EXIT INT TERM

is_fixed_4x3_stage() {
    case $1 in
        11|27|47|52|56|57) return 0 ;;
        *) return 1 ;;
    esac
}

ready=0
for ((second = 0; second < capture_delay; ++second)); do
    sleep 1
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "game exited before capture; see $log" >&2
        exit 1
    fi
    if ! grep -q "\[warp\] stage=$stage " "$log" 2>/dev/null; then
        continue
    fi
    if [[ $widescreen == 1 ]] && ! is_fixed_4x3_stage "$stage"; then
        current_mode=$(grep '\[widescreen\] mode=' "$log" 2>/dev/null | tail -1)
        if [[ $current_mode == *mode=gameplay-expand ]]; then
            if [[ $capture_early == 1 ]]; then
                ready=1
                break
            fi
            sleep 1
            current_mode=$(grep '\[widescreen\] mode=' "$log" 2>/dev/null | tail -1)
            if [[ $current_mode == *mode=gameplay-expand ]]; then
                ready=1
                break
            fi
        fi
    elif grep -q '\[widescreen\] gameplay-ready scene=' \
        "$log" 2>/dev/null; then
        ready=1
        break
    fi
done

if (( ! ready )); then
    echo "stage did not reach stable gameplay; see $log" >&2
    exit 1
fi

# Keep fast-forward engaged while a requested moving-camera target is reached.
# This lets the in-game test auto-advance clear tutorial dialogue without
# capturing the several seconds during which Marina is intentionally paused.
if [[ -n $capture_camera_x ]]; then
    camera_ready=0
    # Poll densely while the controller is moving. A one-second poll can skip
    # straight over a narrow streaming/pop boundary, especially while Tab is
    # still fast-forwarding the game.
    camera_attempts=$((capture_delay * 20))
    for ((attempt = 0; attempt < camera_attempts; ++attempt)); do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "game exited before camera reached $capture_camera_x; see $log" >&2
            exit 1
        fi
        camera_x=$(grep '\[test-move\]' "$log" 2>/dev/null | tail -1 |
            sed -nE 's/.* camera=(-?[0-9]+),.*/\1/p')
        if [[ -n $camera_x ]]; then
            if [[ $test_move == jet-left ]]; then
                (( camera_x <= capture_camera_x )) && camera_ready=1
            else
                (( camera_x >= capture_camera_x )) && camera_ready=1
            fi
        fi
        (( camera_ready )) && break
        sleep 0.05
    done
    if (( ! camera_ready )); then
        echo "camera did not reach $capture_camera_x; see $log" >&2
        exit 1
    fi
fi

if [[ $capture_warmup != 0 ]]; then
    sleep "$capture_warmup"
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "game exited during capture warmup; see $log" >&2
        exit 1
    fi
fi

if [[ -n $fast_window ]]; then
    xdotool keyup --window "$fast_window" Tab 2>/dev/null || true
    fast_window=
fi

frames=()
capture_log="$output_dir/capture-index.tsv"
printf 'capture\tlast_test_move\n' >"$capture_log"
for ((frame = 0; frame < capture_frames; ++frame)); do
    file="$output_dir/frame-$(printf '%03d' "$frame").png"
    last_test_move=$(grep '\[test-move\]' "$log" 2>/dev/null | tail -1)
    printf '%03d\t%s\n' "$frame" "$last_test_move" >>"$capture_log"
    if ! capture_game_frame "$file"; then
        echo "failed to capture frame $frame; see $log" >&2
        exit 1
    fi
    frames+=("$file")
    sleep "$capture_interval"
done

if ! magick montage "${frames[@]}" -thumbnail 400x256 -tile 4x \
    -geometry +2+2 "$output_dir/contact-sheet.png"; then
    echo "failed to assemble contact sheet; see $log" >&2
    exit 1
fi

echo "[render-burst] stage=$stage frames=$capture_frames output=$output_dir"
