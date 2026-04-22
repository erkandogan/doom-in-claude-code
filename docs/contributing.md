# Contributing

## Adding a new backend

A backend is any process that:

1. Reads tick-delimited commands from `$DIC_STATE_DIR/input.fifo`.
2. Writes a frame to `frame.ansi` (and optionally `frame.txt`, `frame.png`) after each tick.
3. Writes state to `state.json` matching [state-schema.json](state-schema.json).
4. Writes its own PID to `daemon.pid` on startup and cleans up on exit.

The input grammar accepted from the FIFO:

- A run of game-key characters: `wasdqefu1-7` and space.
- `go N` — advance N ticks without input.
- `wait` / `.` — advance 1 tick.
- Macro expansions (the hook does the expansion before writing to the FIFO; the daemon sees only expanded text).

The stub ([`src/stub/daemon.js`](../src/stub/daemon.js)) is the reference — it's small and honest about what each rule means.

## Ideas that need a home

- **libpng screenshots** from the doomgeneric backend (so `doom_look` can return a real image).
- **Richer state.json** from the doomgeneric backend: read `players[0]` fields (HP/ammo/weapon), project nearby enemies from Doom's mobj list, include the level name from `gamemap`.
- **WASM Doom backend** (Node-driven) to avoid the C toolchain requirement on real Doom.
- **Smarter input parsing** — distinguish `wait` used as English word vs game command (currently heuristic).
- **Save/load slot support** exposed as MCP tools so Claude can checkpoint before risky moves.

## Running tests

There aren't any yet. Please add some.
