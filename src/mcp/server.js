#!/usr/bin/env node
// doom-in-claude MCP server.
// Exposes tools that let Claude see and play the Doom session:
//   doom_status    is the daemon running?
//   doom_look      return current frame (text + image if present)
//   doom_state     return structured game state JSON
//   doom_move      send keystring(s) to the daemon
//   doom_macro     invoke a registered macro by name
//   doom_burst     auto-tick N frames (no input)
//   doom_wait      one idle tick
//   doom_start     start the daemon
//   doom_stop      stop the daemon
//
// Transport: stdio (standard for locally-launched MCP servers).

'use strict';

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const { Server } = require('@modelcontextprotocol/sdk/server/index.js');
const { StdioServerTransport } = require('@modelcontextprotocol/sdk/server/stdio.js');
const {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} = require('@modelcontextprotocol/sdk/types.js');

const STATE_DIR   = process.env.DIC_STATE_DIR || '/tmp/doom-in-claude';
const FIFO        = path.join(STATE_DIR, 'input.fifo');
const FRAME_ANSI  = path.join(STATE_DIR, 'frame.ansi');
const FRAME_TXT   = path.join(STATE_DIR, 'frame.txt');
const FRAME_PNG   = path.join(STATE_DIR, 'frame.png');
const STATE_JSON  = path.join(STATE_DIR, 'state.json');
const PID_FILE    = path.join(STATE_DIR, 'daemon.pid');
const MACRO_FILE  = process.env.DIC_MACRO_FILE || path.join(process.env.HOME || '', '.claude', 'doom', 'macros.json');
const CLI         = process.env.DIC_CLI || path.join(process.env.HOME || '', '.local', 'bin', 'doom-in-claude');

// ─── Helpers ─────────────────────────────────────────────────────────────

function daemonAlive() {
  if (!fs.existsSync(PID_FILE)) return false;
  const pid = parseInt(fs.readFileSync(PID_FILE, 'utf8').trim(), 10);
  if (!pid) return false;
  try { process.kill(pid, 0); return true; } catch { return false; }
}

function requireDaemon() {
  if (!daemonAlive()) {
    return {
      content: [{ type: 'text', text: 'Doom daemon is not running. Call doom_start first, or run `/doom start`.' }],
      isError: true,
    };
  }
  return null;
}

function writeFifo(line) {
  fs.writeFileSync(FIFO, line + '\n');
}

function readFrameText() {
  if (fs.existsSync(FRAME_TXT)) return fs.readFileSync(FRAME_TXT, 'utf8');
  if (fs.existsSync(FRAME_ANSI)) return fs.readFileSync(FRAME_ANSI, 'utf8');
  return '(no frame yet)';
}

function readStateJson() {
  if (!fs.existsSync(STATE_JSON)) return null;
  try { return JSON.parse(fs.readFileSync(STATE_JSON, 'utf8')); } catch { return null; }
}

function asText(obj) {
  return { content: [{ type: 'text', text: typeof obj === 'string' ? obj : JSON.stringify(obj, null, 2) }] };
}

// ─── Tool definitions ────────────────────────────────────────────────────

const TOOLS = [
  {
    name: 'doom_status',
    description: 'Check whether the Doom daemon is running. Returns { running, pid, state_dir }.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
  },
  {
    name: 'doom_start',
    description: 'Start the Doom daemon. Optional backend: "stub" (default, no compile) or "doomgeneric" (real Doom, requires build).',
    inputSchema: {
      type: 'object',
      properties: {
        backend: { type: 'string', enum: ['stub', 'doomgeneric'] },
        wad: { type: 'string', description: 'Path to a Doom WAD (only for doomgeneric backend).' },
      },
      additionalProperties: false,
    },
  },
  {
    name: 'doom_stop',
    description: 'Stop the Doom daemon.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
  },
  {
    name: 'doom_look',
    description: 'Return the current Doom frame. Text always; image attached if available. Prefer doom_state for cheap, structured reasoning; call doom_look when you need to see the scene.',
    inputSchema: {
      type: 'object',
      properties: {
        image: { type: 'boolean', description: 'Include PNG screenshot if available. Default false to save tokens.' },
      },
      additionalProperties: false,
    },
  },
  {
    name: 'doom_state',
    description: 'Return structured game state: HP, ammo, weapon, enemies with bearings/distances, items, messages. Token-cheap.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
  },
  {
    name: 'doom_move',
    description: 'Send a keystring to the daemon. Each char is one tick: w/a/s/d move, q/e turn, f fire, u use, 1-7 weapons. Example: "wwwf" walks three steps and fires.',
    inputSchema: {
      type: 'object',
      properties: {
        keys: { type: 'string', description: 'A run of game-key characters.' },
      },
      required: ['keys'],
      additionalProperties: false,
    },
  },
  {
    name: 'doom_macro',
    description: 'Run a named macro from ~/.claude/doom/macros.json.',
    inputSchema: {
      type: 'object',
      properties: {
        name: { type: 'string' },
      },
      required: ['name'],
      additionalProperties: false,
    },
  },
  {
    name: 'doom_burst',
    description: 'Advance the game N ticks without any input (lets enemies move / animations resolve).',
    inputSchema: {
      type: 'object',
      properties: {
        ticks: { type: 'integer', minimum: 1, maximum: 200 },
      },
      required: ['ticks'],
      additionalProperties: false,
    },
  },
  {
    name: 'doom_wait',
    description: 'Advance exactly one tick with no input.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
  },
];

// ─── Tool handlers ───────────────────────────────────────────────────────

async function callTool(name, args) {
  args = args || {};
  switch (name) {

    case 'doom_status': {
      const alive = daemonAlive();
      const pid = alive ? parseInt(fs.readFileSync(PID_FILE, 'utf8').trim(), 10) : null;
      return asText({ running: alive, pid, state_dir: STATE_DIR });
    }

    case 'doom_start': {
      if (daemonAlive()) return asText('Daemon already running.');
      const cliArgs = ['start'];
      if (args.backend) cliArgs.push('--backend', args.backend);
      if (args.wad) cliArgs.push('--wad', args.wad);
      try {
        const out = spawn(CLI, cliArgs, { detached: true, stdio: 'ignore' });
        out.unref();
        // Wait briefly for pid file to appear
        for (let i = 0; i < 20; i++) {
          await new Promise(r => setTimeout(r, 100));
          if (daemonAlive()) return asText('Daemon started.');
        }
        return asText('Daemon start requested but pid not detected yet; check `doom_status` in a moment.');
      } catch (e) {
        return { content: [{ type: 'text', text: `Failed to start daemon: ${e.message}` }], isError: true };
      }
    }

    case 'doom_stop': {
      if (!daemonAlive()) return asText('Daemon was not running.');
      try {
        const pid = parseInt(fs.readFileSync(PID_FILE, 'utf8').trim(), 10);
        process.kill(pid, 'SIGTERM');
        return asText('Sent SIGTERM to daemon.');
      } catch (e) {
        return { content: [{ type: 'text', text: `Failed to stop daemon: ${e.message}` }], isError: true };
      }
    }

    case 'doom_look': {
      const err = requireDaemon(); if (err) return err;
      const text = readFrameText();
      const content = [{ type: 'text', text }];
      if (args.image && fs.existsSync(FRAME_PNG)) {
        const data = fs.readFileSync(FRAME_PNG).toString('base64');
        content.push({ type: 'image', data, mimeType: 'image/png' });
      }
      return { content };
    }

    case 'doom_state': {
      const err = requireDaemon(); if (err) return err;
      const state = readStateJson();
      if (!state) return { content: [{ type: 'text', text: 'No state available yet.' }], isError: true };
      return asText(state);
    }

    case 'doom_move': {
      const err = requireDaemon(); if (err) return err;
      const keys = String(args.keys || '').trim();
      if (!/^[wasdqefu1-7 ]{1,60}$/.test(keys)) {
        return { content: [{ type: 'text', text: `Invalid keys: "${keys}". Allowed: wasdqefu1-7 and space.` }], isError: true };
      }
      writeFifo(keys);
      await new Promise(r => setTimeout(r, 150));
      const state = readStateJson();
      return asText({ sent: keys, state });
    }

    case 'doom_macro': {
      const err = requireDaemon(); if (err) return err;
      const name = String(args.name || '').trim();
      let macros = {};
      try { macros = JSON.parse(fs.readFileSync(MACRO_FILE, 'utf8')); } catch {}
      const expansion = macros[name];
      if (!expansion) return { content: [{ type: 'text', text: `Unknown macro "${name}".` }], isError: true };
      writeFifo(expansion);
      await new Promise(r => setTimeout(r, 150));
      return asText({ macro: name, expanded: expansion, state: readStateJson() });
    }

    case 'doom_burst': {
      const err = requireDaemon(); if (err) return err;
      const n = Math.max(1, Math.min(200, parseInt(args.ticks, 10) || 1));
      writeFifo(`go ${n}`);
      await new Promise(r => setTimeout(r, 150));
      return asText({ burst: n, state: readStateJson() });
    }

    case 'doom_wait': {
      const err = requireDaemon(); if (err) return err;
      writeFifo('wait');
      await new Promise(r => setTimeout(r, 100));
      return asText({ state: readStateJson() });
    }

    default:
      return { content: [{ type: 'text', text: `Unknown tool: ${name}` }], isError: true };
  }
}

// ─── Wire up MCP server ──────────────────────────────────────────────────

async function main() {
  const server = new Server(
    { name: 'doom-in-claude', version: '0.1.0' },
    { capabilities: { tools: {} } }
  );

  server.setRequestHandler(ListToolsRequestSchema, async () => ({ tools: TOOLS }));

  server.setRequestHandler(CallToolRequestSchema, async req => {
    try {
      return await callTool(req.params.name, req.params.arguments);
    } catch (e) {
      return { content: [{ type: 'text', text: `Internal error: ${e.message}` }], isError: true };
    }
  });

  const transport = new StdioServerTransport();
  await server.connect(transport);
  // Keep alive until transport closes.
}

main().catch(err => {
  process.stderr.write(`doom-in-claude MCP fatal: ${err.stack || err}\n`);
  process.exit(1);
});
