#!/usr/bin/env node
// Patch / unpatch ~/.claude/settings.json for doom-in-claude.
//
// --patch   <settings> --statusline X --hook Y --mcp-server Z
// --unpatch <settings>
//
// Every field we add is tagged so --unpatch removes only our additions and
// leaves anything the user configured intact.

'use strict';

const fs = require('fs');
const path = require('path');

const MARKER = 'doom-in-claude';
const MCP_KEY = 'doom-in-claude';
const BACKUP_SUFFIX = '.dic-backup.json';

function readJson(p) {
  if (!fs.existsSync(p)) return {};
  const raw = fs.readFileSync(p, 'utf8').trim();
  if (!raw) return {};
  try { return JSON.parse(raw); } catch (e) {
    throw new Error(`Cannot parse ${p}: ${e.message}`);
  }
}

function writeJson(p, obj) {
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, JSON.stringify(obj, null, 2) + '\n');
}

function parseArgs(argv) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith('--')) { out[a.slice(2)] = argv[i + 1]; i++; }
    else out._.push(a);
  }
  return out;
}

function patch(args) {
  const settingsPath = args.patch;
  const statusline   = args.statusline;
  const hook         = args.hook;
  const mcpServer    = args['mcp-server'];
  const mcpJsonPath  = args['mcp-json'];
  const backupPath   = settingsPath + BACKUP_SUFFIX;

  const settings = readJson(settingsPath);

  // ─── Snapshot previous values we might overwrite ─────────────────────
  const backup = fs.existsSync(backupPath) ? readJson(backupPath) : {};
  if (!('statusLine' in backup)) {
    backup.statusLine = settings.statusLine ? JSON.parse(JSON.stringify(settings.statusLine)) : null;
  }
  writeJson(backupPath, backup);

  // ─── statusLine ──────────────────────────────────────────────────────
  if (statusline) {
    settings.statusLine = {
      type: 'command',
      command: statusline,
      refreshInterval: 1,
      _managedBy: MARKER,
    };
  }

  // ─── hooks.UserPromptSubmit ──────────────────────────────────────────
  if (hook) {
    settings.hooks = settings.hooks || {};
    const list = Array.isArray(settings.hooks.UserPromptSubmit) ? settings.hooks.UserPromptSubmit : [];
    const filtered = list.filter(h => !(h && h._managedBy === MARKER));
    filtered.unshift({
      matcher: '.*',
      hooks: [
        {
          type: 'command',
          command: hook,
          _managedBy: MARKER,
        },
      ],
      _managedBy: MARKER,
    });
    settings.hooks.UserPromptSubmit = filtered;
  }

  // Clean any stray mcpServers entry left by prior patcher versions.
  if (settings.mcpServers && settings.mcpServers[MCP_KEY]) {
    delete settings.mcpServers[MCP_KEY];
    if (Object.keys(settings.mcpServers).length === 0) delete settings.mcpServers;
  }

  writeJson(settingsPath, settings);
  console.log(`Patched ${settingsPath}`);

  // ─── MCP server lives in ~/.mcp.json, not settings.json ──────────────
  if (mcpServer && mcpJsonPath) {
    const mcpBackupPath = mcpJsonPath + BACKUP_SUFFIX;
    const mcp = readJson(mcpJsonPath);
    if (!fs.existsSync(mcpBackupPath)) writeJson(mcpBackupPath, mcp);
    mcp.mcpServers = mcp.mcpServers || {};
    mcp.mcpServers[MCP_KEY] = {
      command: 'node',
      args: [mcpServer],
      env: {},
      _managedBy: MARKER,
    };
    writeJson(mcpJsonPath, mcp);
    console.log(`Patched ${mcpJsonPath}`);
  }
}

function unpatch(args) {
  const settingsPath = args.unpatch;
  const mcpJsonPath  = args['mcp-json'];
  const backupPath   = settingsPath + BACKUP_SUFFIX;
  if (!fs.existsSync(settingsPath)) {
    console.log('No settings.json to unpatch.');
  } else {
    const settings = readJson(settingsPath);
    const backup   = fs.existsSync(backupPath) ? readJson(backupPath) : {};

    if (settings.statusLine && settings.statusLine._managedBy === MARKER) {
      if (backup.statusLine) settings.statusLine = backup.statusLine;
      else delete settings.statusLine;
    }

    if (settings.hooks && Array.isArray(settings.hooks.UserPromptSubmit)) {
      settings.hooks.UserPromptSubmit = settings.hooks.UserPromptSubmit.filter(
        h => !(h && h._managedBy === MARKER)
      );
      if (settings.hooks.UserPromptSubmit.length === 0) delete settings.hooks.UserPromptSubmit;
      if (Object.keys(settings.hooks).length === 0) delete settings.hooks;
    }

    // Legacy cleanup in case an older patcher wrote mcpServers into settings.json.
    if (settings.mcpServers && settings.mcpServers[MCP_KEY]) {
      delete settings.mcpServers[MCP_KEY];
      if (Object.keys(settings.mcpServers).length === 0) delete settings.mcpServers;
    }

    writeJson(settingsPath, settings);
    try { fs.unlinkSync(backupPath); } catch {}
    console.log(`Unpatched ${settingsPath}`);
  }

  if (mcpJsonPath && fs.existsSync(mcpJsonPath)) {
    const mcp = readJson(mcpJsonPath);
    if (mcp.mcpServers && mcp.mcpServers[MCP_KEY]) {
      delete mcp.mcpServers[MCP_KEY];
      if (Object.keys(mcp.mcpServers).length === 0) delete mcp.mcpServers;
    }
    writeJson(mcpJsonPath, mcp);
    console.log(`Unpatched ${mcpJsonPath}`);
  }
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.patch)   return patch(args);
  if (args.unpatch) return unpatch(args);
  console.error('Usage: patch-settings.js --patch <file> [options] | --unpatch <file>');
  process.exit(2);
}

main();
