#!/usr/bin/env bash
# Focused visual regression for the early forest terrain pops.
#
# The general playable sweep samples too sparsely and too late: by then Marina
# is usually stuck against the next block stack. This test records the first
# rightward traversal densely and verifies that it crossed the native 512px
# wrap used by the foreground terrain actor. It also freezes scenes 0 and 68
# at the exact rollover of their four-panel landscape strip and checks a wing
# pixel against the adjacent terrain. The resulting GIF, sheet, and rollover
# pair make the before/after behavior easy to review.

set -euo pipefail

usage() {
    echo "usage: $0 GAME_BINARY ROM OUTPUT_DIR" >&2
    echo "env: MM_TERRAIN_FRAMES=48 MM_TERRAIN_INTERVAL=0.05 MM_TERRAIN_CAMERA_X=1340 MM_TERRAIN_ROLLOVER_FRAMES=12 MM_WINDOW=1600x900" >&2
}

if (( $# != 3 )); then
    usage
    exit 2
fi

game=$1
rom=$2
output_dir=$3
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
frames=${MM_TERRAIN_FRAMES:-48}
interval=${MM_TERRAIN_INTERVAL:-0.05}
camera_x=${MM_TERRAIN_CAMERA_X:-1340}
rollover_frames=${MM_TERRAIN_ROLLOVER_FRAMES:-12}

if ! [[ $frames =~ ^[0-9]+$ ]] || (( frames < 24 )); then
    echo "MM_TERRAIN_FRAMES must be an integer >= 24: $frames" >&2
    exit 2
fi
if ! [[ $rollover_frames =~ ^[0-9]+$ ]] || (( rollover_frames < 2 )); then
    echo "MM_TERRAIN_ROLLOVER_FRAMES must be an integer >= 2: $rollover_frames" >&2
    exit 2
fi

rm -rf -- "$output_dir"
mkdir -p -- "$output_dir"

# Progression index 3 is scene 68, the second playable forest level shown in
# the reference recording. jet-bounce's first leg is a single long rightward
# traversal; its reverse leg starts after frame 300, well beyond this capture.
MM_CAPTURE_FRAMES="$frames" \
MM_CAPTURE_INTERVAL="$interval" \
MM_CAPTURE_WARMUP=0 \
MM_CAPTURE_CAMERA_X="$camera_x" \
MM_CAPTURE_EARLY=1 \
MM_WINDOW="${MM_WINDOW:-1600x900}" \
MM_TEST_MOVE=jet-bounce \
MM_TEST_ACTOR_TRACE=1 \
MM_TEST_TERRAIN_REPEAT_TRACE=1 \
MM_TEST_STREAM_TRACE=1 \
"$script_dir/test_render_burst.sh" \
    "$game" "$rom" "$output_dir" 3

log="$output_dir/stage-03.log"
wrap_report="$output_dir/terrain-wrap.txt"

# Require proof that the recording exercised the faulty path: one state-2
# 0x181c terrain actor must jump between the two sides of its +/-256px native
# wrap. This is coverage, not a claim that the rendered seam is already fixed.
if ! awk '
    /\[test-actor\]/ && /type=181c/ && /state=0002/ {
        slot = "";
        x = "";
        frame = "";
        for (i = 1; i <= NF; ++i) {
            if ($i ~ /^slot=/) {
                slot = $i;
                sub(/^slot=/, "", slot);
            }
            else if ($i ~ /^frame=/) {
                frame = $i;
                sub(/^frame=/, "", frame);
            }
            else if ($i ~ /^screen=/) {
                split($i, pos, "[,=]");
                x = pos[2] + 0;
            }
        }
        if (slot != "" && x != "") {
            if ((seen[slot] && previous_x[slot] <= -220 && x >= 220) ||
                (seen[slot] && previous_x[slot] >= 220 && x <= -220)) {
                printf("slot=%s frame=%s previous_x=%d current_x=%d\n",
                    slot, frame, previous_x[slot], x);
                found = 1;
            }
            seen[slot] = 1;
            previous_x[slot] = x;
        }
    }
    END { exit found ? 0 : 1 }
' "$log" >"$wrap_report"; then
    echo "terrain wrap was not exercised; see $log" >&2
    exit 1
fi

# The fix is render-only, so the native wrap above still occurs. Require the
# adjacent terrain copy to have been injected for this exact scene as well;
# otherwise a build without the fix could satisfy the coverage check.
if ! grep -q '^\[widescreen-terrain-repeat\] scene=68 ' "$log"; then
    echo "terrain repeat fix did not run for scene 68; see $log" >&2
    exit 1
fi

# The green landscape itself is a second 512px repeat: four type-000d/state-50
# panels at 128px intervals. Each source panel must have an adjacent render
# copy as well as the state-2 foreground actor checked above.
landscape_clones=$(grep -c \
    '^\[widescreen-terrain-clone\].*type=000d state=0050 ' "$log" || true)
if (( landscape_clones < 4 )); then
    echo "expected four landscape-panel repeats, found $landscape_clones; see $log" >&2
    exit 1
fi

captures=("$output_dir"/frame-*.png)
review_count=${#captures[@]}
(( review_count > 60 )) && review_count=60
review=("${captures[@]:0:review_count}")

# Full numbered context plus a crop centered on the foreground to the right
# of Marina's starting house. Keep only the useful early traversal so the
# later collision with the block stack cannot hide the pop in a long clip.
magick montage -label '%f' "${review[@]}" -thumbnail 400x225 \
    -tile 5x -geometry +2+2 "$output_dir/terrain-pop-frames.png"

delay=$(awk -v seconds="$interval" 'BEGIN {
    value = int(seconds * 100 + 0.5);
    print value < 1 ? 1 : value;
}')
magick -delay "$delay" -loop 0 "${review[@]}" \
    -crop 900x500+700+250 +repage -resize 900x500 \
    "$output_dir/terrain-pop-right.gif"

# Deterministic regression for the visible landscape rollover. With the
# native single copy, the source panel jumps from -256 to +255 and exposes the
# blue environment beneath it at the lower-right wing. At these exact camera
# positions, x=1575 and x=1590 are covered by the same terrain panel when the
# adjacent copy exists, so their pixels are equal. This makes the assertion
# independent of sparse moving-frame timing.
capture_rollover() {
    local stage=$1
    local camera=$2
    local dir=$3
    MM_WINDOW=1600x900 \
    MM_CAPTURE_FRAMES="$rollover_frames" \
    MM_CAPTURE_INTERVAL=0.05 \
    MM_CAPTURE_WARMUP=1 \
    MM_CAPTURE_EARLY=1 \
    MM_TEST_MOVE=0 \
    MM_TEST_STREAM_TRACE=1 \
    MM_TEST_STREAM_CAMERA="$camera,348" \
    MM_TEST_ACTOR_TRACE=1 \
    MM_TEST_TERRAIN_REPEAT_TRACE=1 \
    MM_TEST_DISABLE_LANDSCAPE_REPEAT=0 \
    MM_TEST_HIDE_LANDSCAPE_PANELS=0 \
    "$script_dir/test_render_burst.sh" \
        "$game" "$rom" "$dir" "$stage"
}

scene0_dir="$output_dir/rollover-scene-00"
scene68_dir="$output_dir/rollover-scene-68"
capture_rollover 2 512 "$scene0_dir"
capture_rollover 3 1024 "$scene68_dir"

rollover_report="$output_dir/landscape-rollover.txt"
: >"$rollover_report"
for entry in "scene-00:$scene0_dir" "scene-68:$scene68_dir"; do
    name=${entry%%:*}
    dir=${entry#*:}
    shots=("$dir"/frame-*.png)
    if (( ${#shots[@]} != rollover_frames )); then
        echo "$name expected $rollover_frames rollover frames, found ${#shots[@]}" >&2
        exit 1
    fi
    for shot in "${shots[@]}"; do
        sample_inner_x=1575
        sample_wing_x=1590
        sample_y=775
        if [[ ${MM_VIDEO_DRIVER:-x11} == wayland ]]; then
            # Spectacle preserves KWin's centered drop-shadow canvas around
            # an active window, unlike ImageMagick's direct X11 window grab.
            # Translate the established viewport samples by that symmetric
            # padding instead of accidentally sampling the shadow/decoration.
            read -r shot_width shot_height < <(
                magick identify -format '%w %h\n' "$shot"
            )
            if (( shot_width > 1602 )); then
                pad_x=$(((shot_width - 1602 + 1) / 2))
                ((sample_inner_x += pad_x))
                ((sample_wing_x += pad_x))
            fi
            if (( shot_height > 1022 )); then
                pad_y=$(((shot_height - 1022 + 1) / 2))
                ((sample_y += pad_y))
            fi
        fi
        inner=$(magick "$shot" -format \
            "%[pixel:p{$sample_inner_x,$sample_y}]" info:)
        wing=$(magick "$shot" -format \
            "%[pixel:p{$sample_wing_x,$sample_y}]" info:)
        if [[ $inner != "$wing" ]]; then
            echo "$name landscape rollover flickered: inner=$inner wing=$wing; see $shot" >&2
            exit 1
        fi
    done
    printf '%s frames=%d inner=%s wing=%s\n' \
        "$name" "$rollover_frames" "$inner" "$wing" \
        >>"$rollover_report"
    if [[ $name == scene-00 ]]; then
        stage=2
    else
        stage=3
    fi
    panel_clones=$(grep -c \
        '^\[widescreen-terrain-clone\].*type=000d state=0050 ' \
        "$dir/stage-$(printf '%02d' "$stage").log" || true)
    if (( panel_clones < 4 )); then
        echo "$name did not render all four adjacent landscape panels" >&2
        exit 1
    fi
done

magick montage "$scene0_dir/frame-000.png" "$scene68_dir/frame-000.png" \
    -resize 800x450 -tile 2x -geometry +2+2 \
    "$output_dir/landscape-rollovers.png"

echo "[terrain-pop] reproduced scene=68 stage=3 frames=$review_count"
echo "[terrain-pop] wrap=$(tr '\n' ' ' <"$wrap_report")"
echo "[terrain-pop] repeat=$(grep -m1 '^\[widescreen-terrain-repeat\] scene=68 ' "$log")"
echo "[terrain-pop] landscape-clones=$landscape_clones"
echo "[terrain-pop] rollover=$(tr '\n' ' ' <"$rollover_report")"
echo "[terrain-pop] sheet=$output_dir/terrain-pop-frames.png"
echo "[terrain-pop] gif=$output_dir/terrain-pop-right.gif"
echo "[terrain-pop] rollover-sheet=$output_dir/landscape-rollovers.png"
