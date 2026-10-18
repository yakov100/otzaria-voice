// Checks the voice menu without Otzaria:
//     node plugins/read-aloud/test/voiceMenu.test.js
// main.js runs whole against a fake Otzaria that records every call and
// answers the service's /status with a given list of installed voices.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const dir = path.join(__dirname, '..');
const src = fs.readFileSync(path.join(dir, 'main.js'), 'utf8');
const manifest = JSON.parse(fs.readFileSync(path.join(dir, 'manifest.json'), 'utf8'));

/** Loads main.js with a fake host. [service] answers GET/POST by path, or throws when null; [confirm] answers ui.showConfirm. */
function load(service, confirm) {
  const handlers = {};
  const calls = [];
  const storage = {};
  async function* stream(lines) {
    yield { type: 'response', status: 200 };
    for (const l of lines) yield { type: 'data', body: JSON.stringify(l) + '\n' };
  }
  async function* refused() {
    yield { type: 'response', status: 0 };
    yield { type: 'data', body: 'connection refused' };
  }
  const Otzaria = {
    on(event, cb) { handlers[event] = cb; },
    call(method, payload) {
      calls.push({ method, payload });
      if (method === 'network.fetchStream') {
        if (!service) return refused();
        const p = payload.url.replace(/^http:\/\/127\.0\.0\.1:47811/, '');
        return stream(service(p, payload.method, payload.body ? JSON.parse(payload.body) : null));
      }
      if (method === 'ui.showConfirm') return Promise.resolve({ success: true, data: { confirmed: !!confirm } });
      if (method === 'storage.get') return Promise.resolve({ success: true, data: storage[payload.key] });
      if (method === 'storage.set') { storage[payload.key] = payload.value; return Promise.resolve({ success: true }); }
      if (method === 'storage.remove') { delete storage[payload.key]; return Promise.resolve({ success: true }); }
      return Promise.resolve({ success: true, data: {} });
    }
  };
  const ctx = { Otzaria, window: {}, console, setTimeout, clearTimeout };
  vm.createContext(ctx);
  vm.runInContext(src + '\nthis.menuChildren = menuChildren; this.menuVoices = menuVoices; this.voiceOfItem = voiceOfItem;', ctx);
  return { ctx, handlers, calls, storage };
}

const settle = () => new Promise(r => setTimeout(r, 30));
// Values made inside the vm context have other prototypes; compare them as JSON.
const same = (actual, expected) => assert.deepStrictEqual(JSON.parse(JSON.stringify(actual)), expected);
const lastMenu = calls => calls.filter(c => c.method === 'reader.updateToolbarItem' && c.payload.patch.children).pop();

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
  await test('the manifest declares the menu as built for the light and the pack voice alone', () => {
    const { ctx } = load(null);
    const declared = manifest.contributes.startup.toolbarItems[0].children;
    same(ctx.menuChildren(ctx.menuVoices(undefined, undefined)), declared);
  });

  await test('light voices are labelled as fast and have their own remove item', () => {
    const { ctx } = load(null);
    const children = ctx.menuChildren(['chaim-light', 'chaim']);
    assert.strictEqual(children.find(c => c.id === 'voice-chaim-light').title, 'קול: חיים קל (מהיר)');
    assert.strictEqual(children[children.length - 1].id, 'remove-light-model');
    assert.strictEqual(ctx.voiceOfItem('voice-chaim-light'), 'chaim-light');
    same(ctx.menuVoices(['chaim'], ['chaim-light', 'moshe-light']), ['chaim-light', 'chaim', 'moshe-light']);
  });

  await test('menuVoices: light voice, pack voice, installed ids after, bad ids dropped', () => {
    const { ctx } = load(null);
    same(ctx.menuVoices(['test24', 'chaim', 'moshe']), ['chaim-light', 'chaim', 'test24', 'moshe']);
    same(ctx.menuVoices(undefined), ['chaim-light', 'chaim']);
    same(ctx.menuVoices(['', 'a\u0001b', 7, 'system', 'x'.repeat(61), 'ok']), ['chaim-light', 'chaim', 'ok']);
  });

  await test('menuChildren never exceeds 20 items and keeps the fixed items around the voices', () => {
    const { ctx } = load(null);
    const many = Array.from({ length: 30 }, (_, i) => 'v' + i);
    const children = ctx.menuChildren(many);
    assert.strictEqual(children.length, 20);
    assert.strictEqual(children[children.length - 2].id, 'remove-voice-model');
    assert.strictEqual(children[children.length - 1].id, 'remove-light-model');
    assert.strictEqual(children.find(c => c.id === 'voice-system').title, 'קול: אסף (של Windows)');
    assert.ok(children.some(c => c.id === 'voice-v0'));
    assert.strictEqual(new Set(children.map(c => c.id)).size, children.length);
  });

  await test('voiceOfItem maps menu ids to voices', () => {
    const { ctx } = load(null);
    assert.strictEqual(ctx.voiceOfItem('voice-system'), '');
    assert.strictEqual(ctx.voiceOfItem('voice-chaim'), 'chaim');
    assert.strictEqual(ctx.voiceOfItem('voice-test24'), 'test24');
    assert.strictEqual(ctx.voiceOfItem('voice-'), null);
    assert.strictEqual(ctx.voiceOfItem('rate-slow'), null);
  });

  await test('on boot the menu offers every voice the service has installed', async () => {
    const { handlers, calls } = load(() => [{ voices: ['chaim', 'test24'], voiceModel: true }]);
    handlers['plugin.boot']({ theme: {} });
    await settle();
    const menu = lastMenu(calls);
    assert.ok(menu, 'reader.updateToolbarItem was not called');
    assert.strictEqual(menu.payload.id, 'tts-main');
    const item = menu.payload.patch.children.find(c => c.id === 'voice-test24');
    assert.ok(item, 'voice-test24 is missing');
    assert.strictEqual(item.title, 'קול לשמירה: test24 (מוקלט)');
    assert.strictEqual(menu.payload.patch.children.find(c => c.id === 'voice-chaim').title, 'קול לשמירה: חיים (מוקלט)');
  });

  await test('without the service the menu is left as declared', async () => {
    const { handlers, calls } = load(null);
    handlers['plugin.boot']({ theme: {} });
    await settle();
    assert.strictEqual(lastMenu(calls), undefined);
  });

  await test('choosing an installed voice stores it and the MP3 is made with it', async () => {
    const requests = [];
    const { handlers, calls, storage } = load((p, method, body) => {
      requests.push({ p, method, body });
      if (p === '/status') return [{ voices: ['chaim', 'test24'], voiceModel: true }];
      if (p === '/speak') return [{ ok: true }];
      if (p === '/speak-wait') return [{ state: 'done', savedPath: 'C:\\out\\x.mp3', seconds: 60 }];
      return [{}];
    });
    handlers['plugin.boot']({ theme: {} });
    await settle();
    await handlers['reader.toolbar_item_clicked']({ itemId: 'voice-test24' });
    assert.strictEqual(storage.mp3Voice, 'test24');
    const said = calls.filter(c => c.method === 'ui.showMessage').map(c => c.payload.message);
    assert.ok(said.includes('הקול לשמירה כ-MP3: test24 (קול מוקלט). ההקראה בקול נשארת בקול של Windows.'), 'message: ' + JSON.stringify(said));
    storage.mp3Folder = 'C:\\out';
    handlers['reader.context_menu_item_clicked']({ itemId: 'tts-save-selection', selectedText: 'בראשית ברא', currentBook: 'בראשית' });
    await settle();
    const speak = requests.find(r => r.p === '/speak');
    assert.ok(speak, 'no /speak request');
    assert.strictEqual(speak.body.voice, 'test24');
    assert.strictEqual(speak.body.text, 'בראשית ברא');
  });

  await test('choosing the light voice offers its download with the size the service reports', async () => {
    const requests = [];
    const { handlers, calls, storage } = load((p, method) => {
      requests.push({ p, method });
      if (p === '/status') return [{ voices: ['chaim'], lightVoices: [], voiceModel: true, lightModel: false, lightModelSize: 68 * 1048576, lightPublished: true, nakdan: true }];
      if (p === '/install-light-model') return [{ started: true }];
      if (p === '/light-model-wait') return [{ state: 'done', received: 1, total: 1 }];
      return [{}];
    }, true);
    handlers['plugin.boot']({ theme: {} });
    await settle();
    await handlers['reader.toolbar_item_clicked']({ itemId: 'voice-chaim-light' });
    await settle();
    assert.strictEqual(storage.mp3Voice, 'chaim-light');
    const confirm = calls.find(c => c.method === 'ui.showConfirm');
    assert.ok(confirm, 'no download confirmation');
    assert.ok(confirm.payload.content.includes('68 מגה-בייט'), confirm.payload.content);
    assert.ok(requests.some(r => r.p === '/install-light-model'), 'no install request');
  });

  await test('an unpublished light voice is explained, not downloaded', async () => {
    const requests = [];
    const { handlers, calls } = load((p) => {
      requests.push(p);
      if (p === '/status') return [{ voices: ['chaim'], lightVoices: [], voiceModel: true, lightModel: false, lightModelSize: 0, lightPublished: false, nakdan: true }];
      return [{}];
    });
    handlers['plugin.boot']({ theme: {} });
    await settle();
    await handlers['reader.toolbar_item_clicked']({ itemId: 'voice-chaim-light' });
    await settle();
    assert.ok(!requests.includes('/install-light-model'));
    const said = calls.filter(c => c.method === 'ui.showMessage').map(c => c.payload.message);
    assert.ok(said.some(m => m.includes('עדיין לא פורסם')), JSON.stringify(said));
  });

  await test('the menu is refreshed when a voice is chosen and a new file appeared', async () => {
    let voices = ['chaim'];
    const { handlers, calls } = load(() => [{ voices, voiceModel: true }]);
    handlers['plugin.boot']({ theme: {} });
    await settle();
    assert.ok(!lastMenu(calls).payload.patch.children.some(c => c.id === 'voice-new'));
    voices = ['chaim', 'new'];
    await handlers['reader.toolbar_item_clicked']({ itemId: 'voice-system' });
    await settle();
    assert.ok(lastMenu(calls).payload.patch.children.some(c => c.id === 'voice-new'));
  });

  if (failed) {
    console.log(failed + ' failed');
    process.exit(1);
  }
  console.log('voiceMenu: all passed');
})();
