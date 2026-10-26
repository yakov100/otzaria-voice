// Checks how the plugin gets the voice service to the user, without Otzaria:
//     node plugins/dictation/test/serviceInstall.test.js
// The plugin is bundled from src/ as build.py does ("released") and run against
// a fake Otzaria that records every call.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const root = path.join(__dirname, '..');
const repo = path.join(root, '..', '..');
const RELEASED = ['service.js', 'feedback.js', 'dictation.js', 'toolbar.js', 'field_actions.js', 'main.js'];  // build.py, FILES
const bundle = '(function () {\n\'use strict\';\n' +
  RELEASED.map(f => fs.readFileSync(path.join(root, 'src', f), 'utf8')).join('\n') + '\n})();\n';

const INSTALLER = 'https://github.com/yakov100/otzaria-voice/releases/latest/download/otzaria-voice-windows-x64-without-models.exe';
const PAGE = 'https://github.com/yakov100/otzaria-voice/releases/latest';
const SAVED = 'C:\\Users\\u\\Downloads\\otzaria-voice-windows-x64-without-models.exe';
const FIELD_EVENT = 'search.fieldAction.invoked';  // field_actions.js

/** [status]: the service's /status answer, or null when it does not answer. [download]: network.download's answer. */
function load({ status, download }) {
  const handlers = {};
  const calls = [];
  async function* down() {
    throw new Error('connection refused');
  }
  async function* answer(obj) {
    yield { type: 'response', status: 200 };
    yield { type: 'data', body: JSON.stringify(obj) + '\n' };
  }
  const Otzaria = {
    on(event, cb) { handlers[event] = cb; },
    call(method, payload) {
      calls.push({ method, payload });
      if (method === 'network.fetchStream') {
        const p = payload.url.replace(/^http:\/\/127\.0\.0\.1:47811/, '');
        return status && p === '/status' ? answer(status) : down();
      }
      if (method === 'ui.showConfirm') return Promise.resolve({ success: true, data: { confirmed: true } });
      if (method === 'network.download') return Promise.resolve(download || { success: false, error: { code: 'error.forbidden' } });
      if (method === 'storage.get') return Promise.resolve({ success: true, data: null });
      return Promise.resolve({ success: true, data: {} });
    }
  };
  const ctx = { Otzaria, console, setTimeout, clearTimeout };
  vm.createContext(ctx);
  vm.runInContext(bundle, ctx);
  return { handlers, calls };
}

const settle = () => new Promise(r => setTimeout(r, 30));
const of = (calls, method) => calls.filter(c => c.method === method);

/** Boots the plugin on [platform] and presses the shortcut once. */
async function press(platform, opts) {
  const h = load(opts);
  h.handlers['plugin.boot']({ app: { platform } });
  await settle();
  await h.handlers['app.command']({ command: 'dictate' });  // the Ctrl+Alt+V shortcut
  await settle();
  return h.calls;
}

// Otzaria's check of a URL against a list of prefixes (matchingNetworkAllowlistPrefix in
// lib/plugins/models/plugin_network_allowlist.dart): the URL is the prefix, or lies under it after
// '/', '?' or '#'. Simplified port: scheme and host compared without case, the path with case.
function allowedBy(url, prefixes) {
  const u = new URL(url);
  return prefixes.some(p => {
    const q = new URL(p);
    if (u.protocol !== q.protocol || u.host.toLowerCase() !== q.host.toLowerCase()) return false;
    const rest = url.slice(url.indexOf(q.host) + q.host.length);
    const base = p.slice(p.indexOf(q.host) + q.host.length);
    return rest === base || (rest.startsWith(base) && '/?#'.includes(rest[base.length]));
  });
}
// The hops of the installer's download as GitHub serves them (checked 26.10.2026: the latest
// address answers 302 to releases/download/<tag>/...). The CDN hop after that is allowed by
// isGithubReleaseRedirectAllowed and needs no list.
const HOPS = [INSTALLER, 'https://github.com/yakov100/otzaria-voice/releases/download/service-v2.5.0/otzaria-voice-windows-x64-without-models.exe'];
// Otzaria's official allowlist (plugin_network_allowlist.txt, branch dev), our lines as of 26.10.2026.
const OFFICIAL = [
  'https://github.com/yakov100/otzaria-voice/releases/latest/download',
  'https://github.com/yakov100/otzaria-voice/releases/download',
  'https://api.github.com/repos/yakov100/otzaria-voice'
];

let failed = 0;
async function test(name, fn) {
  try {
    await fn();
  } catch (e) {
    failed++;
    console.log('FAIL', name, '\n  ', e.message);
  }
}

(async () => {
  await test('Windows, no service: Otzaria downloads the installer and says how to open it', async () => {
    const calls = await press('windows', { status: null, download: { success: true, data: { path: SAVED, filename: 'x.exe' } } });
    const dl = of(calls, 'network.download');
    assert.strictEqual(dl.length, 1, 'network.download calls: ' + dl.length);
    assert.strictEqual(dl[0].payload.url, INSTALLER);
    assert.strictEqual(of(calls, 'app.openUrl').length, 0, 'the download page opened as well');
    const told = of(calls, 'ui.showConfirm').map(c => c.payload);
    assert.ok(told[0].content.includes('אוצריא תוריד את תוכנת ההתקנה'), told[0].content);
    assert.ok(told.some(c => c.title === 'תוכנת ההתקנה ירדה' && c.content.includes(SAVED) && c.content.includes('הפעל בכל זאת')), JSON.stringify(told));
  });

  await test('Windows, no service, the microphone inside a search field: the same download', async () => {
    const h = load({ status: null, download: { success: true, data: { path: SAVED } } });
    h.handlers['plugin.boot']({ app: { platform: 'windows' } });
    await settle();
    await h.handlers[FIELD_EVENT]({ actionId: 'dictate', sessionId: 's1', text: '', selectionStart: 0 });
    await settle();
    const dl = of(h.calls, 'network.download');
    assert.strictEqual(dl.length, 1, 'network.download calls: ' + dl.length);
    assert.strictEqual(dl[0].payload.url, INSTALLER);
    assert.ok(of(h.calls, 'search.endFieldSession').length >= 1, 'the field session was not ended');
  });

  await test('Windows, the download fails: the download page opens instead', async () => {
    const calls = await press('windows', { status: null, download: { success: false, error: { code: 'error.forbidden' } } });
    assert.strictEqual(of(calls, 'network.download').length, 1);
    const opened = of(calls, 'app.openUrl');
    assert.strictEqual(opened.length, 1);
    assert.strictEqual(opened[0].payload.url, PAGE);
  });

  await test('other platforms and an unknown one: no download, the page opens', async () => {
    for (const platform of ['android', 'linux', 'macos', '']) {
      const calls = await press(platform, { status: null });
      assert.strictEqual(of(calls, 'network.download').length, 0, platform + ': downloaded');
      assert.strictEqual(of(calls, 'app.openUrl').length, 1, platform + ': page not opened');
    }
  });

  await test('Windows, an older service: the update is downloaded by Otzaria too', async () => {
    const calls = await press('windows', {
      status: { service: 'otzaria-voice', version: '2.2.0', platform: 'windows', runtime: true, pack: true },
      download: { success: true, data: { path: SAVED } }
    });
    const ask = of(calls, 'ui.showConfirm').map(c => c.payload);
    assert.ok(ask[0].title === 'עדכון לשירות הדיבור' && ask[0].content.includes('להוריד את העדכון עכשיו?'), JSON.stringify(ask[0]));
    assert.strictEqual(of(calls, 'network.download').length, 1);
    assert.strictEqual(of(calls, 'app.openUrl').length, 0);
  });

  await test('no toolbar button from 2.3.1; the shortcut and the field microphone stay', () => {
    const m = JSON.parse(fs.readFileSync(path.join(root, 'manifest.base.json'), 'utf8'));
    assert.ok(!m.contributes.startup.toolbarItems, 'toolbarItems is still declared');
    assert.ok(!m.permissions.includes('reader.toolbar'), 'reader.toolbar is still declared');
    const sc = m.contributes.startup.shortcuts.find(s => s.command === 'dictate');
    assert.ok(sc && sc.key === 'ctrl+alt+v', 'the Ctrl+Alt+V shortcut is missing');
    assert.ok(m.permissions.includes('app.shortcuts'), 'app.shortcuts is missing');
    assert.strictEqual(m.contributes.startup.searchFieldActions[0].id, 'dictate');
    assert.ok(!/reader\.updateToolbarItem|toolbar_item_clicked/.test(bundle), 'the code still uses the toolbar');
  });

  await test('the manifest lets Otzaria download the installer, every hop of it', () => {
    const m = JSON.parse(fs.readFileSync(path.join(root, 'manifest.base.json'), 'utf8'));
    assert.ok(m.permissions.includes('network.access'), 'network.access is missing');
    assert.ok(m.permissions.includes('network.localhost'), 'network.localhost is missing');
    assert.strictEqual(m.minAppVersion, '0.9.98.1', 'minAppVersion');
    assert.ok(m.permissions.includes('search.field_actions'), 'search.field_actions is missing');
    assert.strictEqual(m.contributes.startup.searchFieldActions[0].id, 'dictate');
    for (const hop of HOPS) {
      assert.ok(allowedBy(hop, m.network.allowlist), 'not in the manifest: ' + hop);
      assert.ok(allowedBy(hop, OFFICIAL), 'not in the official list: ' + hop);
    }
  });

  await test('read-aloud: its manifest lets every hop through too (the exact address alone did not)', () => {
    const m = JSON.parse(fs.readFileSync(path.join(repo, 'plugins', 'read-aloud', 'manifest.json'), 'utf8'));
    for (const hop of HOPS) assert.ok(allowedBy(hop, m.network.allowlist), 'not in the read-aloud manifest: ' + hop);
    assert.ok(!allowedBy(HOPS[1], [INSTALLER]), 'the old list would have let the second hop through');
  });

  if (failed) {
    console.log(failed + ' failed');
    process.exit(1);
  }
  console.log('serviceInstall: all passed');
})();
