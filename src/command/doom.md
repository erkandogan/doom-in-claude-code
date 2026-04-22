---
argument-hint: [start|stop|status|look|help] [--backend stub|doomgeneric] [--wad PATH]
description: Control the doom-in-claude daemon
allowed-tools: Bash(doom-in-claude *), Bash(cat *), Bash(~/.local/bin/doom-in-claude *)
---

# /doom — control the Doom session

Subcommands:
- `start` — launch the daemon (status line will start rendering the game)
- `stop` — kill the daemon
- `status` — report whether it's running
- `look` — print the current frame
- `help` — show input grammar

## Dispatch

!`~/.local/bin/doom-in-claude --from-slash $ARGUMENTS 2>&1 || true`

## Notes

When the daemon is running, the **UserPromptSubmit hook** intercepts your chat submits that look like Doom input. Type `wwwf` and press Enter to walk forward three tiles and fire. Type `go 10` to let ten ticks elapse with no input (enemies move).

If you want **Claude to play**, just say so in chat: "Play through E1M1." Claude has MCP tools (`doom_look`, `doom_state`, `doom_move`, etc.) and will loop.

If the daemon is not running, type `/doom start` first.
