// Checks reading aloud in the light voice (readLive) without Otzaria:
//     node plugins/read-aloud/test/liveRead.test.js
// main.js runs whole against a fake Otzaria with a small book and a fake
// voice service that "plays" each sentence of what it was handed in turn,
// reporting it through /speak-live-wait as the real one does.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'main.js'), 'utf8');
const sleep = ms => new Promise(r => setTimeout(r, ms));

const BOOK = [
  'בראשית ברא אלהים את השמים ואת הארץ. והארץ היתה תהו ובהו.',
  '',
  'ויאמר אלהים יהי אור. ויהי אור.'
];

function wordsOf(text) {
  const out = [];
  const re = /\S+/g;
  let m;
  while ((m = re.exec(text))) {
    const range = { start: { utf16: m.index }, end: { utf16: m.index + m[0].length } };
    out.push({ text: m[0], start: { utf16: m.index }, renderedRange: range, sourceRange: range });
  }
  return out;
}

/** Sentences of [text] with their UTF-16 offsets, as narrator::pieces and pieceSpans give them. */
function piecesOf(text) {
  const out = [];
  const re = /[^.]+\.?/g;
  let m;
  while ((m = re.exec(text))) {
    const lead = m[0].length - m[0].trimStart().length;
    const body = m[0].trim();
    if (body) out.push({ from: m.index + lead, to: m.index + lead + body.length });
  }
  return out;
}

/** A fake voice service; [mode]: 'ok', 'down' (not running), 'old' (no /speak-live), 'nomodel'. */
function fakeService(mode) {
  const svc = { mode, requests: [], segs: [], si: 0, pi: 0, paused: false, stopped: false, pauseAfter: null };
  const status = state => {
    const seg = svc.segs[Math.min(svc.si, svc.segs.length - 1)];
    const pi = seg && svc.si < svc.segs.length ? svc.pi : seg ? seg.pieces.length - 1 : -1;
    const p = seg && seg.pieces[pi];
    return {
      state, error: '', voice: 'chaim-light', segment: seg ? seg.id : -1, piece: p ? pi : -1,
      pieces: seg ? seg.pieces.length : 0, from: p ? p.from : 0, to: p ? p.to : 0,
      lastSegment: svc.segs.length ? svc.segs[svc.segs.length - 1].id : -1,
      queued: seg ? svc.segs.length - 1 - Math.min(svc.si, svc.segs.length - 1) : 0
    };
  };
  svc.handle = async function* (p, method, body) {
    svc.requests.push({ p, method, body });
    if (mode === 'down') { yield { type: 'response', status: 0 }; yield { type: 'data', body: 'refused' }; return; }
    const err = (code, status) => [{ type: 'response', status }, { type: 'data', body: JSON.stringify({ error: code, message: 'x' }) }];
    if (p.startsWith('/speak-live') && mode === 'old') { yield* err('not_found', 404); return; }
    if (p === '/speak-live' && mode === 'nomodel') { yield* err('light_model_missing', 400); return; }
    yield { type: 'response', status: 200 };
    const line = o => ({ type: 'data', body: JSON.stringify(o) + '\n' });
    if (p === '/status') {
      const light = mode !== 'nomodel';
      yield line({ voices: ['chaim'], lightVoices: light ? ['chaim-light'] : [], lightModel: light, lightModelSize: 68 << 20,
        lightPublished: true, nakdan: true, voiceModel: true, speakLive: true });
      return;
    }
    if (p === '/speak-live') {
      svc.segs = [{ id: body.id, text: body.text, pieces: piecesOf(body.text) }];
      svc.si = 0; svc.pi = 0; svc.paused = false; svc.stopped = false;
      yield line(status('loading'));
      return;
    }
    if (p === '/speak-live-append') { svc.segs.push({ id: body.id, text: body.text, pieces: piecesOf(body.text) }); yield line(status('playing')); return; }
    if (p === '/speak-live-pause') { svc.paused = true; yield line(status('paused')); return; }
    if (p === '/speak-live-resume') { svc.paused = false; yield line(status('playing')); return; }
    if (p === '/speak-live-stop') { svc.stopped = true; svc.segs = []; yield line(status('idle')); return; }
    if (p === '/speak-live-wait') {
      while (true) {
        if (svc.stopped) { yield line(status('idle')); return; }
        if (svc.paused) { yield line(status('paused')); return; }
        if (svc.si >= svc.segs.length) { yield line(status('done')); return; }
        yield line(status('playing'));
        await sleep(8);  // the sentence is heard
        if (svc.paused || svc.stopped) continue;
        if (++svc.pi >= svc.segs[svc.si].pieces.length) {
          if (svc.si + 1 >= svc.segs.length) {  // said everything handed over
            await sleep(15);  // an append may still be on its way
            if (svc.si + 1 >= svc.segs.length) { svc.si++; continue; }
          }
          svc.si++; svc.pi = 0;
        }
      }
    }
    yield line({});
  };
  return svc;
}

function load(svc, storage) {
  const handlers = {};
  const calls = [];
  const spoken = [];
  class SpeechSynthesisUtterance { constructor(text) { this.text = text; } }
  const speechSynthesis = {
    getVoices: () => [{ lang: 'he-IL', name: 'Asaf' }],
    speak(u) { spoken.push(u.text); setTimeout(() => u.onend && u.onend(), 1); },
    cancel() {},
    addEventListener() {}
  };
  const Otzaria = {
    on(event, cb) { handlers[event] = cb; },
    call(method, payload) {
      calls.push({ method, payload });
      if (method === 'network.fetchStream') {
        const p = payload.url.replace(/^http:\/\/127\.0\.0\.1:47811/, '');
        return svc.handle(p, payload.method, payload.body ? JSON.parse(payload.body) : null);
      }
      if (method === 'reader.getSectionTextMap') {
        const text = BOOK[payload.sectionIndex];
        if (text === undefined) return Promise.resolve({ success: false, error: { message: 'out of range' } });
        return Promise.resolve({ success: true, data: { renderedText: text, sourceText: text, words: wordsOf(text), hasMore: false } });
      }
      if (method === 'reader.getCurrentRef') return Promise.resolve({ success: true, data: { currentBookId: 'b' } });
      if (method === 'ui.showConfirm') return Promise.resolve({ success: true, data: { confirmed: false } });
      if (method === 'storage.get') return Promise.resolve({ success: true, data: storage[payload.key] });
      if (method === 'storage.set') { storage[payload.key] = payload.value; return Promise.resolve({ success: true }); }
      if (method === 'storage.remove') { delete storage[payload.key]; return Promise.resolve({ success: true }); }
      return Promise.resolve({ success: true, data: {} });
    }
  };
  const window = { speechSynthesis };
  const ctx = { Otzaria, window, speechSynthesis, SpeechSynthesisUtterance, console, setTimeout, clearTimeout };
  vm.createContext(ctx);
  vm.runInContext(src + '\nthis.sentenceRange = sentenceRange; this.toSpeech = toSpeech; this.state = state;', ctx);
  handlers['plugin.boot']({ theme: {} });
  return { ctx, handlers, calls, spoken };
}

async function until(cond, ms = 3000) {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) throw new Error('timed out');
    await sleep(5);
  }
}

const play = handlers => handlers['reader.toolbar_item_clicked']({ itemId: 'play', currentBookId: 'b', currentIndex: 0, currentType: 'text' });
const highlights = calls => calls.filter(c => c.method === 'reader.setHighlight').map(c => c.payload);
const messages = calls => calls.filter(c => c.method === 'ui.showMessage').map(c => c.payload.message);
const paths = svc => svc.requests.map(r => r.p);

let failed = 0;
async function test(name, fn) {
  try {
    await fn();
  } catch (e) {
    failed++;
    console.log('FAIL', name, '\n  ', e.stack);
  }
}

(async () => {
  await test('sentenceRange covers the words of a spoken range, through the spoken -> original map', () => {
    const { ctx } = load(fakeService('ok'), {});
    const text = 'אמר ר\' יוחנן. ויהי אור.';
    const speech = ctx.toSpeech(text);  // ר' is said רבי: the spoken text is longer
    const seg = { words: wordsOf(text), map: speech.map, base: 0 };
    const end = speech.text.indexOf('.') + 1;
    const r = ctx.sentenceRange(seg, 0, end);
    assert.strictEqual(r.first, 0);
    assert.strictEqual(r.range.start.utf16, 0);
    assert.strictEqual(r.range.end.utf16, text.indexOf('.') + 1);
    const second = ctx.sentenceRange({ words: seg.words, map: speech.map, base: end }, 1, speech.text.length - end);
    assert.strictEqual(second.range.start.utf16, text.indexOf('ויהי'));
    assert.strictEqual(ctx.sentenceRange({ words: [], map: null, base: 0 }, 0, 4), null);
  });

  await test('the light voice reads the whole book through the service, a section ahead, highlighting each sentence', async () => {
    const svc = fakeService('ok');
    const { handlers, calls, spoken, ctx } = load(svc, { mp3Voice: 'chaim-light' });
    await play(handlers);
    await until(() => !ctx.state.playing && paths(svc).includes('/speak-live-stop'));
    const start = svc.requests.find(r => r.p === '/speak-live');
    assert.strictEqual(start.body.voice, 'chaim-light');
    assert.strictEqual(start.body.text, BOOK[0]);
    assert.strictEqual(start.body.rate, 1);
    const appended = svc.requests.filter(r => r.p === '/speak-live-append').map(r => r.body);
    assert.deepStrictEqual(JSON.parse(JSON.stringify(appended)), [{ text: BOOK[2], id: 1 }]);  // the empty section is skipped
    assert.strictEqual(spoken.length, 0, 'speechSynthesis was used');
    const hl = highlights(calls).map(h => [h.sectionIndex, h.range.start.utf16, h.range.end.utf16]);
    assert.deepStrictEqual(hl, [
      [0, 0, BOOK[0].indexOf('.') + 1],
      [0, BOOK[0].indexOf('והארץ'), BOOK[0].length],
      [2, 0, BOOK[2].indexOf('.') + 1],
      [2, BOOK[2].indexOf('ויהי'), BOOK[2].length]
    ]);
    assert.ok(calls.some(c => c.method === 'reader.scrollToSection' && c.payload.sectionIndex === 2), 'no scroll to the next section');
    assert.ok(messages(calls).includes('ההקראה הגיעה לסוף הספר.'), JSON.stringify(messages(calls)));
    assert.ok(calls.some(c => c.method === 'reader.clearHighlight'));
  });

  await test('pause pauses the service and saves the place; play resumes the same reading', async () => {
    const svc = fakeService('ok');
    const storage = { mp3Voice: 'chaim-light' };
    const { handlers, calls, ctx } = load(svc, storage);
    await play(handlers);
    await until(() => highlights(calls).length >= 2);
    await play(handlers);  // pause
    assert.ok(paths(svc).includes('/speak-live-pause'));
    assert.strictEqual(ctx.state.playing, false);
    assert.strictEqual(storage.position.bookId, 'b');
    assert.strictEqual(storage.position.utf16, BOOK[0].indexOf('והארץ'));
    const shown = highlights(calls).length;
    await sleep(60);
    assert.strictEqual(highlights(calls).length, shown, 'the highlight moved while paused');
    await play(handlers);  // resume
    assert.ok(paths(svc).includes('/speak-live-resume'));
    assert.strictEqual(paths(svc).filter(p => p === '/speak-live').length, 1, 'resuming started a new reading');
    await until(() => !ctx.state.playing && paths(svc).includes('/speak-live-stop'));
    assert.deepStrictEqual(highlights(calls).map(h => h.sectionIndex), [0, 0, 2, 2]);
  });

  await test('stop ends the service\'s reading', async () => {
    const svc = fakeService('ok');
    const { handlers, calls, ctx } = load(svc, { mp3Voice: 'chaim-light' });
    await play(handlers);
    await until(() => highlights(calls).length >= 1);
    await handlers['reader.toolbar_item_clicked']({ itemId: 'stop' });
    assert.ok(paths(svc).includes('/speak-live-stop'));
    assert.strictEqual(ctx.state.playing, false);
    assert.strictEqual(ctx.state.live, null);
    const shown = highlights(calls).length;
    await sleep(60);
    assert.strictEqual(highlights(calls).length, shown);
  });

  await test('without the service the light voice falls back to the system voice', async () => {
    const svc = fakeService('down');
    const { handlers, calls, spoken } = load(svc, { mp3Voice: 'chaim-light' });
    await play(handlers);
    await until(() => spoken.length >= 2);
    assert.ok(messages(calls).some(m => m.includes('בקול של Windows')), JSON.stringify(messages(calls)));
    assert.strictEqual(spoken[0], BOOK[0]);
  });

  await test('a service too old for live reading says so and reads in the system voice', async () => {
    const svc = fakeService('old');
    const { handlers, calls, spoken } = load(svc, { mp3Voice: 'chaim-light' });
    await play(handlers);
    await until(() => spoken.length >= 1);
    assert.ok(messages(calls).some(m => m.includes('ישן מדי')), JSON.stringify(messages(calls)));
  });

  await test('a missing light model offers its download instead of reading', async () => {
    const svc = fakeService('nomodel');
    const { handlers, calls, spoken, ctx } = load(svc, { mp3Voice: 'chaim-light' });
    await play(handlers);
    await until(() => calls.some(c => c.method === 'ui.showConfirm'));
    assert.strictEqual(spoken.length, 0);
    assert.strictEqual(ctx.state.playing, false);
  });

  await test('the system voice still reads through speechSynthesis, word by word, without the service', async () => {
    const svc = fakeService('ok');
    const { handlers, spoken } = load(svc, { mp3Voice: '' });
    await play(handlers);
    await until(() => spoken.length >= 2);
    assert.deepStrictEqual(spoken, [BOOK[0], BOOK[2]]);
    assert.ok(!paths(svc).some(p => p.startsWith('/speak-live')), JSON.stringify(paths(svc)));
  });

  await test('a recorded voice saves only: reading stays in the system voice', async () => {
    const svc = fakeService('ok');
    const { handlers, spoken } = load(svc, { mp3Voice: 'chaim' });
    await play(handlers);
    await until(() => spoken.length >= 1);
    assert.ok(!paths(svc).some(p => p.startsWith('/speak-live')));
  });

  await test('reading a selection in the light voice reads just that range', async () => {
    const svc = fakeService('ok');
    const { handlers, calls, ctx } = load(svc, { mp3Voice: 'chaim-light' });
    await settle();
    const from = BOOK[0].indexOf('והארץ');
    const range = { start: { utf16: from }, end: { utf16: BOOK[0].length } };
    handlers['reader.context_menu_item_clicked']({
      itemId: 'tts-read-selection',
      selection: { sections: [{ bookId: 'b', sectionIndex: 0, renderedRange: range, sourceRange: range }] }
    });
    await until(() => paths(svc).includes('/speak-live'));
    await until(() => !ctx.state.playing);
    assert.strictEqual(svc.requests.find(r => r.p === '/speak-live').body.text, 'והארץ היתה תהו ובהו.');
    assert.deepStrictEqual(highlights(calls).map(h => [h.range.start.utf16, h.range.end.utf16]), [[from, BOOK[0].length]]);
  });

  if (failed) {
    console.log(failed + ' failed');
    process.exit(1);
  }
  console.log('liveRead: all passed');
})();

function settle() { return sleep(30); }
