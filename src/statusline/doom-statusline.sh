#!/usr/bin/env bash
# doom-in-claude status line script.
# Reads the latest Doom frame and prints it. Falls back gracefully if the
# daemon isn't running so your terminal never goes blank.
#
# Claude Code pipes a JSON blob on stdin; we don't need it, but we drain it
# so stdin close doesn't surprise anyone.

set -u
cat >/dev/null 2>&1 || true

STATE_DIR="${DIC_STATE_DIR:-/tmp/doom-in-claude}"
FRAME="$STATE_DIR/frame.ansi"
PID_FILE="$STATE_DIR/daemon.pid"

# Is the daemon alive?
daemon_alive() {
  [ -f "$PID_FILE" ] || return 1
  local pid
  pid=$(cat "$PID_FILE" 2>/dev/null) || return 1
  kill -0 "$pid" 2>/dev/null
}

if ! daemon_alive; then
  printf '\x1b[2m[doom-in-claude idle — /doom start to play]\x1b[0m\n'
  exit 0
fi

if [ -f "$FRAME" ]; then
  # Leading SGR reset neutralizes whatever state Ink's previous draw left
  # us in, so cell 1 of the frame always paints on top of a known-default
  # terminal state.
  printf '\x1b[0m'
  cat "$FRAME"
else
  printf '\x1b[2m[doom daemon starting...]\x1b[0m\n'
fi
