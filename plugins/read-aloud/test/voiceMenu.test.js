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
        const lines = service(p, payload.method, payload.body ? JSON.parse(payload.body) : null);
        return lines === null ? refused() : stream(lines);  // a service function may return null: the service is down
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
  vm.runInContext(src + '\nthis.menuGroups = menuGroups; this.toolbarItems = toolbarItems; this.menuVoices = menuVoices; this.voiceOfItem = voiceOfItem;', ctx);
  return { ctx, handlers, calls, storage };
}

const settle = () => new Promise(r => setTimeout(r, 30));
// Values made inside the vm context have other prototypes; compare them as JSON.
const same = (actual, expected) => assert.deepStrictEqual(JSON.parse(JSON.stringify(actual)), expected);
const lastMenu = (calls, id = 'tts-voice') => calls.filter(c => c.method === 'reader.updateToolbarItem' && c.payload.id === id && c.payload.patch.children).pop();

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
  await test('the manifest declares the two toolbar menus as built for the pack voice alone', () => {
    const { ctx } = load(null);
    same(ctx.toolbarItems(ctx.menuVoices(undefined, undefined), true), manifest.contributes.startup.toolbarItems);
    assert.ok(!JSON.stringify(manifest.contributes.startup.toolbarItems).includes('●'), 'the manifest must not mark any choice');
    same(manifest.contributes.startup.toolbarItems.map(i => i.id), ['tts-main', 'tts-voice']);
  });

  await test('light voices are labelled as fast and have their own remove item', () => {
    const { ctx } = load(null);
    const g = ctx.menuGroups(['chaim-light', 'chaim']);
    assert.strictEqual(g.voice.find(c => c.id === 'voice-chaim-light').title, '○ חיים קל (מהיר)');
    assert.strictEqual(g.voice[g.voice.length - 1].id, 'remove-light-model');
    assert.strictEqual(ctx.voiceOfItem('voice-chaim-light'), 'chaim-light');
    same(ctx.menuVoices(['chaim'], ['chaim-light', 'moshe-light']), ['chaim-light', 'chaim', 'moshe-light']);
  });

  await test('menuVoices: pack voice, installed ids after, bad ids dropped', () => {
    const { ctx } = load(null);
    same(ctx.menuVoices(['test24', 'chaim', 'moshe']), ['chaim', 'test24', 'moshe']);
    same(ctx.menuVoices(undefined), ['chaim']);
    same(ctx.menuVoices(['', 'a\u0001b', 7, 'system', 'x'.repeat(61), 'ok']), ['chaim', 'ok']);
  });

  await test('menuVoices: the light voice comes first, only when published or installed', () => {
    const { ctx } = load(null);
    same(ctx.menuVoices(['chaim'], [], true), ['chaim-light', 'chaim']);
    same(ctx.menuVoices(['chaim'], ['chaim-light'], false), ['chaim-light', 'chaim']);
    same(ctx.menuVoices(['chaim'], [], false), ['chaim']);
    same(ctx.menuVoices(['chaim'], undefined, undefined), ['chaim']);
    same(ctx.menuVoices(['chaim'], [], 'yes'), ['chaim']);
  });

  await test('the menu offers the light voice and its remove item only where the service says so', async () => {
    const ids = async (status) => {
      const { handlers, calls } = load(status === null ? null : () => [status]);
      handlers['plugin.boot']({ theme: {} });
      await settle();
      const menu = lastMenu(calls);
      return menu ? menu.payload.patch.children.map(c => c.id) : [];
    };
    const has = (list, id) => list.includes(id);
    const none = await ids(null);  // no service
    assert.ok(!has(none, 'voice-chaim-light') && !has(none, 'remove-light-model'), 'no service: ' + none);
    const old = await ids({ voices: ['chaim'], voiceModel: true });  // a service 2.2.0: no light fields
    assert.ok(!has(old, 'voice-chaim-light') && !has(old, 'remove-light-model'), 'old service: ' + old);
    const unpublished = await ids({ voices: ['chaim'], lightVoices: [], lightModel: false, lightPublished: false, voiceModel: true });
    assert.ok(!has(unpublished, 'voice-chaim-light') && !has(unpublished, 'remove-light-model'), 'unpublished: ' + unpublished);
    const published = await ids({ voices: ['chaim'], lightVoices: [], lightModel: false, lightPublished: true, voiceModel: true });
    assert.ok(has(published, 'voice-chaim-light') && has(published, 'remove-light-model'), 'published: ' + published);
    const installed = await ids({ voices: ['chaim'], lightVoices: ['chaim-light'], lightModel: true, lightPublished: false, voiceModel: true });
    assert.ok(has(installed, 'voice-chaim-light') && has(installed, 'remove-light-model'), 'installed: ' + installed);
    assert.strictEqual(installed.indexOf('voice-chaim-light'), 1, 'the light voice follows the system voice: ' + installed);
  });

  await test('no menu exceeds 20 items; the voice menu keeps the system voice first', () => {
    const { ctx } = load(null);
    const many = Array.from({ length: 30 }, (_, i) => 'v' + i);
    const g = ctx.menuGroups(many);
    Object.values(g).forEach(children => {
      assert.ok(children.length <= 20);
      assert.strictEqual(new Set(children.map(c => c.id)).size, children.length);
    });
    assert.strictEqual(g.voice.length, 20);
    assert.strictEqual(g.voice[0].id, 'voice-system');
    assert.strictEqual(g.voice[0].title, '● אסף (של Windows)');
    assert.ok(g.voice.some(c => c.id === 'voice-v0'));
    assert.strictEqual(g.voice[g.voice.length - 1].id, 'remove-voice-model');
    const withLight = ctx.menuGroups(['chaim-light'].concat(many));
    assert.strictEqual(withLight.voice.length, 20);
    assert.strictEqual(withLight.voice[withLight.voice.length - 2].id, 'remove-voice-model');
    assert.strictEqual(withLight.voice[withLight.voice.length - 1].id, 'remove-light-model');
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
    assert.strictEqual(menu.payload.id, 'tts-voice');
    const item = menu.payload.patch.children.find(c => c.id === 'voice-test24');
    assert.ok(item, 'voice-test24 is missing');
    assert.strictEqual(item.title, '○ test24 (מוקלט, לשמירה בלבד)');
    assert.strictEqual(menu.payload.patch.children.find(c => c.id === 'voice-chaim').title, '○ חיים (מוקלט, לשמירה בלבד)');
  });

  await test('at start the menus get their marks even with the defaults and no service', async () => {
    const { handlers, calls } = load(null);
    handlers['plugin.boot']({ theme: {} });
    await settle();
    const voice = lastMenu(calls);
    assert.ok(voice, 'the voice menu was not updated');
    same(voice.payload.patch.children.filter(c => c.title.startsWith('● ')).map(c => c.id), ['voice-system']);
    same(lastMenu(calls, 'tts-main').payload.patch.children.filter(c => c.title.startsWith('● ')).map(c => c.id), ['rate-normal']);
  });

  await test('voices known so far stay in the menu when the service stops answering', async () => {
    let up = true;
    const { handlers, calls } = load(() => (up ? [{ voices: ['chaim', 'test24'], lightVoices: ['chaim-light'], voiceModel: true, lightModel: true }] : null));
    handlers['plugin.boot']({ theme: {} });
    await settle();
    assert.ok(lastMenu(calls).payload.patch.children.some(c => c.id === 'voice-test24'));
    up = false;
    await handlers['reader.toolbar_item_clicked']({ itemId: 'rate-fast' });
    await settle();
    const voice = lastMenu(calls).payload.patch.children;
    assert.ok(voice.some(c => c.id === 'voice-test24'), 'a known voice disappeared');
    same(lastMenu(calls, 'tts-main').payload.patch.children.filter(c => c.title.startsWith('● ')).map(c => c.id), ['rate-fast']);
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
    const first = lastMenu(calls);  // nothing changed from the declared menu: no rebuild needed
    assert.ok(!first || !first.payload.patch.children.some(c => c.id === 'voice-new'));
    voices = ['chaim', 'new'];
    await handlers['reader.toolbar_item_clicked']({ itemId: 'voice-system' });
    await settle();
    assert.ok(lastMenu(calls).payload.patch.children.some(c => c.id === 'voice-new'));
  });

  await test('the chosen speed and voice are marked, one per list, and follow every choice', async () => {
    const marked = (calls, id) => lastMenu(calls, id).payload.patch.children.filter(c => c.title.startsWith('● '));
    const title = (calls, id) => calls.filter(c => c.method === 'reader.updateToolbarItem' && c.payload.id === id && c.payload.patch.title).pop().payload.patch.title;
    const { handlers, calls, storage } = load(() => [{ voices: ['chaim'], lightVoices: ['chaim-light'], voiceModel: true, lightModel: true }]);
    storage.rate = 1.3;
    storage.mp3Voice = 'chaim-light';
    handlers['plugin.boot']({ theme: {} });
    await settle();
    same(marked(calls, 'tts-main').map(c => c.id), ['rate-fast']);
    same(marked(calls, 'tts-voice').map(c => c.id), ['voice-chaim-light']);
    assert.strictEqual(title(calls, 'tts-voice'), 'קול: חיים קל');
    const unmarked = lastMenu(calls, 'tts-main').payload.patch.children.concat(lastMenu(calls, 'tts-voice').payload.patch.children).filter(c => c.title.startsWith('○ '));
    assert.strictEqual(unmarked.length, 3 + 2);
    await handlers['reader.toolbar_item_clicked']({ itemId: 'rate-slow' });
    await settle();
    same(marked(calls, 'tts-main').map(c => c.id), ['rate-slow']);
    await handlers['reader.toolbar_item_clicked']({ itemId: 'voice-system' });
    await settle();
    same(marked(calls, 'tts-voice').map(c => c.id), ['voice-system']);
    assert.strictEqual(title(calls, 'tts-voice'), 'קול: אסף');
  });

  await test('the choice is marked even when the service is not running', async () => {
    const { handlers, calls, storage } = load(null);
    storage.mp3Voice = 'chaim-light';
    handlers['plugin.boot']({ theme: {} });
    await settle();
    const menu = lastMenu(calls);
    assert.ok(menu, 'the menu was not updated');
    same(menu.payload.patch.children.filter(c => c.title.startsWith('● ')).map(c => c.id), ['voice-chaim-light']);
  });

  await test('every menu item reacts to a click (a host call is made, nothing throws)', async () => {
    const { ctx, handlers, calls } = load(() => [{ voices: ['chaim', 'x'], lightVoices: ['chaim-light'], voiceModel: true, lightModel: true }]);
    const g = ctx.menuGroups(ctx.menuVoices(['chaim', 'x'], ['chaim-light']));
    const ids = [].concat(g.main, g.voice).map(c => c.id);
    for (const id of ids) {
      const before = calls.length;
      await handlers['reader.toolbar_item_clicked']({ itemId: id });
      await settle();
      assert.ok(calls.length > before, 'a click on ' + id + ' did nothing');
    }
  });

  await test('the voice in use is shown and marked even when the service does not list it', async () => {
    const { handlers, calls, storage } = load(() => [{ voices: [], lightVoices: [], voiceModel: false, lightModel: false }]);
    storage.mp3Voice = 'moshe-light';
    handlers['plugin.boot']({ theme: {} });
    await settle();
    const children = lastMenu(calls).payload.patch.children;
    const item = children.find(c => c.id === 'voice-moshe-light');
    assert.ok(item && item.title.startsWith('● '), 'the chosen voice is not marked');
    assert.strictEqual(children.length <= 20, true);
  });

  await test('a choice is marked at once, without waiting for the service', async () => {
    let answer;
    const { handlers, calls } = load((p) => (p === '/status' ? answer() : [{}]));
    answer = () => [{ voices: ['chaim'], lightVoices: ['chaim-light'], voiceModel: true, lightModel: true }];
    handlers['plugin.boot']({ theme: {} });
    await settle();
    const patches = () => calls.filter(c => c.method === 'reader.updateToolbarItem' && c.payload.patch.children).length;
    const before = patches();
    handlers['reader.toolbar_item_clicked']({ itemId: 'rate-fast' });  // not awaited: the marking must come first
    await new Promise(r => setTimeout(r, 0));
    assert.ok(patches() > before, 'the menu was not updated at once');
  });

  if (failed) {
    console.log(failed + ' failed');
    process.exit(1);
  }
  console.log('voiceMenu: all passed');
})();
