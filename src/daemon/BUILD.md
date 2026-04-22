# Building the doomgeneric backend

This backend runs the actual Doom engine (via [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric)) in pause-step mode: the game advances only when a command arrives on the input FIFO. It renders a 640×400 framebuffer into 24-bit ANSI half-blocks for the status line.

## Build

```bash
cd src/daemon
make
```

This clones `ozkl/doomgeneric` into `src/daemon/doomgeneric/`, compiles the Doom source files, links them with `wrapper.c`, and produces `./doomd`.

Requirements:

- `make`
- A C compiler (`gcc`, `clang`)
- `git`
- `pthread` (POSIX; shipped on macOS and every Linux)
- A Doom IWAD. Get Freedoom (free, GPL-licensed) with:
  ```bash
  ../../scripts/fetch-freedoom.sh
  ```

## Run directly

```bash
./doomd -iwad ../../assets/freedoom1.wad
```

In another shell (or from the MCP server) write commands to the FIFO:

```bash
echo 'wwwwf' > /tmp/doom-in-claude/input.fifo
cat /tmp/doom-in-claude/frame.ansi
```

## Run via the CLI

```bash
doom-in-claude start --backend doomgeneric --wad $(pwd)/../../assets/freedoom1.wad
```

## Status of this backend

**Unverified.** The wrapper source is complete and compiles against a stock `ozkl/doomgeneric` clone, but the integrated end-to-end path has not been shaken out in this repo. Known likely-issues for first-time builders:

1. **Symbol references.** `wrapper.c` declares `extern int gametic` to report game tick in `state.json`. If the upstream repo renames this, replace with the current symbol (grep for `gametic` in `doomgeneric/g_game.c`).
2. **Richer state.** To export HP / ammo / weapon into `state.json`, include `doomgeneric/d_player.h` and read `players[0].health`, `.armorpoints`, `.readyweapon`, `.ammo[]`. See comments in `wrapper.c:write_state_json`.
3. **PNG screenshot.** Not produced yet. Easiest add: after each `DG_DrawFrame`, dump `DG_ScreenBuffer` as PPM and shell out to `ffmpeg -i - -f image2 frame.png`, or link `libpng`.
4. **SIGFPE on some levels.** Known doomgeneric issue on certain maps; upstream has fixes in open PRs.
5. **macOS linker.** If `-lpthread` fails, try removing it — pthread is in libc on modern macOS.

Contributions welcome. See [../../docs/contributing.md](../../docs/contributing.md).
