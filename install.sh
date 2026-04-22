#!/usr/bin/env bash
# doom-in-claude installer.
#
# Installs the status line, UserPromptSubmit hook, slash command, MCP server,
# and CLI into ~/.claude and ~/.local/bin. Attempts to build the doomgeneric
# backend; falls back to the stub backend if the build fails.
#
# Usage:
#   ./install.sh              # install
#   ./install.sh --uninstall  # reverse
#   ./install.sh --skip-build # don't try to build doomgeneric

set -eu

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
CLAUDE_DIR="$HOME/.claude"
BIN_DIR="$HOME/.local/bin"
DOOM_DIR="$CLAUDE_DIR/doom"
SETTINGS="$CLAUDE_DIR/settings.json"
COMMANDS_DIR="$CLAUDE_DIR/commands"
HOOKS_DIR="$CLAUDE_DIR/hooks"
MCP_DIR="$CLAUDE_DIR/mcp"
MARKER="$DOOM_DIR/.installed-by-doom-in-claude"
MCP_JSON="$HOME/.mcp.json"

MODE="install"
SKIP_BUILD=0
while [ $# -gt 0 ]; do
  case "$1" in
    --uninstall) MODE="uninstall" ;;
    --skip-build) SKIP_BUILD=1 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

say() { printf '\033[36m[dic]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[dic]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31m[dic]\033[0m %s\n' "$*" >&2; exit 1; }

require() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required command: $1"
}

# ─── Uninstall ───────────────────────────────────────────────────────────

uninstall() {
  say "Stopping daemon (if running)..."
  "$BIN_DIR/doom-in-claude" stop 2>/dev/null || true

  say "Removing CLI symlink..."
  rm -f "$BIN_DIR/doom-in-claude" "$BIN_DIR/dic"

  say "Removing status line / hook / slash command / MCP..."
  rm -f "$DOOM_DIR/statusline.sh"
  rm -f "$DOOM_DIR/input-hook.sh"
  rm -f "$DOOM_DIR/mcp-server.js"
  rm -f "$COMMANDS_DIR/doom.md"
  rm -f "$MARKER"

  if [ -f "$SETTINGS" ] && command -v node >/dev/null 2>&1; then
    say "Unpatching settings.json and ~/.mcp.json..."
    node "$REPO_ROOT/scripts/patch-settings.js" --unpatch "$SETTINGS" --mcp-json "$MCP_JSON" \
      || warn "unpatch failed; you may need to edit settings.json / ~/.mcp.json manually."
  fi

  say "Uninstall complete."
}

# ─── Install ─────────────────────────────────────────────────────────────

install() {
  require bash
  require node
  command -v jq >/dev/null 2>&1 || warn "jq not found. Recommended for robust hook parsing. Install with: brew install jq"

  mkdir -p "$CLAUDE_DIR" "$COMMANDS_DIR" "$HOOKS_DIR" "$MCP_DIR" "$DOOM_DIR" "$BIN_DIR"

  say "Copying files..."
  cp -f "$REPO_ROOT/src/statusline/doom-statusline.sh" "$DOOM_DIR/statusline.sh"
  cp -f "$REPO_ROOT/src/hook/doom-input-hook.sh"       "$DOOM_DIR/input-hook.sh"
  cp -f "$REPO_ROOT/src/command/doom.md"               "$COMMANDS_DIR/doom.md"
  chmod +x "$DOOM_DIR/statusline.sh" "$DOOM_DIR/input-hook.sh"
  # The MCP server needs its node_modules; point MCP config at the repo
  # copy rather than copying to ~/.claude/doom (where modules wouldn't resolve).
  MCP_SERVER_PATH="$REPO_ROOT/src/mcp/server.js"
  rm -f "$DOOM_DIR/mcp-server.js"

  if [ ! -f "$DOOM_DIR/macros.json" ]; then
    cat >"$DOOM_DIR/macros.json" <<'JSON'
{
  "rush":    "wwwww",
  "retreat": "sssss",
  "spin":    "eeee",
  "salvo":   "fff",
  "peek":    "w wait wait wait"
}
JSON
  fi

  say "Installing CLI to $BIN_DIR..."
  ln -sf "$REPO_ROOT/bin/doom-in-claude" "$BIN_DIR/doom-in-claude"
  ln -sf "$REPO_ROOT/bin/doom-in-claude" "$BIN_DIR/dic"
  chmod +x "$REPO_ROOT/bin/doom-in-claude"

  say "Installing MCP dependency..."
  if [ ! -d "$REPO_ROOT/node_modules/@modelcontextprotocol" ]; then
    (cd "$REPO_ROOT" && npm install --silent --no-audit --no-fund >/dev/null 2>&1) || warn "npm install failed; MCP server may not start until you run 'npm install' in $REPO_ROOT."
  fi

  say "Patching ~/.claude/settings.json and ~/.mcp.json..."
  node "$REPO_ROOT/scripts/patch-settings.js" --patch "$SETTINGS" \
    --statusline "$DOOM_DIR/statusline.sh" \
    --hook       "$DOOM_DIR/input-hook.sh" \
    --mcp-server "$MCP_SERVER_PATH" \
    --mcp-json   "$MCP_JSON"

  if [ $SKIP_BUILD -eq 0 ]; then
    if command -v make >/dev/null 2>&1 && (command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1); then
      say "Attempting to build doomgeneric backend..."
      if (cd "$REPO_ROOT/src/daemon" && make 2>&1 | tail -5); then
        say "doomgeneric backend built."
      else
        warn "doomgeneric build failed. The stub backend will be used. You can retry later with: (cd $REPO_ROOT/src/daemon && make)"
      fi
    else
      warn "No C toolchain found. Using stub backend. Install gcc/clang + make to enable real Doom."
    fi
  fi

  touch "$MARKER"

  say ""
  say "─────────────────────────────────────────────"
  say "  doom-in-claude installed."
  say ""
  say "  Inside a Claude Code session, run:"
  say "    /doom start"
  say ""
  say "  Then type \`wwwf\` + Enter to walk and shoot."
  say "  Or ask Claude: \"Play through E1M1 using the doom_ tools.\""
  say "─────────────────────────────────────────────"
}

if [ "$MODE" = "uninstall" ]; then
  uninstall
else
  install
fi
