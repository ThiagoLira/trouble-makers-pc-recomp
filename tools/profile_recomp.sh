#!/usr/bin/env bash
# Capture native CPU profiles of the host runtime, RT64, RSP microcode, and
# statically recompiled game code. GNU gprofng is part of modern binutils and
# samples an optimized executable without per-function instrumentation.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$script_dir/.." && pwd)

build_dir="$repo_dir/build-profile"
duration=30
warmup=5
mode=cpu
output=
jobs=
build=1
use_user_config=0
html=0
report_only=

usage() {
    cat <<'EOF'
Usage:
  tools/profile_recomp.sh [options] -- ROM [game options]
  tools/profile_recomp.sh --report EXPERIMENT.er [--html]

Options:
  --mode MODE        cpu (default), counters, cache, sync, heap, or io
  --duration SEC     measured interval after warm-up (default: 30)
  --warmup SEC       unmeasured startup interval (default: 5)
  --build-dir DIR    profiling build directory (default: build-profile)
  --output PATH      experiment directory; must end in .er
  --jobs N           parallel build jobs (default: build-tool default)
  --no-build         use an already-built profiling executable
  --use-user-config  allow the run to use the normal config/save directory
  --html             also generate a navigable HTML report
  --report PATH      regenerate reports for an existing experiment
  -h, --help         show this help

Examples:
  tools/profile_recomp.sh --duration 45 --warmup 15 -- \
      input/troublemakers.us1.z64 --window 960x720 --no-widescreen
  tools/profile_recomp.sh --mode counters --duration 20 -- \
      input/troublemakers.us1.z64 --window 960x720
  tools/profile_recomp.sh --no-build --duration 60 -- \
      input/troublemakers.us1.z64 --window 960x720

Profiles are written below build-profile/profiles by default. The game is run
through a target-side timeout so gprofng can finalize normally after the
bounded capture. Exit status 124 from timeout is expected.
EOF
}

die() {
    echo "profile_recomp: $*" >&2
    exit 2
}

is_nonnegative_integer() {
    [[ $1 =~ ^[0-9]+$ ]]
}

while (($#)); do
    case "$1" in
        --mode)
            (($# >= 2)) || die "--mode needs a value"
            mode=$2
            shift 2
            ;;
        --duration)
            (($# >= 2)) || die "--duration needs a value"
            duration=$2
            shift 2
            ;;
        --warmup)
            (($# >= 2)) || die "--warmup needs a value"
            warmup=$2
            shift 2
            ;;
        --build-dir)
            (($# >= 2)) || die "--build-dir needs a value"
            build_dir=$2
            shift 2
            ;;
        --output)
            (($# >= 2)) || die "--output needs a value"
            output=$2
            shift 2
            ;;
        --jobs)
            (($# >= 2)) || die "--jobs needs a value"
            jobs=$2
            shift 2
            ;;
        --no-build)
            build=0
            shift
            ;;
        --use-user-config)
            use_user_config=1
            shift
            ;;
        --html)
            html=1
            shift
            ;;
        --report)
            (($# >= 2)) || die "--report needs an experiment directory"
            report_only=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            die "unknown option: $1 (put game arguments after --)"
            ;;
    esac
done

case "$mode" in
    cpu|counters|cache|sync|heap|io) ;;
    *) die "unsupported mode '$mode'" ;;
esac

is_nonnegative_integer "$duration" || die "duration must be a non-negative integer"
is_nonnegative_integer "$warmup" || die "warmup must be a non-negative integer"
((duration > 0)) || die "duration must be greater than zero"

command -v gprofng >/dev/null 2>&1 || die \
    "gprofng was not found; install GNU binutils 2.39 or newer"

write_reports() {
    local experiment=$1
    local report_mode=$2
    local -a metric_args
    local -a experiments
    local -a descendants

    # When the target is the timeout wrapper, gprofng records the actual game
    # and any grandchildren as descendant experiments. Reports do not
    # aggregate those automatically when passed only the founder directory.
    mapfile -d '' -t descendants < <(
        find "$experiment" -mindepth 1 -type d -name '*.er' -print0
    )
    experiments=("$experiment" "${descendants[@]}")

    case "$report_mode" in
        counters|cache)
            metric_args=(-metrics hwc -sort e.insts)
            ;;
        *)
            metric_args=(-metrics e.totalcpu:i.totalcpu:name -sort e.totalcpu)
            ;;
    esac

    gprofng display text "${metric_args[@]}" -limit 120 -functions -overview \
        "${experiments[@]}" >"$experiment/functions.txt"
    gprofng display text "${metric_args[@]}" -limit 240 -calltree \
        "${experiments[@]}" >"$experiment/calltree.txt"

    if ((html)); then
        gprofng display html --quiet -o "$experiment/html" "${experiments[@]}"
    fi

    echo "Experiment: $experiment"
    echo "Functions:  $experiment/functions.txt"
    echo "Call tree:  $experiment/calltree.txt"
    if ((html)); then
        echo "HTML:       $experiment/html/index.html"
    else
        echo "HTML:       tools/profile_recomp.sh --report '$experiment' --html"
    fi
}

write_environment() {
    local experiment=$1
    local variable
    shift

    {
        printf 'captured_at=%s\n' "$(date --iso-8601=seconds)"
        printf 'repo_commit='
        git -C "$repo_dir" rev-parse HEAD 2>/dev/null || printf 'unknown\n'
        printf 'repo_dirty_files='
        git -C "$repo_dir" status --porcelain 2>/dev/null | wc -l
        printf 'command='
        printf ' %q' "$game" "$@"
        printf '\n\n'

        for variable in XDG_SESSION_TYPE DISPLAY WAYLAND_DISPLAY \
            APP_FOLDER_PATH XDG_CONFIG_HOME \
            SDL_VIDEODRIVER SDL_AUDIODRIVER VK_DRIVER_FILES \
            VK_ICD_FILENAMES; do
            printf '%s=%s\n' "$variable" "${!variable-}"
        done

        printf '\n[uname]\n'
        uname -a
        if command -v lscpu >/dev/null 2>&1; then
            printf '\n[lscpu]\n'
            lscpu
        fi
        printf '\n[compiler]\n'
        "${CXX:-c++}" --version
        printf '\n[gprofng]\n'
        gprofng --version

        # The loader summary lists every visible device. The target's actual
        # selection and any swapchain/import errors remain in run.log.
        if command -v vulkaninfo >/dev/null 2>&1; then
            printf '\n[vulkaninfo --summary]\n'
            timeout --signal=TERM --kill-after=2s 10s vulkaninfo --summary
        fi
    } >"$experiment/environment.txt" 2>&1 || true
}

if [[ -n $report_only ]]; then
    [[ -d $report_only ]] || die "experiment does not exist: $report_only"
    if [[ -f $report_only/mm-mode ]]; then
        mode=$(<"$report_only/mm-mode")
    fi
    write_reports "$report_only" "$mode"
    exit 0
fi

(($# >= 1)) || die "missing ROM and game arguments after --"

if [[ $build_dir != /* ]]; then
    build_dir="$repo_dir/$build_dir"
fi

if ((build)); then
    cmake -S "$repo_dir" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DMM_PROFILE_BUILD=ON
    build_args=(--build "$build_dir" --target troublemakers)
    if [[ -n $jobs ]]; then
        build_args+=(--parallel "$jobs")
    fi
    cmake "${build_args[@]}"
fi

game="$build_dir/src/game/troublemakers"
[[ -x $game ]] || die "profiling executable not found: $game"

if [[ -z $output ]]; then
    stamp=$(date +%Y%m%d-%H%M%S)
    output="$build_dir/profiles/$mode-$stamp.er"
elif [[ $output != /* ]]; then
    output="$repo_dir/$output"
fi
[[ $output == *.er ]] || die "output path must end in .er"
[[ ! -e $output ]] || die "output already exists: $output"
mkdir -p "$(dirname -- "$output")"

collect_args=(-j off -F on -a usedldobjects -S 1)
case "$mode" in
    cpu)
        collect_args+=(-p hi)
        ;;
    counters)
        collect_args+=(-p off -h insts -h cycles)
        ;;
    cache)
        collect_args+=(-p off -h insts -h cycles -h cache-misses -h branch-misses)
        ;;
    sync)
        collect_args+=(-p hi -s on,n)
        ;;
    heap)
        collect_args+=(-p hi -H on)
        ;;
    io)
        collect_args+=(-p hi -i on)
        ;;
esac

measured_end=$((warmup + duration))
hard_timeout=$((measured_end + 3))
profile_config="$build_dir/profile-config"
if ((use_user_config == 0)); then
    mkdir -p "$profile_config"
fi

echo "Capturing $mode profile: warm-up=${warmup}s measured=${duration}s"
echo "Target: $game $*"

run_log="$output.run.log"

set +e
if ((use_user_config)); then
    gprofng collect app "${collect_args[@]}" -t "${warmup}-${measured_end}s" \
        -o "$output" timeout --signal=TERM --kill-after=3s "${hard_timeout}s" \
        "$game" "$@" 2>&1 | tee "$run_log"
else
    APP_FOLDER_PATH="$profile_config" XDG_CONFIG_HOME="$profile_config" \
        gprofng collect app "${collect_args[@]}" -t "${warmup}-${measured_end}s" \
        -o "$output" timeout --signal=TERM --kill-after=3s "${hard_timeout}s" \
        "$game" "$@" 2>&1 | tee "$run_log"
fi
collect_status=${PIPESTATUS[0]}
set -e

if ((collect_status != 0 && collect_status != 124)); then
    die "collector failed with status $collect_status"
fi
[[ -s $output/overview ]] || die "collector produced no usable experiment"

mv "$run_log" "$output/run.log"
printf '%s\n' "$mode" >"$output/mm-mode"
write_environment "$output" "$@"
write_reports "$output" "$mode"
