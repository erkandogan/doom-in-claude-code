# Letting Claude play

Once `/doom start` is running, you can ask Claude to play. It has six MCP tools (see [`src/mcp/server.js`](../src/mcp/server.js)).

## Prompting patterns that work

**"Cheap first, expensive only when stuck."**

```
Play through the current level. Call doom_state every move to check HP / ammo
/ enemy bearings. Only call doom_look (PNG) if doom_state seems inconsistent
or you lose track of where you are.
```

**"Hard exit conditions."** Claude will otherwise chew tokens forever:

```
Play up to 50 moves or until HP ≤ 20 or level is clear. Then stop and
report what happened.
```

**"Structured plan first."**

```
Before moving, call doom_state. Then describe your plan in one sentence.
Then execute one doom_move. Repeat.
```

## Coach mode

Often more fun than autoplay.

```
I'm going to play. On each of my moves, call doom_state and tell me only
things I'd miss — enemies behind me, low ammo warnings, secrets. Keep it to
one line per move.
```

## Known failure modes

- **Claude walks into walls.** Doom's geometry is 3-D; the state JSON only exposes 2-D bearings. Screenshots help but cost tokens.
- **Claude picks the wrong weapon.** Reinforce: "Check ammo counts before switching."
- **Loops.** If Claude stalls in a corner, tell it to `doom_burst 10` (let the world move) and try a different direction.
