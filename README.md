# doom-in-claude

Play Doom inside Claude Code. The actual Doom engine renders to your status line, you move by typing keys into the chat box, and an MCP server lets Claude itself play the game.

```
┌───────────────────────────────────────────────┐
│  Claude Code                                  │
│                                               │
│  you: wwwf                                    │
│  claude: ...                                  │
│                                               │
│  [ status line below renders real Doom here ] │
│                                               │
│  ┌─────────────────────────────────────────┐  │
│  │ DOOM E1M1   HP 87  ARM 0  PISTOL 34/200 │  │
│  │ ▓▓▓▓▓▓▓▓▓▓▓  █▓▓▓▓▓█  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ │  │
│  │ ▓░▒▒░▒▓░░▓▓  ▓░▒░░░▓  ▓▓▒▒░░▒▒▒░░░▒▒▓▓ │  │
│  │ ... (28 rows of chafa-rendered frame) . │  │
│  └─────────────────────────────────────────┘  │
└───────────────────────────────────────────────┘
```

## What this is

Four native Claude Code surfaces cooperate to host a real Doom game:

| Surface | Role |
| --- | --- |
| **Status line** (`refreshInterval: 1`) | Renders each frame as ANSI art in the bottom bar |
| **UserPromptSubmit hook** | Captures your keystrokes (`w`, `a`, `s`, `d`, `f`, ...) from the chat input |
| **MCP server** | Exposes `doom_look`, `doom_move`, `doom_state`, etc. so Claude can play |
| **Slash command** `/doom` | Controls the daemon (start, stop, status, preset, render mode, gamma) |

A background daemon holds the game state. Two daemon backends are provided:

- **`doomgeneric` backend** — real Doom. Built from [doomgeneric](https://github.com/ozkl/doomgeneric), wrapped with a tick-on-demand controller. C + Makefile, requires a toolchain.
- **`stub` backend** — a pure-Node fake Doom that implements the same file contract. Works out of the box without compiling anything. Use it to verify the integration.

## Architecture

```
                ┌─────────────────────────────────────────┐
                │  daemon (doomgeneric OR stub)           │
                │  ─ reads:  /tmp/doom-in-claude/input.fifo│
                │  ─ writes: frame.ansi, frame.png,       │
                │            state.json                    │
                └──────┬────────────┬──────────────┬──────┘
                       │            │              │
               ┌───────▼──┐  ┌──────▼─────┐  ┌─────▼──────┐
               │Status    │  │UserPrompt- │  │MCP server  │
               │line      │  │Submit hook │  │(stdio)     │
               │(display) │  │(your keys) │  │(Claude's   │
               │          │  │            │  │  tools)    │
               └──────────┘  └────────────┘  └────────────┘
```

All four Claude Code surfaces are documented, stable extension points. The daemon is an ordinary background process.

## Install

Requires: macOS or Linux, `bash`, `node >= 18`, `jq`, `perl` (for the byte-stability normalizer), and (for real Doom) `gcc`/`clang` + `make`. Also recommended: `chafa` (`brew install chafa`) — the default renderer.

```bash
git clone https://github.com/erkandogan/doom-in-claude-code.git
cd doom-in-claude-code
./install.sh
```

The installer:

1. Copies the status line script, hook, slash command, and MCP server to `~/.claude/`.
2. Patches `~/.claude/settings.json` to register `statusLine`, `hooks.UserPromptSubmit`, and the `doom-in-claude` MCP server.
3. Attempts to build the `doomgeneric` backend. If the build fails or `gcc` is missing, falls back to the stub backend.
4. Installs the `doom-in-claude` CLI to `~/.local/bin/`.
5. Downloads the [Freedoom](https://freedoom.github.io/) WADs (GPL, free to redistribute).

Uninstall with `./install.sh --uninstall`.

## Recommended setup (important)

Claude Code's Ink-based renderer drifts under the byte volume of a live Doom frame. After extensive debugging (details in `docs/`), the drift-free setup is:

### 1. Run Claude Code inside tmux

tmux absorbs Ink's cell-diff drift by re-parsing output into its own terminal-state model. Without tmux, the statusline corrupts after heavy scene changes and only a full Claude Code restart clears it. With tmux, it stays stable indefinitely.

Your `~/.tmux.conf` should include:

```tmux
set -g default-terminal "tmux-256color"
set -sa terminal-overrides ",xterm*:Tc"
set -sa terminal-overrides ",iterm*:Tc"
set -sa terminal-overrides ",kitty*:Tc"
set -sa terminal-overrides ",ghostty*:Tc"
set -sa terminal-overrides ",alacritty*:Tc"
set -sa terminal-features ",*:RGB"
set-environment -g COLORTERM "truecolor"
```

### 2. Restore TrueColor for Claude Code's UI inside tmux

Claude Code has an (undocumented but functional) defensive clamp to 256-color when `$TMUX` is set. Disable it:

```bash
# Add to ~/.zshrc (or ~/.bashrc)
export CLAUDE_CODE_TMUX_TRUECOLOR=1
```

This restores TrueColor for Claude's OWN TUI. The Doom frame renders in 256-palette regardless (see below) — that's intentional for stability — but Claude's prompt / diff / syntax highlighting use the full gamut.

See [anthropics/claude-code#46146](https://github.com/anthropics/claude-code/issues/46146) for the env var reference.

### 3. Recommended play command

```
/doom start --gamma 0.7
```

- **Default renderer**: chafa with 256-color palette + ordered dither — half the per-cell SGR byte cost of TrueColor, drift-resistant under heavy scene changes (damage flashes, fireballs).
- **`--gamma 0.7`**: lifts Doom's dark corridor values out of the single "black cube entry" so walls and textures are legible. Without it, dark rooms collapse to monochrome. Range 0.5–1.0, 0.7 is the sweet spot.

## Usage

In a Claude Code session:

```
/doom start --gamma 0.7    # start the daemon with recommended settings
wwwasdw                    # type keys as a single prompt → you move
/doom look                 # print the current frame
/doom stop
```

### Input language

The hook interprets chat submits as Doom commands whenever the daemon is active:

| Prompt | Action |
| --- | --- |
| `w` / `a` / `s` / `d` | forward / strafe-left / back / strafe-right |
| `q` / `e` | turn left / turn right |
| `f` | fire current weapon |
| `space` or `u` | use (open door, press switch) |
| digits `1`..`7` | switch weapon |
| a run of characters | each char is one action, executed in order (`wwwf` = walk 3 then fire) |
| `rush`, `retreat`, ... | named macros from `~/.claude/doom/macros.json` |
| `go <n>` | let `n` ticks elapse with no input (enemies move) |
| anything else | passes through to Claude as normal |

### Letting Claude play

With the MCP server running, the full tool surface is:

- `doom_state` — game state JSON (hp, armor, weapon, ammo, position, killcount, map, tick)
- `doom_look` — current frame as PNG (Claude can see the scene)
- `doom_move` — send keys
- `doom_burst` — send multiple key sequences in order
- `doom_wait` — advance N ticks with no input
- `doom_macro` — run a named macro
- `doom_start` / `doom_stop` / `doom_status` — daemon lifecycle

A prompt that reliably gets Claude through E1M1 is in [docs/claude-play.md](docs/claude-play.md).

### Coach mode

You play; Claude watches via `doom_state` on each of your moves and comments in chat:

> "Shotgun behind you, two imps in the left corridor. Duck left before firing."

Just ask Claude to watch while you play.

## Renderer options

```
/doom start [--render MODE] [--chafa-preset NAME]
            [--blocks-colors 256|full] [--gamma VALUE]
            [--cols N] [--rows N]
```

### Render modes

| Mode | Native? | Notes |
|---|---|---|
| `chafa` (default) | external | gold-standard quality; auto-selected if installed |
| `blocks-hq` | yes | area-averaged native C renderer, no external deps |
| `timg` | external | `brew install timg` — different color quantization |
| `quadrant` | yes | simpler native point-sampled blocks |
| `sextant` | yes | Unicode 13 sextants (2×3 subpixels, drift risk outside tmux) |
| `sixel` / `kitty` | — | image protocols; confirmed blocked by Ink |

### Chafa presets

| Preset | Colors | Dither | Notes |
|---|---|---|---|
| `default` / `balanced` | 256 | ordered 4×4 | **recommended** — drift-safe, good colors |
| `detailed` | 256 | ordered 2×2 | + preprocess + avg extractor, pair with `--cols 160` |
| `sextant-tmux` | 256 | ordered | 2×3 subpixel density, **requires tmux** |
| `rich` | full | ordered | TrueColor gamut, moderate drift risk |
| `vivid` | full | diffusion | best first-frame colors, drift under heavy scenes |
| `sharp` / `fast` | 256 | none | flat colors, no dither noise |

### Gamma (`--gamma VALUE`)

Applies a pre-quantization gamma curve:
- `< 1.0` brightens midtones (recommended for Doom: `0.6`–`0.8`)
- `1.0` no change (default)
- `> 1.0` darkens

Zero byte-count cost — the LUT is per-pixel, not per-frame. Drift risk unchanged.

### Blocks-hq color depth

`--blocks-colors 256` switches the native renderer to xterm-palette output for the smallest possible byte stream (~78 KB frames). Default is TrueColor (138 KB).

## Limits (inherent)

- **Turn-based cadence.** One Enter press per decision. You can't hold W to run — you type `www` + Enter. Use `go N` to auto-tick.
- **Passive refresh at 3.3 Hz.** The daemon writes a new frame every 300 ms; the statusline reads on refresh + events.
- **~28 rows × 120 cols** of frame at default size, tunable via `--cols`/`--rows`.
- **Claude Code's Ink renderer drifts without tmux.** The extensive internal mitigations (byte-stable cells, static black border, fixed-width HUD) slow drift but don't eliminate it outside tmux. tmux is the structural fix.
- **MCP screenshots cost tokens.** `doom_look` returns a PNG (~1–2k tokens). Use `doom_state` first, `doom_look` when stuck.

## Why this architecture

Claude Code's TUI does not expose a "custom panel" API. Status lines, hooks, slash commands, and MCP are the documented extension surfaces. This project composes all four to create something that *feels* like a built-in Doom panel without patching the binary.

A full rationale, including 15+ alternative approaches that don't work, is in [docs/design.md](docs/design.md).

## Contributing

The stub backend is the reference contract. A new real-Doom backend (e.g., Chocolate Doom fork, WASM Doom) implements the same file interface:

- Read tick-delimited commands from `/tmp/doom-in-claude/input.fifo`
- Write an ANSI frame to `frame.ansi` after each tick
- Write a PNG screenshot to `frame.png`
- Write game state to `state.json` (see [docs/state-schema.json](docs/state-schema.json))

## License

MIT. See [LICENSE](LICENSE).

Doom itself is not included. This project is not affiliated with id Software or Bethesda. Use [Freedoom](https://freedoom.github.io/) (GPL) for a legally redistributable WAD, or point the daemon at your own licensed `doom.wad` / `doom2.wad`.
