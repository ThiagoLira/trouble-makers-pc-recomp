#!/usr/bin/env bash
# Capture a short frame sequence from one progression stage. Unlike the full
# playable-stage sweep, this keeps rendering long enough to expose transient
# geometry loss, z-fighting, and actor pop-in.

set -u

usage() {
    echo "usage: $0 GAME_BINARY ROM OUTPUT_DIR STAGE_INDEX" >&2
    echo "env: MM_CAPTURE_FRAMES=12 MM_CAPTURE_INTERVAL=0.25 MM_CAPTURE_DELAY=60 MM_CAPTURE_GEOMETRY=1602x1022+39+59 MM_WINDOW=1600x900 MM_WIDESCREEN=1" >&2
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
capture_geometry=${MM_CAPTURE_GEOMETRY:-1602x1022+39+59}
window_size=${MM_WINDOW:-1600x900}
widescreen=${MM_WIDESCREEN:-1}

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
fi

setsid env MM_WARP_STAGE="$stage" MM_WARP_AT=1 MM_WARP_DELAY=1 \
    MM_TEST_AUTO_ADVANCE=1 MM_WIN_POS=40,40 SDL_VIDEODRIVER=x11 \
    "${args[@]}" >"$log" 2>&1 &
pid=$!

cleanup() {
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL -- "-$pid" 2>/dev/null || true
    fi
    { wait "$pid"; } 2>/dev/null || true
}
trap cleanup EXIT INT TERM

is_fixed_4x3_stage() {
    case $1 in
        11|13|21|22|27|30|47|52|54|56|57) return 0 ;;
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
    if [[ $widescreen == 1 ]] && ! is_fixed_4x3_stage "$stage"; then
        current_mode=$(grep '\[widescreen\] mode=' "$log" 2>/dev/null | tail -1)
        if [[ $current_mode == *mode=gameplay-expand ]]; then
            sleep 1
            ready=1
            break
        fi
    elif (( second >= 12 )); then
        ready=1
        break
    fi
done

if (( ! ready )); then
    echo "stage did not reach stable gameplay; see $log" >&2
    exit 1
fi

frames=()
for ((frame = 0; frame < capture_frames; ++frame)); do
    file="$output_dir/frame-$(printf '%03d' "$frame").png"
    magick x:root -crop "$capture_geometry" +repage "$file"
    frames+=("$file")
    sleep "$capture_interval"
done

magick montage "${frames[@]}" -thumbnail 400x256 -tile 4x -geometry +2+2 \
    "$output_dir/contact-sheet.png"

echo "[render-burst] stage=$stage frames=$capture_frames output=$output_dir"
