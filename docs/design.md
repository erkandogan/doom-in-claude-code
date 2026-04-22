# Design notes

## The goal, honestly stated

Run real Doom inside Claude Code's TUI. Not a sidecar in tmux, not a VS Code webview, not a screenshot demo — something that uses Claude Code's own extension surfaces to host a playable game.

## What Claude Code gives us

Four documented, stable extension points:

1. **Status line** — a shell command re-run on events and every `refreshInterval` seconds (min 1 s). Output is written to the bottom UI row. Stdout-only; no input.
2. **Hooks** — shell commands fired at lifecycle events (`UserPromptSubmit`, `PreToolUse`, etc.). Can block the event via exit code 2.
3. **Custom slash commands** — markdown prompt templates in `~/.claude/commands/`. Can embed bash output (`!`cmd``) at prompt-expansion time.
4. **MCP servers** — stdio/HTTP tool servers. Give Claude itself new tools it can invoke.

## What Claude Code does *not* give us

- A way to register a custom full-screen TUI panel like `/agents`. That component is built into Claude Code; no public extension API exposes the render tree.
- A raw keyboard input channel into an extension. Custom keybindings map to Claude Code actions, not arbitrary shell.
- A way to refresh the status line faster than 1 Hz on a timer (updates debounce to 300 ms, but only fire on assistant-message / permission-mode / vim-mode events).

## Therefore

The only path to "Doom inside Claude Code" composes all four surfaces:

- **Status line** = display. A script that cats our latest frame.
- **UserPromptSubmit hook** = input from the human. Chat-box submits that look like Doom keys are intercepted and forwarded to the game.
- **Slash command** = control plane. Start / stop / status.
- **MCP server** = input from Claude. Lets Claude play (or coach) via tool calls.

A background daemon owns the game state. The four surfaces are all readers/writers against a shared directory at `/tmp/doom-in-claude/`:

```
input.fifo    ← hook + MCP write here
frame.ansi    → statusline reads
frame.png     → MCP returns as image content
state.json    → MCP returns as structured tool result
daemon.pid    → liveness probe
```

## Why turn-based

Status line polls at 1 Hz. User prompts arrive at human-typing speed. There is no mechanism — without patching Claude Code — for the game to push frames faster than an event fires. The honest answer is: one Enter = one command = one or more ticks. Doom as a roguelike. Enemies don't move while you think.

The `go N` command and the `burst` MCP tool exist to let game time advance deliberately (hold position while the imp walks into view).

## Alternatives considered and rejected

| Approach | Why not |
| --- | --- |
| Render Doom at 35 FPS in the status line | Cap is 1 Hz; no event can trigger faster refresh |
| Patch Claude Code to add a Doom panel | Breaks on every update; not shareable; violates "install and play" |
| Terminal-takeover from a subprocess (raw mode on /dev/tty) | Fights Claude Code's Ink renderer; corrupts terminal state; double-read of keys |
| tmux sidecar pane | Not "inside" Claude Code; degrades to "Doom next to Claude Code" |
| VS Code webview extension | CLI users excluded; not the native surface |
| Agent SDK custom TUI wrapper | Clean and works, but it *replaces* Claude Code instead of extending it |
| MCP UI resources / elicitation | Not a documented render channel in Claude Code |
| Keybindings to arbitrary shell | Keybindings map to Claude Code actions only |
| Claude Code hook that blocks indefinitely and reads /dev/tty | Blocks the entire session; no clean exit |

## Known limits

- 1 Hz passive refresh; ~300 ms active refresh after each interaction
- ~12 × 80 ANSI cells for the frame (≈ 80 × 24 effective pixels via half-blocks)
- One Enter per decision; no continuous key-hold
- Sound is the daemon's problem — safe defaults mute it
- MCP screenshot costs tokens; prefer `doom_state` for cheap reasoning

## Why the stub backend exists

Compiling doomgeneric is non-trivial and platform-specific. The stub daemon is a pure-Node toy that honors the same file contract. Users get something playable in 30 seconds, even without a C toolchain. It also serves as executable documentation of the contract any backend must implement.
