#!/usr/bin/env bash
# Dense regression matrix for the rotating-room wall and moving 3D platforms.
# The burst harness creates contact sheets for manual trail inspection; this
# wrapper also rejects the large low-saturation slabs produced by a missing
# platform texture/material.

set -u

usage() {
    echo "usage: $0 GAME_BINARY ROM OUTPUT_DIR" >&2
    echo "env: MM_ROTATION_ASPECTS='wide 4x3' MM_ROTATION_FPS='60 display' MM_ROTATION_FRAMES=64 MM_ROTATION_INTERVAL=0.08 MM_ROTATION_FLAT_THRESHOLD=0.05" >&2
}

if (( $# != 3 )); then
    usage
    exit 2
fi

game=$1
rom=$2
output_dir=$3
frames=${MM_ROTATION_FRAMES:-64}
interval=${MM_ROTATION_INTERVAL:-0.08}
flat_threshold=${MM_ROTATION_FLAT_THRESHOLD:-0.05}
read -r -a aspects <<<"${MM_ROTATION_ASPECTS:-wide 4x3}"
read -r -a fps_modes <<<"${MM_ROTATION_FPS:-60 display}"

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
burst="$script_dir/test_render_burst.sh"
mkdir -p "$output_dir"
manifest="$output_dir/results.tsv"
printf 'aspect\tfps\tstage\tscene\tstatus\tmax-neutral-flat\tcontact-sheet\n' >"$manifest"

failures=0
passes=0
for aspect in "${aspects[@]}"; do
    case $aspect in
        wide) widescreen=1 ;;
        4x3) widescreen=0 ;;
        *) echo "unsupported aspect: $aspect" >&2; exit 2 ;;
    esac

    for fps in "${fps_modes[@]}"; do
        for stage in 13 21; do
            if [[ $stage == 13 ]]; then
                scene=69
                stage_name=vertigo
            else
                scene=13
                stage_name=seasick-climb
            fi
            run_dir="$output_dir/$stage_name-$aspect-$fps"
            echo "[rotation-suite] stage=$stage scene=$scene aspect=$aspect fps=$fps"

            if MM_CAPTURE_FRAMES="$frames" \
                MM_CAPTURE_INTERVAL="$interval" \
                MM_CAPTURE_DELAY=60 MM_FAST_FORWARD=1 \
                MM_TEST_MOVE=walk-right MM_WIDESCREEN="$widescreen" \
                MM_FPS="$fps" "$burst" "$game" "$rom" "$run_dir" "$stage"; then
                run_ok=1
            else
                run_ok=0
            fi

            max_flat=0
            if (( run_ok )); then
                for ((frame_index = 0; frame_index < frames; ++frame_index)); do
                    frame="$run_dir/frame-$(printf '%03d' "$frame_index").png"
                    if [[ ! -f $frame ]]; then
                        echo "  FAILED missing captured frame $frame" >&2
                        run_ok=0
                        break
                    fi
                    # Ignore black pillarboxes, then measure neutral mid-tone
                    # pixels in the authored canvas. The broken platform covers
                    # roughly 12% of a frame; correct textured captures remain
                    # below 1% on the reference NVIDIA and RADV paths.
                    if ! flat=$(magick "$frame" -gravity center \
                        -crop 75%x100%+0+0 +repage -resize 25% \
                        -colorspace HSL \
                        -fx '(g<0.04&&b>0.15&&b<0.75)?1:0' \
                        -format '%[fx:mean]' info:); then
                        echo "  FAILED unreadable captured frame $frame" >&2
                        run_ok=0
                        break
                    fi
                    if [[ ! $flat =~ ^[0-9]+([.][0-9]+)?$ ]]; then
                        echo "  FAILED invalid neutral slab score '$flat' for $frame" >&2
                        run_ok=0
                        break
                    fi
                    if awk -v value="$flat" -v maximum="$max_flat" \
                        'BEGIN { exit !(value > maximum) }'; then
                        max_flat=$flat
                    fi
                done

                if (( run_ok )); then
                    log="$run_dir/stage-$(printf '%02d' "$stage").log"
                    grep -q "\[widescreen\] gameplay-ready scene=$scene" "$log" || run_ok=0
                    last_mode=$(grep '\[widescreen\] mode=' "$log" | tail -1)
                    [[ $last_mode == *mode=cinematic-4:3 ]] || run_ok=0
                    if grep -Eqi 'segmentation fault|assertion.*failed|fatal error|aborted|RT64 setup failed' "$log"; then
                        run_ok=0
                    fi
                    if awk -v value="$max_flat" -v limit="$flat_threshold" \
                        'BEGIN { exit !(value > limit) }'; then
                        echo "  FAILED neutral slab score $max_flat exceeds $flat_threshold" >&2
                        run_ok=0
                    fi
                fi
            fi

            if (( run_ok )); then
                status=pass
                ((passes += 1))
            else
                status=fail
                ((failures += 1))
            fi
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                "$aspect" "$fps" "$stage" "$scene" "$status" \
                "$max_flat" "$run_dir/contact-sheet.png" >>"$manifest"
        done
    done
done

echo "[rotation-suite] pass=$passes fail=$failures manifest=$manifest"
(( failures == 0 ))
