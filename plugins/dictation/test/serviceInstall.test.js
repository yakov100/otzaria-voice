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
const INSTALLER_FULL = 'https://github.com/yakov100/otzaria-voice/releases/latest/download/otzaria-voice-windows-x64-with-models.exe';
const PAGE = 'https://github.com/yakov100/otzaria-voice/releases/latest';
const SAVED = 'C:\\Users\\u\\Downloads\\otzaria-voice-windows-x64-with-models.exe';
const FIELD_EVENT = 'search.fieldAction.invoked';  // field_actions.js

/**
 * [status]: the service's /status answer, or null when it does not answer (or a function returning
 * either, asked at every /status). [download]: network.download's answer, or a function returning a
 * promise of it. [storage]: the plugin's stored keys (written to as well); [storageFails]: storage.get
 * throws. [confirm]: (title) => the user's answer to a dialog (default: confirmed). [timers]: replaces
 * setInterval/clearInterval.
 */
function load({ status, download, storage, storageFails, confirm, timers }) {
  const handlers = {};
  const calls = [];
  const store = storage || {};
  const statusNow = () => (typeof status === 'function' ? status() : status);
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
        const st = p === '/status' ? statusNow() : null;
        return st ? answer(st) : down();
      }
      if (method === 'ui.showConfirm') {
        return Promise.resolve({ success: true, data: { confirmed: confirm ? !!confirm(payload.title) : true } });
      }
      if (method === 'network.download') {
        if (typeof download === 'function') return download();
        return Promise.resolve(download || { success: false, error: { code: 'error.forbidden' } });
      }
      if (method === 'storage.get') {
        if (storageFails) return Promise.reject(new Error('storage unavailable'));
        return Promise.resolve({ success: true, data: store[payload.key] || null });
      }
      if (method === 'storage.set') store[payload.key] = payload.value;
      return Promise.resolve({ success: true, data: {} });
    }
  };
  const ctx = {
    Otzaria, console, setTimeout, clearTimeout,
    setInterval: (timers && timers.setInterval) || setInterval,
    clearInterval: (timers && timers.clearInterval) || clearInterval,
  };
  vm.createContext(ctx);
  vm.runInContext(bundle, ctx);
  return { handlers, calls, store };
}

const settle = () => new Promise(r => setTimeout(r, 30));
const of = (calls, method) => calls.filter(c => c.method === method);

/** Boots the plugin on [platform] and presses the microphone once. */
async function press(platform, opts) {
  const h = load(opts);
  h.handlers['plugin.boot']({ app: { platform } });
  await settle();
  await h.handlers['reader.toolbar_item_clicked']({ itemId: 'dictate' });
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
const HOPS_FULL = [INSTALLER_FULL, 'https://github.com/yakov100/otzaria-voice/releases/download/service-v2.5.3/otzaria-voice-windows-x64-with-models.exe'];
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
  const messages = (calls) => of(calls, 'ui.showMessage').map(c => c.payload.message);

  await test('Windows, no service ever seen: Otzaria downloads the full installer and says how to open it', async () => {
    const calls = await press('windows', { status: null, download: { success: true, data: { path: SAVED, filename: 'x.exe' } } });
    const dl = of(calls, 'network.download');
    assert.strictEqual(dl.length, 1, 'network.download calls: ' + dl.length);
    assert.strictEqual(dl[0].payload.url, INSTALLER_FULL);
    assert.strictEqual(of(calls, 'app.openUrl').length, 0, 'the download page opened as well');
    const told = of(calls, 'ui.showConfirm').map(c => c.payload);
    assert.ok(told[0].content.includes('אוצריא תוריד את תוכנת ההתקנה') && told[0].content.includes('כולל חבילת הדיבור') &&
      told[0].content.includes('כ-315 מגה-בייט') && told[0].content.includes('כמה דקות'), told[0].content);
    assert.ok(messages(calls).some(m => m.includes('אל תסגרו את אוצריא')), JSON.stringify(messages(calls)));
    assert.ok(told.some(c => c.title === 'תוכנת ההתקנה ירדה' && c.content.includes(SAVED) && c.content.includes('הפעל בכל זאת') &&
      c.content.includes('חבילת הדיבור כלולה') && c.content.includes('אפשר אז למחוק')), JSON.stringify(told));
  });

  await test('Windows, no service, the microphone inside a search field: the full download, and the field is freed at once', async () => {
    let finish;
    const pending = new Promise(r => { finish = r; });
    const h = load({ status: null, download: () => pending });
    h.handlers['plugin.boot']({ app: { platform: 'windows' } });
    await settle();
    h.handlers[FIELD_EVENT]({ actionId: 'dictate', sessionId: 's1', text: '', selectionStart: 0 });
    await settle();
    const dl = of(h.calls, 'network.download');
    assert.strictEqual(dl.length, 1, 'network.download calls: ' + dl.length);
    assert.strictEqual(dl[0].payload.url, INSTALLER_FULL);
    assert.ok(of(h.calls, 'search.endFieldSession').length >= 1, 'the field stayed busy while the installer downloads');
    finish({ success: true, data: { path: SAVED } });
    await settle();
    assert.ok(of(h.calls, 'ui.showConfirm').some(c => c.payload.title === 'תוכנת ההתקנה ירדה'), 'no word when the download ended');
  });

  await test('Windows, a second press while the installer downloads: no second download or question', async () => {
    let finish;
    const pending = new Promise(r => { finish = r; });
    const h = load({ status: null, download: () => pending });
    h.handlers['plugin.boot']({ app: { platform: 'windows' } });
    await settle();
    h.handlers['reader.toolbar_item_clicked']({ itemId: 'dictate' });
    await settle();
    h.handlers['reader.toolbar_item_clicked']({ itemId: 'dictate' });
    await settle();
    h.handlers[FIELD_EVENT]({ actionId: 'dictate', sessionId: 's2', text: '', selectionStart: 0 });
    await settle();
    assert.strictEqual(of(h.calls, 'network.download').length, 1, 'downloads: ' + of(h.calls, 'network.download').length);
    assert.strictEqual(of(h.calls, 'ui.showConfirm').length, 1, 'questions: ' + of(h.calls, 'ui.showConfirm').length);
    assert.ok(messages(h.calls).filter(m => m.includes('עדיין יורדת')).length >= 2, JSON.stringify(messages(h.calls)));
    finish({ success: true, data: { path: SAVED } });
    await settle();
  });

  await test('Windows, the service answered here before: the small installer', async () => {
    const calls = await press('windows', { status: null, storage: { serviceSeen: '1' }, download: { success: true, data: { path: SAVED } } });
    const dl = of(calls, 'network.download');
    assert.strictEqual(dl.length, 1, 'network.download calls: ' + dl.length);
    assert.strictEqual(dl[0].payload.url, INSTALLER);
    const told = of(calls, 'ui.showConfirm').map(c => c.payload);
    assert.ok(told[0].content.includes('כ-3 מגה-בייט') && !told[0].content.includes('חבילת הדיבור'), told[0].content);
    assert.ok(!told.some(c => c.content.includes('חבילת הדיבור כלולה')), JSON.stringify(told));
  });

  await test('Windows, a key older plugins kept only after the service answered: the small installer', async () => {
    for (const [key, value] of [['serviceUpdateDeclined', '2.5.1:1'], ['packUpdateDeclined', '4:1'], ['hotkeyTipShown', true]]) {
      const calls = await press('windows', { status: null, storage: { [key]: value }, download: { success: true, data: { path: SAVED } } });
      const dl = of(calls, 'network.download');
      assert.strictEqual(dl.length === 1 && dl[0].payload.url, INSTALLER, key + ': ' + JSON.stringify(dl.map(c => c.payload.url)));
    }
  });

  const ANSWERING = { service: 'otzaria-voice', version: '2.5.3', platform: 'windows', runtime: true, pack: false };

  await test('the service answers, then stops answering: the small installer, in the same engine and in a new one', async () => {
    let up = true;
    const h = load({ status: () => (up ? ANSWERING : null), download: { success: true, data: { path: SAVED } } });
    h.handlers['plugin.boot']({ app: { platform: 'windows' } });
    await settle();
    await h.handlers['reader.toolbar_item_clicked']({ itemId: 'dictate' });
    await settle();
    assert.ok(h.store.serviceSeen, 'serviceSeen was not stored: ' + JSON.stringify(h.store));
    // The same engine must know it without storage: the stored key is taken away first.
    delete h.store.serviceSeen;
    up = false;
    await h.handlers['reader.toolbar_item_clicked']({ itemId: 'dictate' });
    await settle();
    let dl = of(h.calls, 'network.download');
    assert.strictEqual(dl.length === 1 && dl[0].payload.url, INSTALLER, 'same engine: ' + JSON.stringify(dl.map(c => c.payload.url)));
    const later = await press('windows', { status: null, storage: { serviceSeen: '1' }, download: { success: true, data: { path: SAVED } } });
    dl = of(later, 'network.download');
    assert.strictEqual(dl.length === 1 && dl[0].payload.url, INSTALLER, 'new engine: ' + JSON.stringify(dl.map(c => c.payload.url)));
  });

  await test('Windows, storage cannot be read: taken as a first installation (the full installer)', async () => {
    const calls = await press('windows', { status: null, storageFails: true, download: { success: true, data: { path: SAVED } } });
    const dl = of(calls, 'network.download');
    assert.strictEqual(dl.length === 1 && dl[0].payload.url, INSTALLER_FULL, JSON.stringify(dl.map(c => c.payload.url)));
  });

  await test('Windows, a press after the full installer came down: points to the file, no second copy unless asked', async () => {
    const h = load({
      status: null,
      download: { success: true, data: { path: SAVED } },
      confirm: (title) => title !== 'תוכנת ההתקנה כבר ירדה',  // declines the second copy
    });
    h.handlers['plugin.boot']({ app: { platform: 'windows' } });
    await settle();
    await h.handlers['reader.toolbar_item_clicked']({ itemId: 'dictate' });
    await settle();
    assert.ok(h.store.fullInstallerSaved && h.store.fullInstallerSaved.path === SAVED, JSON.stringify(h.store));
    await h.handlers['reader.toolbar_item_clicked']({ itemId: 'dictate' });
    await settle();
    assert.strictEqual(of(h.calls, 'network.download').length, 1, 'a second copy came down');
    const again = of(h.calls, 'ui.showConfirm').map(c => c.payload).filter(p => p.title === 'תוכנת ההתקנה כבר ירדה');
    assert.ok(again.length === 1 && again[0].content.includes(SAVED) && again[0].content.includes('לחצו «אישור» כדי להוריד אותו שוב') && again[0].content.includes('אחרת לחצו «ביטול»'), JSON.stringify(again));
  });

  await test('Windows, a full installer saved in an earlier engine: within a day the file is pointed to, after it a new download', async () => {
    const fresh = await press('windows', {
      status: null, confirm: (title) => title !== 'תוכנת ההתקנה כבר ירדה',
      storage: { fullInstallerSaved: { path: SAVED, at: Date.now() - 3600 * 1000 } },
    });
    assert.strictEqual(of(fresh, 'network.download').length, 0, 'downloaded although the file came down an hour ago');
    assert.ok(of(fresh, 'ui.showConfirm').some(c => c.payload.title === 'תוכנת ההתקנה כבר ירדה'));
    const stale = await press('windows', {
      status: null, download: { success: true, data: { path: SAVED } },
      storage: { fullInstallerSaved: { path: SAVED, at: Date.now() - 25 * 3600 * 1000 } },
    });
    const dl = of(stale, 'network.download');
    assert.strictEqual(dl.length === 1 && dl[0].payload.url, INSTALLER_FULL, JSON.stringify(dl.map(c => c.payload.url)));
    assert.ok(!of(stale, 'ui.showConfirm').some(c => c.payload.title === 'תוכנת ההתקנה כבר ירדה'));
  });

  await test('Windows, the reminder while the full installer downloads stops when the download ends', async () => {
    const live = new Set();
    let next = 1;
    const timers = {
      setInterval: () => { const id = next++; live.add(id); return id; },
      clearInterval: (id) => { live.delete(id); },
    };
    let finish;
    const pending = new Promise(r => { finish = r; });
    const h = load({ status: null, download: () => pending, timers });
    h.handlers['plugin.boot']({ app: { platform: 'windows' } });
    await settle();
    h.handlers['reader.toolbar_item_clicked']({ itemId: 'dictate' });
    await settle();
    assert.strictEqual(live.size, 1, 'reminders running during the download: ' + live.size);
    finish({ success: false, error: { code: 'error.network' } });
    await settle();
    assert.strictEqual(live.size, 0, 'a reminder kept running after the download');
  });

  await test('Windows, the download fails: the user is told, and the download page opens instead', async () => {
    const calls = await press('windows', { status: null, download: { success: false, error: { code: 'error.forbidden' } } });
    assert.strictEqual(of(calls, 'network.download').length, 1);
    const opened = of(calls, 'app.openUrl');
    assert.strictEqual(opened.length, 1);
    assert.strictEqual(opened[0].payload.url, PAGE);
    assert.ok(messages(calls).some(m => m.includes('לא הצליחה')), JSON.stringify(messages(calls)));
  });

  await test('other platforms and an unknown one: no download, the page opens', async () => {
    for (const platform of ['android', 'linux', 'macos', '']) {
      const calls = await press(platform, { status: null });
      assert.strictEqual(of(calls, 'network.download').length, 0, platform + ': downloaded');
      assert.strictEqual(of(calls, 'app.openUrl').length, 1, platform + ': page not opened');
    }
  });

  await test('Windows, an older service: the update is downloaded by Otzaria too, the small installer', async () => {
    const calls = await press('windows', {
      status: { service: 'otzaria-voice', version: '2.2.0', platform: 'windows', runtime: true, pack: true },
      download: { success: true, data: { path: SAVED } }
    });
    const ask = of(calls, 'ui.showConfirm').map(c => c.payload);
    assert.ok(ask[0].title === 'עדכון לשירות הדיבור' && ask[0].content.includes('להוריד את העדכון עכשיו?') &&
      ask[0].content.includes('כ-3 מגה-בייט'), JSON.stringify(ask[0]));
    const dl = of(calls, 'network.download');
    assert.strictEqual(dl.length, 1);
    assert.strictEqual(dl[0].payload.url, INSTALLER);
    assert.strictEqual(of(calls, 'app.openUrl').length, 0);
  });

  const UPDATE_TITLE = 'עדכון לשירות הדיבור';
  const updateAsk = (calls) => of(calls, 'ui.showConfirm').map(c => c.payload).filter(p => p.title === UPDATE_TITLE);

  await test('Windows, service 2.5.0: the update carries the "cannot load" fix and the accuracy news, not the 2.5.0 news', async () => {
    const calls = await press('windows', {
      status: { service: 'otzaria-voice', version: '2.5.0', platform: 'windows', runtime: true, pack: true },
      download: { success: true, data: { path: SAVED } }
    });
    const ask = updateAsk(calls);
    assert.strictEqual(ask.length, 1, 'update offers: ' + ask.length);
    assert.ok(ask[0].content.includes('מדויקת יותר') && ask[0].content.includes('cannot load'), ask[0].content);
    assert.ok(!ask[0].content.includes('Ctrl+Alt+M'), 'a 2.5.0 service was told the 2.5.0 news: ' + ask[0].content);
  });

  await test('Windows, service 2.2.0: the update also carries the 2.5.0 news', async () => {
    const calls = await press('windows', {
      status: { service: 'otzaria-voice', version: '2.2.0', platform: 'windows', runtime: true, pack: true },
      download: { success: true, data: { path: SAVED } }
    });
    const ask = updateAsk(calls);
    assert.strictEqual(ask.length, 1, 'update offers: ' + ask.length);
    assert.ok(ask[0].content.includes('מדויקת יותר') && ask[0].content.includes('Ctrl+Alt+M'), ask[0].content);
  });

  const serviceAt = (version, platform) => ({ service: 'otzaria-voice', version, platform, runtime: true, pack: true });

  await test('Windows, service 2.5.1: the update is offered for the "cannot load" fix, without the accuracy news', async () => {
    const calls = await press('windows', { status: serviceAt('2.5.1', 'windows'), download: { success: true, data: { path: SAVED } } });
    const ask = updateAsk(calls);
    assert.strictEqual(ask.length, 1, 'update offers: ' + ask.length);
    assert.ok(ask[0].content.includes('cannot load') && ask[0].content.includes('Windows 11 עם מעבד ARM'), ask[0].content);
    assert.ok(ask[0].content.includes('צריך לעדכן כדי שההכתבה תעבוד'), 'a user with the error is told "not needed": ' + ask[0].content);
    assert.ok(!ask[0].content.includes('מדויקת יותר'), 'a 2.5.1 service was told the 2.5.1 news: ' + ask[0].content);
    const dl = of(calls, 'network.download');
    assert.strictEqual(dl.length === 1 && dl[0].payload.url, INSTALLER, JSON.stringify(dl.map(c => c.payload.url)));
  });

  await test('Windows, service 2.5.2: it has the fix, so no update is offered', async () => {
    const calls = await press('windows', { status: serviceAt('2.5.2', 'windows') });
    assert.strictEqual(updateAsk(calls).length, 0, 'an update was offered to 2.5.2');
  });

  await test('Linux, service 2.5.1: no update (the fix is for Windows); 2.5.0: the accuracy news, no "cannot load"', async () => {
    const at251 = await press('linux', { status: serviceAt('2.5.1', 'linux') });
    assert.strictEqual(updateAsk(at251).length, 0, 'an update was offered to a Linux 2.5.1');
    const at250 = await press('linux', { status: serviceAt('2.5.0', 'linux') });
    const ask = updateAsk(at250);
    assert.strictEqual(ask.length, 1, 'update offers: ' + ask.length);
    assert.ok(ask[0].content.includes('מדויקת יותר') && !ask[0].content.includes('cannot load'), ask[0].content);
    assert.strictEqual(of(at250, 'network.download').length, 0, 'downloaded on Linux');
    assert.strictEqual(of(at250, 'app.openUrl').length, 1, 'the page did not open');
  });

  await test('every platform: just below its threshold an update with news is offered, at it none', async () => {
    for (const [platform, below, at] of [['windows', '2.5.1', '2.5.2'], ['android', '2.5.0', '2.5.1'], ['macos', '2.5.0', '2.5.1'], ['linux', '2.5.0', '2.5.1'], ['', '2.5.0', '2.5.1']]) {
      const offered = updateAsk(await press(platform, { status: serviceAt(below, platform), download: { success: true, data: { path: SAVED } } }));
      assert.strictEqual(offered.length, 1, platform + ' ' + below + ': offers ' + offered.length);
      assert.ok(offered[0].content.includes('cannot load') || offered[0].content.includes('מדויקת יותר'), platform + ' ' + below + ': no news: ' + offered[0].content);
      assert.strictEqual(updateAsk(await press(platform, { status: serviceAt(at, platform) })).length, 0, platform + ' ' + at + ': offered');
    }
  });

  await test('Windows, service 2.5.1 while its voice pack downloads: no update now (it would cut the download short)', async () => {
    const st = { ...serviceAt('2.5.1', 'windows'), pack: false, packJob: { state: 'downloading', received: 1, total: 2 } };
    // watchPack would poll forever against a pack that never finishes: its timer is not run here.
    const timers = { setInterval: () => 1, clearInterval: () => {} };
    const calls = await press('windows', { status: st, timers });
    assert.strictEqual(updateAsk(calls).length, 0, 'an update was offered during the pack download');
    assert.ok(messages(calls).some(m => m.includes('עדיין יורדת')), JSON.stringify(messages(calls)));
  });

  await test('raising LATEST_SERVICE is a decision about the thresholds too', () => {
    const src = fs.readFileSync(path.join(root, 'src', 'feedback.js'), 'utf8');
    const read = (name) => new RegExp('const ' + name + " = '([^']+)'").exec(src)[1];
    const now = [read('LATEST_SERVICE'), read('SERVICE_UPDATE_BELOW_WINDOWS'), read('SERVICE_UPDATE_BELOW')];
    // When this fails: decide who should be offered the new service (SERVICE_UPDATE_BELOW*), add its
    // news to offerServiceUpdate for them, and only then update this line.
    assert.deepStrictEqual(now, ['2.5.5', '2.5.2', '2.5.1'],
      'LATEST_SERVICE or a threshold changed: decide the thresholds and the news, then update this test');
  });

  await test('the latest service: no update is offered', async () => {
    const latest = /const LATEST_SERVICE = '([^']+)'/.exec(fs.readFileSync(path.join(root, 'src', 'feedback.js'), 'utf8'))[1];
    const calls = await press('windows', {
      status: { service: 'otzaria-voice', version: latest, platform: 'windows', runtime: true, pack: true }
    });
    assert.strictEqual(updateAsk(calls).length, 0, 'an update was offered to ' + latest);
  });

  await test('LATEST_SERVICE in the plugin equals kVersion in the service', () => {
    const latest = /const LATEST_SERVICE = '([^']+)'/.exec(fs.readFileSync(path.join(root, 'src', 'feedback.js'), 'utf8'))[1];
    const kVersion = /const char\* kVersion = "([^"]+)"/.exec(
      fs.readFileSync(path.join(repo, 'service', 'src', 'common', 'service.cpp'), 'utf8'))[1];
    assert.strictEqual(latest, kVersion);
  });

  await test('the manifest lets Otzaria download the installer, every hop of it', () => {
    const m = JSON.parse(fs.readFileSync(path.join(root, 'manifest.base.json'), 'utf8'));
    assert.ok(m.permissions.includes('network.access'), 'network.access is missing');
    assert.ok(m.permissions.includes('network.localhost'), 'network.localhost is missing');
    assert.strictEqual(m.minAppVersion, '0.9.98.1', 'minAppVersion');
    assert.ok(m.permissions.includes('search.field_actions'), 'search.field_actions is missing');
    assert.strictEqual(m.contributes.startup.searchFieldActions[0].id, 'dictate');
    for (const hop of [...HOPS, ...HOPS_FULL]) {
      assert.ok(allowedBy(hop, m.network.allowlist), 'not in the manifest: ' + hop);
      assert.ok(allowedBy(hop, OFFICIAL), 'not in the official list: ' + hop);
    }
  });

  await test('the plugin downloads exactly the two installer addresses tested here', () => {
    const src = fs.readFileSync(path.join(root, 'src', 'feedback.js'), 'utf8');
    const urls = (src.match(/https:\/\/github\.com\/[^'"\s]+\.exe/g) || []).sort();
    assert.deepStrictEqual(urls, [INSTALLER_FULL, INSTALLER].sort());
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
