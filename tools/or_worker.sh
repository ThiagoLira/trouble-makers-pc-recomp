#!/usr/bin/env bash
# or_worker.sh — headless decomp worker on an OpenRouter model (default GLM 5.2)
# usage: OPENROUTER_API_KEY=... or_worker.sh <workspace-dir> <prompt-file> [logfile]
set -euo pipefail
WS="${1:?workspace}"; PROMPT_FILE="${2:?prompt-file}"; LOG="${3:-$WS/or_worker.log}"
: "${OPENROUTER_API_KEY:?export OPENROUTER_API_KEY first}"
MODEL="${OR_MODEL:-z-ai/glm-5.2}"
export ANTHROPIC_BASE_URL="https://openrouter.ai/api"
export ANTHROPIC_AUTH_TOKEN="$OPENROUTER_API_KEY"
export ANTHROPIC_API_KEY=""   # must be explicitly blank
export ANTHROPIC_DEFAULT_SONNET_MODEL="$MODEL" ANTHROPIC_DEFAULT_OPUS_MODEL="$MODEL"
export ANTHROPIC_DEFAULT_HAIKU_MODEL="$MODEL" CLAUDE_CODE_SUBAGENT_MODEL="$MODEL"
cd "$WS"
exec claude -p "$(cat "$PROMPT_FILE")" --model sonnet --permission-mode acceptEdits \
  --output-format stream-json --verbose \
  --allowedTools "Bash" "Read" "Edit" "Write" "Grep" "Glob" >"$LOG" 2>&1

