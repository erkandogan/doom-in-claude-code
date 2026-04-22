#!/usr/bin/env node
// Stub Doom daemon. Implements the same file contract as the doomgeneric
// backend so the Claude Code integration can be verified without compiling C.
//
// Contract:
//   ${STATE_DIR}/input.fifo    ← read commands from here (one line per command)
//   ${STATE_DIR}/frame.ansi    → latest frame (ANSI art)
//   ${STATE_DIR}/frame.txt     → latest frame (plain ASCII)
//   ${STATE_DIR}/state.json    → { hp, armor, ammo, weapons, level, enemies, ... }
//   ${STATE_DIR}/daemon.pid    → pid file
//   ${STATE_DIR}/daemon.log    → stderr log

'use strict';

const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

const STATE_DIR = process.env.DIC_STATE_DIR || '/tmp/doom-in-claude';
const FIFO = path.join(STATE_DIR, 'input.fifo');
const FRAME_ANSI = path.join(STATE_DIR, 'frame.ansi');
const FRAME_TXT = path.join(STATE_DIR, 'frame.txt');
const STATE_JSON = path.join(STATE_DIR, 'state.json');
const PID_FILE = path.join(STATE_DIR, 'daemon.pid');
const LOG_FILE = path.join(STATE_DIR, 'daemon.log');

function ensureStateDir() {
  fs.mkdirSync(STATE_DIR, { recursive: true });
  if (!fs.existsSync(FIFO)) {
    try {
      execSync(`mkfifo ${FIFO}`);
    } catch {
      // fallback: use a regular file and poll
      fs.writeFileSync(FIFO, '');
    }
  }
  fs.writeFileSync(PID_FILE, String(process.pid));
}

// ─── Game state ──────────────────────────────────────────────────────────

const MAP = [
  '########################',
  '#......................#',
  '#..#####...............#',
  '#..#...#....Z..........#',
  '#..+...#...............#',
  '#..#####.........I.....#',
  '#......................#',
  '#...........@..........#',
  '#......................#',
  '#.....!........Z.......#',
  '#......................#',
  '########################',
];

const WEAPONS = ['fist', 'pistol', 'shotgun', 'chaingun', 'rocket', 'plasma', 'bfg'];
const FACINGS = ['N', 'E', 'S', 'W'];
const FACING_DX = { N: 0, E: 1, S: 0, W: -1 };
const FACING_DY = { N: -1, E: 0, S: 1, W: 0 };
const FACING_ARROW = { N: '^', E: '>', S: 'v', W: '<' };

const state = {
  tick: 0,
  level: 'STUB-E1M1',
  grid: MAP.map(r => r.split('')),
  player: { x: 12, y: 7, facing: 'N' },
  hp: 100,
  armor: 0,
  ammo: { bullets: 50, shells: 0, rockets: 0, cells: 0 },
  weapon: 'pistol',
  weapons_owned: ['fist', 'pistol'],
  enemies: [
    { kind: 'zombieman', x: 12, y: 3, hp: 20 },
    { kind: 'imp',       x: 17, y: 5, hp: 60 },
    { kind: 'zombieman', x: 15, y: 9, hp: 20 },
  ],
  items: [
    { kind: 'medikit',  x: 6,  y: 9 },
  ],
  messages: ['You are in a damp, stone corridor.'],
};

function enemyAt(x, y) {
  return state.enemies.find(e => e.hp > 0 && e.x === x && e.y === y);
}

function itemAt(x, y) {
  return state.items.find(i => i.x === x && i.y === y);
}

function cellAt(x, y) {
  if (y < 0 || y >= state.grid.length) return '#';
  const row = state.grid[y];
  if (x < 0 || x >= row.length) return '#';
  return row[x];
}

function passable(x, y) {
  const c = cellAt(x, y);
  return c === '.' || c === '!' || c === '+';
}

function pushMsg(msg) {
  state.messages.push(msg);
  if (state.messages.length > 3) state.messages.shift();
}

// ─── Commands ────────────────────────────────────────────────────────────

function rotate(delta) {
  const i = FACINGS.indexOf(state.player.facing);
  state.player.facing = FACINGS[(i + delta + 4) % 4];
}

function move(dx, dy) {
  const p = state.player;
  const nx = p.x + dx, ny = p.y + dy;
  const e = enemyAt(nx, ny);
  if (e) { pushMsg(`You bump into the ${e.kind}.`); return; }
  if (!passable(nx, ny)) { pushMsg('Ouch, wall.'); return; }
  p.x = nx; p.y = ny;
  const item = itemAt(nx, ny);
  if (item) {
    if (item.kind === 'medikit') {
      state.hp = Math.min(100, state.hp + 25);
      pushMsg('Picked up a medikit. +25 HP.');
    }
    state.items = state.items.filter(i => i !== item);
    state.grid[ny][nx] = '.';
  }
}

function forward(n = 1) {
  for (let i = 0; i < n; i++) {
    move(FACING_DX[state.player.facing], FACING_DY[state.player.facing]);
  }
}

function strafe(dir, n = 1) {
  const right = { N: 'E', E: 'S', S: 'W', W: 'N' }[state.player.facing];
  const left  = { N: 'W', W: 'S', S: 'E', E: 'N' }[state.player.facing];
  const f = dir === 'L' ? left : right;
  for (let i = 0; i < n; i++) {
    move(FACING_DX[f], FACING_DY[f]);
  }
}

function back(n = 1) {
  for (let i = 0; i < n; i++) {
    move(-FACING_DX[state.player.facing], -FACING_DY[state.player.facing]);
  }
}

function fire() {
  const dmg = { fist: 5, pistol: 10, shotgun: 30, chaingun: 12, rocket: 80, plasma: 18, bfg: 200 }[state.weapon] || 10;
  const ammoKind = { pistol: 'bullets', chaingun: 'bullets', shotgun: 'shells', rocket: 'rockets', plasma: 'cells', bfg: 'cells', fist: null }[state.weapon];
  if (ammoKind && state.ammo[ammoKind] <= 0) { pushMsg(`Out of ${ammoKind}.`); return; }
  if (ammoKind) state.ammo[ammoKind]--;
  // cast a ray in facing direction, hit first enemy
  const p = state.player, dx = FACING_DX[p.facing], dy = FACING_DY[p.facing];
  let x = p.x + dx, y = p.y + dy;
  while (cellAt(x, y) !== '#' && y >= 0 && y < state.grid.length) {
    const e = enemyAt(x, y);
    if (e) {
      e.hp -= dmg;
      if (e.hp <= 0) pushMsg(`You fragged the ${e.kind}!`);
      else pushMsg(`You hit the ${e.kind} for ${dmg}.`);
      return;
    }
    x += dx; y += dy;
  }
  pushMsg('Shot went wide.');
}

function use() {
  // open door in front
  const p = state.player;
  const tx = p.x + FACING_DX[p.facing], ty = p.y + FACING_DY[p.facing];
  if (cellAt(tx, ty) === '+') {
    state.grid[ty][tx] = '.';
    pushMsg('You open the door.');
  } else {
    pushMsg('Nothing to use here.');
  }
}

function switchWeapon(n) {
  const w = WEAPONS[n - 1];
  if (!w) return;
  if (!state.weapons_owned.includes(w)) { pushMsg(`You don't have the ${w}.`); return; }
  state.weapon = w;
  pushMsg(`Switched to ${w}.`);
}

// ─── Enemy AI (1 tick) ───────────────────────────────────────────────────

function enemyTurn() {
  for (const e of state.enemies) {
    if (e.hp <= 0) continue;
    const dx = Math.sign(state.player.x - e.x);
    const dy = Math.sign(state.player.y - e.y);
    const nx = e.x + dx, ny = e.y + dy;
    if (nx === state.player.x && ny === state.player.y) {
      const dmg = e.kind === 'imp' ? 8 : 5;
      state.hp -= dmg;
      pushMsg(`The ${e.kind} hits you for ${dmg}.`);
      if (state.hp <= 0) pushMsg('YOU DIED.');
    } else if (passable(nx, ny) && !enemyAt(nx, ny)) {
      e.x = nx; e.y = ny;
    }
  }
}

// ─── Rendering ───────────────────────────────────────────────────────────

const ANSI = {
  reset: '\x1b[0m',
  dim: '\x1b[2m',
  bold: '\x1b[1m',
  fg: (r, g, b) => `\x1b[38;2;${r};${g};${b}m`,
  bg: (r, g, b) => `\x1b[48;2;${r};${g};${b}m`,
};

function glyphAt(x, y) {
  const p = state.player;
  if (x === p.x && y === p.y) return { ch: FACING_ARROW[p.facing], fg: [120, 255, 120] };
  const e = enemyAt(x, y);
  if (e) {
    if (e.kind === 'imp')       return { ch: 'I', fg: [255, 120, 60] };
    if (e.kind === 'zombieman') return { ch: 'Z', fg: [200, 200, 120] };
    return { ch: 'E', fg: [255, 80, 80] };
  }
  const it = itemAt(x, y);
  if (it) return { ch: '!', fg: [255, 255, 100] };
  const c = cellAt(x, y);
  if (c === '#') return { ch: '#', fg: [130, 130, 140] };
  if (c === '+') return { ch: '+', fg: [200, 140, 60] };
  return { ch: '·', fg: [60, 60, 70] };
}

function renderAnsi() {
  const lines = [];
  const hpColor = state.hp > 66 ? ANSI.fg(120, 255, 120)
                : state.hp > 33 ? ANSI.fg(255, 220, 80)
                : ANSI.fg(255, 80, 80);
  const ammoKind = { pistol: 'bullets', chaingun: 'bullets', shotgun: 'shells', rocket: 'rockets', plasma: 'cells', bfg: 'cells', fist: null }[state.weapon];
  const ammoStr = ammoKind ? `${state.ammo[ammoKind]} ${ammoKind}` : '∞';
  lines.push(
    `${ANSI.bold}${ANSI.fg(255, 60, 60)}DOOM${ANSI.reset} ` +
    `${ANSI.dim}[${state.level}]${ANSI.reset}  ` +
    `HP ${hpColor}${state.hp}${ANSI.reset}  ` +
    `ARM ${state.armor}  ` +
    `WPN ${ANSI.fg(200, 200, 255)}${state.weapon}${ANSI.reset}  ` +
    `AMMO ${ammoStr}  ` +
    `FACE ${state.player.facing}`
  );
  for (let y = 0; y < state.grid.length; y++) {
    let row = '';
    for (let x = 0; x < state.grid[y].length; x++) {
      const g = glyphAt(x, y);
      row += ANSI.fg(g.fg[0], g.fg[1], g.fg[2]) + g.ch + ANSI.reset;
    }
    lines.push(row);
  }
  const msg = state.messages[state.messages.length - 1] || '';
  lines.push(`${ANSI.dim}> ${msg}${ANSI.reset}`);
  return lines.join('\n') + '\n';
}

function renderTxt() {
  const lines = [];
  const ammoKind = { pistol: 'bullets', chaingun: 'bullets', shotgun: 'shells', rocket: 'rockets', plasma: 'cells', bfg: 'cells', fist: null }[state.weapon];
  const ammoStr = ammoKind ? `${state.ammo[ammoKind]} ${ammoKind}` : 'inf';
  lines.push(`DOOM [${state.level}]  HP ${state.hp}  ARM ${state.armor}  WPN ${state.weapon}  AMMO ${ammoStr}  FACE ${state.player.facing}`);
  for (let y = 0; y < state.grid.length; y++) {
    let row = '';
    for (let x = 0; x < state.grid[y].length; x++) {
      row += glyphAt(x, y).ch;
    }
    lines.push(row);
  }
  const msg = state.messages[state.messages.length - 1] || '';
  lines.push(`> ${msg}`);
  return lines.join('\n') + '\n';
}

function snapshot() {
  return {
    backend: 'stub',
    tick: state.tick,
    level: state.level,
    hp: state.hp,
    armor: state.armor,
    weapon: state.weapon,
    weapons_owned: state.weapons_owned,
    ammo: state.ammo,
    player: state.player,
    enemies_alive: state.enemies.filter(e => e.hp > 0).map(e => ({
      kind: e.kind, x: e.x, y: e.y, hp: e.hp,
      bearing: bearingTo(e.x, e.y),
      distance: Math.hypot(e.x - state.player.x, e.y - state.player.y),
    })),
    items_visible: state.items.map(i => ({ kind: i.kind, x: i.x, y: i.y })),
    last_message: state.messages[state.messages.length - 1] || '',
    messages: state.messages.slice(),
  };
}

function bearingTo(x, y) {
  const dx = x - state.player.x, dy = y - state.player.y;
  const ang = (Math.atan2(dx, -dy) * 180 / Math.PI + 360) % 360;
  const labels = ['N','NE','E','SE','S','SW','W','NW'];
  return labels[Math.round(ang / 45) % 8];
}

function writeAll() {
  try { fs.writeFileSync(FRAME_ANSI, renderAnsi()); } catch (e) { log(e); }
  try { fs.writeFileSync(FRAME_TXT, renderTxt()); } catch (e) { log(e); }
  try { fs.writeFileSync(STATE_JSON, JSON.stringify(snapshot(), null, 2)); } catch (e) { log(e); }
}

function log(msg) {
  try { fs.appendFileSync(LOG_FILE, `[${new Date().toISOString()}] ${msg}\n`); } catch {}
}

// ─── Command dispatcher ──────────────────────────────────────────────────

function dispatch(cmd) {
  cmd = cmd.trim();
  if (!cmd) return;

  // macro: `go N`
  const goMatch = cmd.match(/^go\s+(\d+)$/i);
  if (goMatch) {
    const n = Math.min(parseInt(goMatch[1], 10), 200);
    for (let i = 0; i < n; i++) { state.tick++; enemyTurn(); }
    return;
  }

  // single named macros
  if (cmd === 'wait' || cmd === '.') { state.tick++; enemyTurn(); return; }

  // multi-char string: each char is a key
  for (const ch of cmd.toLowerCase()) {
    applyKey(ch);
    state.tick++;
    enemyTurn();
    if (state.hp <= 0) break;
  }
}

function applyKey(ch) {
  switch (ch) {
    case 'w': forward(); break;
    case 's': back(); break;
    case 'a': strafe('L'); break;
    case 'd': strafe('R'); break;
    case 'q': rotate(-1); break;
    case 'e': rotate(1); break;
    case 'f': fire(); break;
    case ' ':
    case 'u': use(); break;
    case '1': case '2': case '3': case '4':
    case '5': case '6': case '7':
      switchWeapon(parseInt(ch, 10)); break;
    default: /* ignore */
  }
}

// ─── Event loop ──────────────────────────────────────────────────────────

function startFifoLoop() {
  const rs = fs.createReadStream(FIFO, { encoding: 'utf8' });
  let buf = '';
  rs.on('data', chunk => {
    buf += chunk;
    let idx;
    while ((idx = buf.indexOf('\n')) !== -1) {
      const line = buf.slice(0, idx);
      buf = buf.slice(idx + 1);
      try { dispatch(line); writeAll(); }
      catch (e) { log(`dispatch error: ${e.stack || e}`); }
    }
  });
  rs.on('end', () => { setTimeout(startFifoLoop, 100); }); // FIFO writer closed, reopen
  rs.on('error', err => { log(`fifo error: ${err.message}`); setTimeout(startFifoLoop, 500); });
}

function main() {
  ensureStateDir();
  writeAll();
  log(`stub daemon started, pid=${process.pid}`);
  process.on('SIGTERM', () => { try { fs.unlinkSync(PID_FILE); } catch {} process.exit(0); });
  process.on('SIGINT',  () => { try { fs.unlinkSync(PID_FILE); } catch {} process.exit(0); });
  startFifoLoop();
}

main();
