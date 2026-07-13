#!/usr/bin/env bash
# Screenshot/crash sweep for the 52 actual progression levels. Cinematic,
# attract, opening, ending, and demo rows are deliberately excluded.

set -u

usage() {
    echo "usage: $0 GAME_BINARY ROM OUTPUT_DIR [STAGE_INDEX ...]" >&2
    echo "env: MM_CAPTURE_DELAY=60 MM_CAPTURE_FRAMES=3 MM_CAPTURE_INTERVAL=0.35 MM_CAPTURE_GEOMETRY=1602x1022+39+59 MM_WINDOW=1600x900 MM_AUTO_ADVANCE=1 MM_TEST_MOVE=1|jet-right MM_TEST_STREAM_TRACE=1 MM_TEST_STREAM_CAMERA=x[,y]" >&2
}

if (( $# < 3 )); then
    usage
    exit 2
fi

game=$1
rom=$2
output_dir=$3
shift 3

capture_delay=${MM_CAPTURE_DELAY:-60}
capture_geometry=${MM_CAPTURE_GEOMETRY:-1602x1022+39+59}
window_size=${MM_WINDOW:-1600x900}
auto_advance=${MM_AUTO_ADVANCE:-1}
capture_frames=${MM_CAPTURE_FRAMES:-3}
capture_interval=${MM_CAPTURE_INTERVAL:-0.35}
test_move=${MM_TEST_MOVE:-1}
stream_trace=${MM_TEST_STREAM_TRACE:-0}
stream_camera=${MM_TEST_STREAM_CAMERA:-}

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
    echo "DISPLAY is not set; the live screenshot suite requires X11/XWayland" >&2
    exit 2
fi

mkdir -p "$output_dir/logs"
manifest="$output_dir/results.tsv"
printf 'stage\tscene\tstatus\tscreenshot\n' >"$manifest"

active_pid=
fast_window=
cleanup_active() {
    if [[ -n $fast_window ]]; then
        xdotool keyup --window "$fast_window" Tab 2>/dev/null || true
        fast_window=
    fi
    if [[ -n $active_pid ]] && kill -0 "$active_pid" 2>/dev/null; then
        kill -KILL -- "-$active_pid" 2>/dev/null || true
    fi
    if [[ -n $active_pid ]]; then
        { wait "$active_pid"; } 2>/dev/null || true
    fi
    active_pid=
}
trap cleanup_active EXIT
trap 'cleanup_active; exit 130' INT TERM

# stage-index:scene-id, taken from the US 1.1 gStageScenes progression table.
# Excluded rows: opn=0, *-d=1/23/36/48/55, end=58/59, extras=60..63.
playable=(
    2:0 3:68 4:53 5:55 6:52 7:67 8:56 9:58 10:54 11:57
    12:1 13:69 14:37 15:60 16:38 17:59 18:61 19:70 20:62 21:13 22:5
    24:72 25:12 26:35 27:71 28:32 29:31 30:36 31:9 32:33 33:18 34:29 35:19
    37:42 38:39 39:40 40:23 41:41 42:10 43:48 44:24 45:46 46:47 47:25
    49:77 50:20 51:78 52:85 53:22 54:26 56:79 57:27
)

requested=("$@")
is_requested() {
    local candidate=$1
    if (( ${#requested[@]} == 0 )); then
        return 0
    fi
    local item
    for item in "${requested[@]}"; do
        [[ $candidate == "$item" ]] && return 0
    done
    return 1
}

is_fixed_4x3_stage() {
    case $1 in
        11|27|47|52|56|57) return 0 ;;
        *) return 1 ;;
    esac
}

passes=0
failures=0
for entry in "${playable[@]}"; do
    stage=${entry%%:*}
    scene=${entry##*:}
    is_requested "$stage" || continue

    stem=$(printf 'stage-%02d-scene-%02d' "$stage" "$scene")
    screenshot="$output_dir/$stem-contact.png"
    log="$output_dir/logs/$stem.log"
    echo "[widescreen-suite] stage=$stage scene=$scene"

    setsid env MM_WARP_STAGE="$stage" MM_WARP_AT=1 MM_WARP_DELAY=1 \
        MM_TEST_AUTO_ADVANCE="$auto_advance" MM_TEST_MOVE="$test_move" \
        MM_TEST_STREAM_TRACE="$stream_trace" \
        MM_TEST_STREAM_CAMERA="$stream_camera" \
        MM_WIN_POS=40,40 SDL_VIDEODRIVER=x11 \
        "$game" "$rom" \
        --window "$window_size" --widescreen >"$log" 2>&1 &
    pid=$!
    active_pid=$pid

    if command -v xdotool >/dev/null; then
        for ((attempt = 0; attempt < 50; ++attempt)); do
            window=$(xdotool search --onlyvisible --pid "$pid" 2>/dev/null | tail -1)
            if [[ -n $window ]]; then
                xdotool windowactivate --sync "$window" 2>/dev/null || true
                xdotool keydown --window "$window" Tab 2>/dev/null || true
                fast_window=$window
                break
            fi
            sleep 0.1
        done
    fi

    alive=1
    reached_gameplay=0
    for ((second = 0; second < capture_delay; ++second)); do
        sleep 1
        if ! kill -0 "$pid" 2>/dev/null; then
            alive=0
            break
        fi
        if ! grep -q "\[warp\] stage=$stage " "$log" 2>/dev/null; then
            continue
        fi
        current_mode=$(grep '\[widescreen\] mode=' "$log" 2>/dev/null | tail -1)
        if is_fixed_4x3_stage "$stage"; then
            if grep -q "\[widescreen\] gameplay-ready scene=$scene" \
                "$log" 2>/dev/null; then
                reached_gameplay=1
                break
            fi
            continue
        fi
        if [[ $current_mode == *mode=gameplay-expand ]]; then
            sleep 1
            current_mode=$(grep '\[widescreen\] mode=' "$log" 2>/dev/null | tail -1)
            if [[ $current_mode == *mode=gameplay-expand ]]; then
                reached_gameplay=1
                break
            fi
        fi
    done

    layer_ok=1
    if (( alive && reached_gameplay )); then
        if [[ -n $fast_window ]]; then
            xdotool keyup --window "$fast_window" Tab 2>/dev/null || true
            fast_window=
        fi
        capture_ok=1
        frames=()
        for ((frame = 0; frame < capture_frames; ++frame)); do
            frame_file="$output_dir/$stem-frame-$(printf '%02d' "$frame").png"
            if magick x:root -crop "$capture_geometry" +repage "$frame_file"; then
                frames+=("$frame_file")
            else
                capture_ok=0
                break
            fi
            sleep "$capture_interval"
        done
        if (( capture_ok )); then
            magick montage "${frames[@]}" -thumbnail 400x256 \
                -tile "${capture_frames}x1" -geometry +2+2 "$screenshot" || capture_ok=0
        fi
        # Test builds log one audit row for every tile layer that actually
        # draws in expanded gameplay. Any partially empty 70-tile wing is a
        # render-quality failure even if the process stayed alive; this catches
        # the Taurus 4:3 layer box that the original crash-only suite missed.
        if ! is_fixed_4x3_stage "$stage" &&
            grep -Eq 'wings=([0-9]|[1-6][0-9])/70' "$log"; then
            layer_ok=0
            echo "  FAILED incomplete widescreen layer (see $log)" >&2
        fi
    else
        capture_ok=0
        # Preserve the blocked 4:3 frame for diagnosing a prompt or an
        # incorrectly classified progression row.
        if (( alive )); then
            magick x:root -crop "$capture_geometry" +repage \
                "$output_dir/$stem-blocked.png" 2>/dev/null || true
        fi
    fi

    # A successful case has already survived loading, reached stable player
    # control, and rendered an additional second. Stop it here so the full
    # 52-level sweep does not spend most of its time waiting on timeouts.
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL -- "-$pid" 2>/dev/null || true
        stopped_by_suite=$capture_ok
    else
        stopped_by_suite=0
    fi
    { wait "$pid"; } 2>/dev/null
    rc=$?
    active_pid=
    fast_window=
    if (( capture_ok && stopped_by_suite && layer_ok )); then
        status=pass
        ((passes += 1))
    else
        status=fail
        ((failures += 1))
        echo "  FAILED rc=$rc gameplay=$reached_gameplay capture=$capture_ok (see $log)" >&2
        tail -20 "$log" >&2
    fi
    printf '%s\t%s\t%s\t%s\n' "$stage" "$scene" "$status" "$screenshot" >>"$manifest"
done

echo "[widescreen-suite] pass=$passes fail=$failures manifest=$manifest"
(( failures == 0 ))
