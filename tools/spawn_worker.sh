#!/usr/bin/env bash
# spawn_worker.sh — launch an OpenRouter worker in a tmux window with a live pretty view
# usage: spawn_worker.sh <name> <workspace-dir> <prompt-file>
#   window layout: left pane = raw worker process, right pane = watch_worker.sh
#   session: $TMUX_SESSION (default "glm"); attach with: tmux attach -t glm
set -euo pipefail
NAME="${1:?worker name}"; WS="${2:?workspace-dir}"; PROMPT="${3:?prompt-file}"
SESSION="${TMUX_SESSION:-glm}"
DIR="$(cd "$(dirname "$0")" && pwd)"
WS="$(cd "$WS" && pwd)"; PROMPT="$(realpath "$PROMPT")"
LOG="$WS/$NAME.log"
: >"$LOG"

tmux has-session -t "$SESSION" 2>/dev/null || tmux new-session -d -s "$SESSION" -n home
tmux new-window -t "$SESSION" -n "$NAME" \
  "'$DIR/or_worker.sh' '$WS' '$PROMPT' '$LOG'; echo '── worker exited (rc='\$?')'; sleep infinity"
tmux split-window -t "$SESSION:$NAME" -h -l '65%' "'$DIR/watch_worker.sh' '$LOG'"
echo "spawned '$NAME' in tmux session '$SESSION' · log: $LOG"
echo "watch: tmux attach -t $SESSION   (window: $NAME)"
