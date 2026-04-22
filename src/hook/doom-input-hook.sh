#!/usr/bin/env bash
# UserPromptSubmit hook for doom-in-claude.
#
# Claude Code pipes JSON on stdin:
#   { "hook_event_name": "UserPromptSubmit", "prompt": "<user text>", ... }
#
# We intercept prompts that look like Doom input (pure game-keys, `go N`,
# `wait`, `.`, or a registered macro) by writing them to the Doom daemon's
# input FIFO and blocking the prompt from reaching Claude. Everything else
# passes through untouched so normal chat is unaffected.
#
# Exit codes:
#   0  allow prompt through (default, not a Doom command)
#   2  block prompt (it was a Doom command, stderr is shown to user)

set -u

STATE_DIR="${DIC_STATE_DIR:-/tmp/doom-in-claude}"
FIFO="$STATE_DIR/input.fifo"
PID_FILE="$STATE_DIR/daemon.pid"
MACRO_FILE="${DIC_MACRO_FILE:-$HOME/.claude/doom/macros.json}"

# Read the full JSON input
INPUT="$(cat)"

# Extract the prompt. Use jq if available, else a crude fallback.
if command -v jq >/dev/null 2>&1; then
  PROMPT="$(printf '%s' "$INPUT" | jq -r '.prompt // empty' 2>/dev/null)"
else
  # Best-effort regex fallback. Works for the common case.
  PROMPT="$(printf '%s' "$INPUT" | sed -n 's/.*"prompt"[[:space:]]*:[[:space:]]*"\(.*\)"[[:space:]]*[,}].*/\1/p' | head -1)"
fi

# Nothing to do if prompt is empty.
[ -z "${PROMPT:-}" ] && exit 0

# Daemon must be alive to intercept.
daemon_alive() {
  [ -f "$PID_FILE" ] || return 1
  local pid
  pid=$(cat "$PID_FILE" 2>/dev/null) || return 1
  kill -0 "$pid" 2>/dev/null
}

if ! daemon_alive; then
  exit 0
fi

# Trim whitespace.
TRIMMED="$(printf '%s' "$PROMPT" | awk '{$1=$1; print}')"

# Case 1: `go N` — auto-tick N frames.
if [[ "$TRIMMED" =~ ^go[[:space:]]+[0-9]+$ ]]; then
  printf '%s\n' "$TRIMMED" >"$FIFO"
  echo "[doom] ticked: $TRIMMED" >&2
  exit 2
fi

# Case 2: `wait` / `.` — one idle tick.
if [ "$TRIMMED" = "wait" ] || [ "$TRIMMED" = "." ]; then
  printf '%s\n' "$TRIMMED" >"$FIFO"
  echo "[doom] waited one tick" >&2
  exit 2
fi

# Case 3: registered macro.
if [ -f "$MACRO_FILE" ] && command -v jq >/dev/null 2>&1; then
  MACRO=$(jq -r --arg k "$TRIMMED" '.[$k] // empty' "$MACRO_FILE" 2>/dev/null || true)
  if [ -n "$MACRO" ]; then
    printf '%s\n' "$MACRO" >"$FIFO"
    echo "[doom] macro: $TRIMMED → $MACRO" >&2
    exit 2
  fi
fi

# Case 4: pure game-key run. No spaces, only game chars, max 30 chars.
#         Chars: w a s d q e f u 1-7 space
if [[ "$TRIMMED" =~ ^[wasdqefu1-7]{1,30}$ ]]; then
  printf '%s\n' "$TRIMMED" >"$FIFO"
  echo "[doom] input: $TRIMMED" >&2
  exit 2
fi

# Not a Doom command. Pass through.
exit 0
