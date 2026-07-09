#!/usr/bin/env bash
# watch_worker.sh — live human-readable view of a stream-json worker log
# usage: watch_worker.sh <logfile>   (follows the file; Ctrl-C to stop)
set -euo pipefail
LOG="${1:?logfile}"
tail -n +1 -F "$LOG" | jq -Rrj --unbuffered '
  fromjson? |
  if .type == "system" and .subtype == "init" then
    "── worker up · model=" + (.model // "?") + " · cwd=" + (.cwd // "?") + "\n\n"
  elif .type == "assistant" then
    (.message.content[]? |
      if .type == "text" then .text + "\n"
      elif .type == "tool_use" then
        "⏺ " + .name + "(" +
        ((.input.command // .input.file_path // .input.pattern // (.input|tostring))
          | tostring | gsub("\\n"; " ") | .[0:160]) + ")\n"
      else empty end)
  elif .type == "result" then
    "\n══ RESULT: " + (.subtype // "?") +
    " · turns=" + ((.num_turns // 0) | tostring) +
    " · $" + ((.total_cost_usd // 0) | tostring) + " ══\n" +
    (.result // "") + "\n"
  else empty end'
