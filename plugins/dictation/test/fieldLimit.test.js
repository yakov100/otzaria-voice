// Checks what the microphone inside a search field does when the dictated text
// grows past what the field accepts, without Otzaria:
//     node plugins/dictation/test/fieldLimit.test.js
// Otzaria refuses search.setFieldText with more than 1,000 characters
// (PluginSearchFieldSessionService.maxTextLength; docs/dictation/search-field-actions-contract.md).
// Before this test the plugin ignored that refusal: the field stopped changing
// and nobody was told.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const root = path.join(__dirname, '..');
const FILES = ['service.js', 'feedback.js', 'dictation.js', 'toolbar.js', 'field_actions.js', 'main.js'];  // build.py, FILES
const bundle = '(function () {\n\'use strict\';\n' +
  FILES.map(f => fs.readFileSync(path.join(root, 'src', f), 'utf8')).join('\n') + '\n})();\n';

const FIELD_MAX = 1000;
const STATUS = {
  service: 'otzaria-voice', version: '9.9.9', platform: 'windows',
  runtime: true, pack: true, microphone: true, packOutdated: false,
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** [updates]: the texts of the segments the fake service streams, one update each. */
function load(updates) {
  const handlers = {};
  const calls = [];
  const state = { dictateClosed: false };
  async function* answer(obj) {
    yield { type: 'response', status: 200 };
    yield { type: 'data', body: JSON.stringify(obj) + '\n' };
  }
  async function* down() {
    throw new Error('connection refused');
  }
  async function* dictate() {
    try {
      yield { type: 'response', status: 200 };
      yield { type: 'data', body: '{"type":"listening"}\n{"type":"ready"}\n' };
      let segment = 0;
      for (const text of updates) {
        await sleep(5);
        segment++;
        yield { type: 'data', body: JSON.stringify({ type: 'update', segment, text, final: true }) + '\n' };
      }
      await sleep(5);
      yield { type: 'data', body: '{"type":"end","reason":"silence"}\n' };
    } finally {
      state.dictateClosed = true;
    }
  }
  const Otzaria = {
    on(event, cb) { handlers[event] = cb; },
    call(method, payload) {
      calls.push({ method, payload });
      if (method === 'network.fetchStream') {
        const p = payload.url.replace(/^http:\/\/127\.0\.0\.1:47811/, '');
        if (p === '/status') return answer(STATUS);
        if (p === '/dictate') return dictate();
        if (p === '/cancel' || p === '/stop') return answer({ ok: true });
        return down();
      }
      if (method === 'search.setFieldText') {
        if (typeof payload.text !== 'string' || payload.text.length > FIELD_MAX) {
          return Promise.resolve({ success: false, error: { code: 'error.invalid_params', message: 'text is longer than 1000 characters' } });
        }
        return Promise.resolve({ success: true, data: true });
      }
      if (method === 'storage.get') return Promise.resolve({ success: true, data: null });
      return Promise.resolve({ success: true, data: {} });
    },
  };
  const ctx = { Otzaria, console: { log() {}, warn() {}, error() {} }, setTimeout, clearTimeout, setInterval, clearInterval };
  vm.createContext(ctx);
  vm.runInContext(bundle, ctx);
  return { handlers, calls, state };
}

const of = (calls, method) => calls.filter((c) => c.method === method);
const fetched = (calls, p) => of(calls, 'network.fetchStream').filter((c) => c.payload.url.endsWith(p));
const said = (calls) => calls.filter((c) => /^ui\.show(Message|Error|Success)$/.test(c.method)).map((c) => c.payload.message);

/** Presses the microphone in a search field and lets the fake dictation run out. */
async function dictateIntoField(updates) {
  const h = load(updates);
  h.handlers['plugin.boot']({ app: { platform: 'windows' } });
  await sleep(20);
  await h.handlers['search.fieldAction.invoked']({ actionId: 'dictate', sessionId: 's1', field: 'library', text: '', selectionStart: 0 });
  await sleep(50);
  return h;
}

const words = (n) => Array.from({ length: n }, (_, i) => 'מילה' + (i % 10)).join(' ');  // 5n + (n - 1) characters

let failed = 0;
async function test(name, fn) {
  try {
    await fn();
    console.log('ok  ', name);
  } catch (e) {
    failed++;
    console.log('FAIL', name, '\n  ', e.message);
  }
}

(async () => {
  await test('a dictation within the limit is written in full and nothing extra is said', async () => {
    const h = await dictateIntoField([words(20), words(20)]);
    const writes = of(h.calls, 'search.setFieldText').map((c) => c.payload.text);
    assert.ok(writes.length >= 1, 'nothing was written');
    assert.strictEqual(writes[writes.length - 1], words(20) + ' ' + words(20));
    assert.strictEqual(fetched(h.calls, '/cancel').length, 0, 'a short dictation was cancelled');
    assert.ok(!said(h.calls).some((m) => m.includes('1,000')), 'a short dictation was told about the limit');
    assert.strictEqual(of(h.calls, 'search.endFieldSession').length, 1, 'the field session was not ended once');
  });

  await test('past 1,000 characters: nothing too long is sent, the dictation stops, the user is told', async () => {
    const first = words(100);   // 599 characters
    const second = words(100);  // together 1,199
    const h = await dictateIntoField([first, second, words(5)]);
    const writes = of(h.calls, 'search.setFieldText').map((c) => c.payload.text);
    const tooLong = writes.filter((t) => t.length > FIELD_MAX);
    assert.strictEqual(tooLong.length, 0, 'texts over the limit were sent: ' + tooLong.map((t) => t.length).join(', '));
    assert.strictEqual(writes[writes.length - 1], first, 'the field does not keep the last text that fit');
    assert.ok(fetched(h.calls, '/cancel').length + fetched(h.calls, '/stop').length >= 1, 'the dictation was not ended');
    assert.ok(h.state.dictateClosed, 'the dictation stream is still open');
    const told = said(h.calls).filter((m) => m.includes('1,000'));
    assert.strictEqual(told.length, 1, 'messages about the limit: ' + JSON.stringify(said(h.calls)));
    assert.strictEqual(of(h.calls, 'search.endFieldSession').length, 1, 'the field session was not ended once');
  });

  await test('the boundary: 1,000 characters are written, 1,001 stop the dictation', async () => {
    const exact = await dictateIntoField(['א'.repeat(FIELD_MAX)]);
    const exactWrites = of(exact.calls, 'search.setFieldText').map((c) => c.payload.text.length);
    assert.deepStrictEqual(exactWrites.slice(-1), [FIELD_MAX], 'writes: ' + exactWrites.join(', '));
    assert.strictEqual(fetched(exact.calls, '/cancel').length, 0, '1,000 characters cancelled the dictation');

    const over = await dictateIntoField(['א'.repeat(FIELD_MAX - 1), 'ב']);  // 999 + space + 1 = 1,001
    const overWrites = of(over.calls, 'search.setFieldText').map((c) => c.payload.text.length);
    assert.ok(overWrites.every((n) => n <= FIELD_MAX), 'writes: ' + overWrites.join(', '));
    assert.strictEqual(overWrites[overWrites.length - 1], FIELD_MAX - 1, 'writes: ' + overWrites.join(', '));
    assert.strictEqual(fetched(over.calls, '/cancel').length, 1, '1,001 characters did not cancel the dictation');
  });

  if (failed) {
    console.log(failed + ' failed');
    process.exit(1);
  }
  console.log('all passed');
})();
