/* ---------------------------------------------------------------------------
   חיפוש בדיבור.

   The speech itself is recognized by the local voice service (otzaria-voice.exe,
   http://127.0.0.1:47811): it records from the microphone and streams text back
   while the user speaks. This page only starts and stops it, shows the text, and
   hands the result to Otzaria's search.
--------------------------------------------------------------------------- */

'use strict';

// ----------------------------------------------------------------- service

const SERVICE = 'http://127.0.0.1:47811';
const HEADERS = { 'X-Otzaria-Voice': '1', 'Content-Type': 'application/json' };
const SERVICE_HELP =
  'שירות הדיבור הוא תוכנה קטנה שרצה ברקע במחשב ומזהה את הדיבור בלי אינטרנט.\n' +
  'אם הוא מותקן, הפעילו אותו מחדש (או הפעילו מחדש את המחשב).\n' +
  'אם לא — הורידו את otzaria-voice.exe ופתחו אותו פעם אחת בלחיצה כפולה;\n' +
  'הוא יתקין את עצמו ויעלה מעצמו בכל הפעלה של המחשב.';

class ServiceError extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
}

/**
 * One HTTP exchange with the service through network.fetchStream, yielding the
 * newline-delimited JSON objects of the body as they arrive. Chunk boundaries do
 * not follow lines, so partial lines are carried over.
 */
async function* serviceLines(path, options) {
  const o = options || {};
  let it;
  try {
    const stream = Otzaria.call('network.fetchStream', {
      url: SERVICE + path,
      method: o.method || 'POST',
      headers: HEADERS,
      body: o.body || '',
      timeoutMs: o.timeoutMs || 120000,
    });
    it = stream[Symbol.asyncIterator]();
  } catch (e) {
    throw new ServiceError('unreachable', String((e && e.message) || e));
  }
  if (o.signal) o.signal.close = () => it.return && it.return();
  let pending = '';
  let status = 200;
  let errorBody = '';
  try {
    while (true) {
      let step;
      try {
        step = await it.next();
      } catch (e) {
        throw new ServiceError('unreachable', String((e && e.message) || e));
      }
      if (step.done) break;
      const chunk = step.value;
      if (chunk.type === 'response') {
        status = chunk.status;
        continue;
      }
      if (status !== 200) {
        errorBody += chunk.body;
        continue;
      }
      pending += chunk.body;
      const lines = pending.split('\n');
      pending = lines.pop() || '';
      for (const line of lines) if (line.trim()) yield JSON.parse(line);
    }
  } finally {
    if (o.signal) o.signal.close = null;
  }
  if (status !== 200) {
    let err = {};
    try { err = JSON.parse(errorBody); } catch (_) { /* not JSON */ }
    throw new ServiceError(err.error || 'http_' + status, err.message || 'HTTP ' + status);
  }
  if (pending.trim()) yield JSON.parse(pending);
}

async function serviceCall(path, method) {
  let last = null;
  for await (const obj of serviceLines(path, { method, timeoutMs: 5000 })) last = obj;
  return last;
}

// ------------------------------------------------------------------- state

const els = {};
let state = 'checking';   // checking | unavailable | idle | starting | listening | stopping
let session = null;       // { segments, base, signal, cancelled }
let autoSearch = true;

function setStatus(text) { els.status.textContent = text; }

function setState(next) {
  state = next;
  const mic = els.mic;
  mic.classList.toggle('listening', next === 'listening' || next === 'starting');
  mic.classList.toggle('busy', next === 'starting' || next === 'stopping');
  mic.disabled = !(next === 'idle' || next === 'listening' || next === 'starting');
  mic.setAttribute('aria-label', next === 'idle' ? 'התחלת דיבור' : 'סיום דיבור');
  els.transcript.contentEditable = next === 'idle' || next === 'unavailable' ? 'true' : 'false';
  updateSearchButton();
}

function currentText() {
  return els.transcript.textContent.replace(/\s+/g, ' ').trim();
}

function updateSearchButton() {
  els.search.disabled = !(state === 'idle' || state === 'unavailable') || !currentText();
}

function showNotice(title, text, canDownload) {
  els.noticeTitle.textContent = title;
  els.noticeText.textContent = text;
  els.download.hidden = !canDownload;
  els.progress.hidden = true;
  els.notice.hidden = false;
}

function hideNotice() {
  els.notice.hidden = true;
  stopPolling();
}

/** The transcript: what was there before, finished segments, then the draft. */
function render() {
  const s = session;
  const keys = Object.keys(s.segments).map(Number).sort((a, b) => a - b);
  const parts = [];
  for (const k of keys) {
    const seg = s.segments[k];
    if (seg.text) parts.push({ text: seg.text, draft: !seg.final });
  }
  let html = esc(s.base);
  parts.forEach((p, i) => {
    const sep = s.base || i > 0 ? ' ' : '';
    html += p.draft
      ? esc(sep) + '<span class="draft">' + esc(p.text) + '</span>'
      : esc(sep + p.text);
  });
  els.transcript.innerHTML = html;
  els.mic.classList.toggle('heard', parts.some((p) => p.draft));
}

// ---------------------------------------------------------------- dictation

async function checkService() {
  if (session) return;
  setState('checking');
  setStatus('בודק את שירות הדיבור…');
  try {
    const st = await serviceCall('/status', 'GET');
    if (!st || st.service !== 'otzaria-voice') throw new ServiceError('unreachable', 'no service');
    if (!st.runtime) {
      showNotice('חסר רכיב של אוצריא',
        'שירות הדיבור משתמש במנוע שמותקן עם אוצריא, ולא מצא אותו.\nודאו שאוצריא מותקנת כרגיל.');
      setState('unavailable');
      setStatus('');
      return;
    }
    if (!st.pack || PACK_BUSY.includes(st.packJob && st.packJob.state)) {
      showNotice('חבילת הדיבור חסרה',
        'כדי לזהות עברית בלי אינטרנט צריך להוריד פעם אחת חבילת דיבור של כ-' +
        megabytes(st.packSize) + ' מגה-בייט.\n' +
        'אחרי ההורדה הזיהוי פועל במחשב עצמו, וההקלטות אינן נשלחות לשום מקום.', true);
      setState('unavailable');
      setStatus('');
      if (PACK_BUSY.includes(st.packJob && st.packJob.state)) showPackJob(st.packJob);
      return;
    }
    hideNotice();
    showHotkeyTip(st.hotkey);
    setState('idle');
    setStatus('לחצו על המיקרופון ודברו');
  } catch (e) {
    showNotice('שירות הדיבור לא פועל', SERVICE_HELP);
    setState('unavailable');
    setStatus('');
  }
}

/** The service's key combination types into any field, Otzaria's own included. */
function showHotkeyTip(hotkey) {
  if (!hotkey) {
    els.tip.hidden = true;
    return;
  }
  const keys = hotkey.split('+').map((k) => '<kbd>' + esc(k === 'Space' ? 'רווח' : k) + '</kbd>');
  els.tip.innerHTML = 'טיפ: בכל שדה כתיבה — גם בשדות החיפוש של אוצריא ובוורד — לחצו ' +
    keys.join('+') + ' ודברו; לחיצה נוספת מסיימת.';
  els.tip.hidden = false;
}

// ------------------------------------------------------------ voice pack

const PACK_BUSY = ['downloading', 'verifying', 'extracting'];
let pollTimer = null;

function megabytes(bytes) { return Math.round((bytes || 0) / 1048576); }

function stopPolling() {
  if (pollTimer) clearTimeout(pollTimer);
  pollTimer = null;
}

/**
 * The service downloads on its own (a fetchStream request is capped at two
 * minutes, the download may take longer); this page only shows its progress.
 */
function showPackJob(job) {
  els.download.hidden = true;
  els.progress.hidden = false;
  const fraction = job.total ? job.received / job.total : 0;
  els.progress.classList.toggle('indeterminate', job.state === 'extracting');
  els.bar.style.width = Math.round(fraction * 100) + '%';
  els.noticeText.textContent =
    job.state === 'downloading'
      ? 'מוריד… ' + Math.round(fraction * 100) + '% (' + megabytes(job.received) + ' מתוך ' +
        megabytes(job.total) + ' מגה-בייט).\nאפשר להמשיך לעבוד בינתיים; ההורדה נמשכת גם אם סוגרים את הלשונית.'
      : job.state === 'verifying'
        ? 'בודק שהקובץ שהורד תקין…'
        : 'מתקין את חבילת הדיבור…';
  stopPolling();
  pollTimer = setTimeout(pollPackJob, 700);
}

async function pollPackJob() {
  pollTimer = null;
  let st;
  try {
    st = await serviceCall('/status', 'GET');
  } catch (_) {
    showNotice('שירות הדיבור לא פועל', SERVICE_HELP);
    return;
  }
  const job = st.packJob || {};
  if (PACK_BUSY.includes(job.state)) {
    showPackJob(job);
  } else if (job.state === 'error') {
    showNotice('ההורדה לא הצליחה', job.error, true);
    els.download.textContent = 'לנסות שוב';
  } else if (st.pack) {
    Otzaria.call('ui.showSuccess', { message: 'חבילת הדיבור הותקנה. לחצו על המיקרופון ודברו' });
    checkService();
  }
}

async function downloadPack() {
  els.download.disabled = true;
  try {
    const res = await serviceCall('/install-pack', 'POST');
    if (res && res.packJob) showPackJob(res.packJob);
  } catch (e) {
    showNotice('ההורדה לא הצליחה', e.message, true);
  } finally {
    els.download.disabled = false;
  }
}

async function start() {
  if (state !== 'idle') return;
  const base = currentText();
  const s = { segments: {}, base, signal: {}, cancelled: false };
  session = s;
  setState('starting');
  setStatus('פותח את המיקרופון…');
  let ended = false;
  try {
    for await (const ev of serviceLines('/dictate', { body: '{}', signal: s.signal })) {
      if (session !== s) break;
      if (ev.type === 'listening') {
        setStatus('טוען את מנוע הדיבור… אפשר כבר לדבר');
      } else if (ev.type === 'ready') {
        if (state === 'starting') setState('listening');
        setStatus('מקשיב — דברו. לחיצה נוספת מסיימת');
      } else if (ev.type === 'update') {
        s.segments[ev.segment] = { text: ev.text, final: ev.final };
        render();
      } else if (ev.type === 'error') {
        Otzaria.call('ui.showError', { message: 'זיהוי הדיבור נכשל: ' + ev.message });
      } else if (ev.type === 'end') {
        ended = ev.reason === 'stopped' || ev.reason === 'time_limit';
        if (ev.reason === 'time_limit') {
          Otzaria.call('ui.showMessage', { message: 'הדיבור הסתיים אחרי 100 שניות' });
        }
      }
    }
  } catch (e) {
    if (e.code === 'microphone') {
      Otzaria.call('ui.showError', { message: 'אין גישה למיקרופון: ' + e.message });
    } else if (e.code === 'pack_missing' || e.code === 'pack_installing') {
      session = null;
      checkService();
      return;
    } else if (!s.cancelled) {
      showNotice('שירות הדיבור לא פועל', SERVICE_HELP);
    }
  }
  if (session !== s) return;
  session = null;
  // Drafts that never became final stay as text: the user saw them.
  els.transcript.textContent = currentText();
  els.mic.classList.remove('heard');
  setState('idle');
  setStatus('לחצו על המיקרופון ודברו');
  const text = currentText();
  if (ended && autoSearch && text && text !== base) search();
}

async function stop() {
  if (state !== 'listening' && state !== 'starting') return;
  setState('stopping');
  setStatus('משלים את המשפט האחרון…');
  try {
    await serviceCall('/stop', 'POST');
  } catch (_) {
    // The dictation stream reports the outcome either way.
  }
}

/** Leaving the tab: drop the recording rather than keep the microphone open. */
function cancel() {
  const s = session;
  if (!s) return;
  s.cancelled = true;
  session = null;
  if (s.signal.close) s.signal.close();
  serviceCall('/cancel', 'POST').catch(() => {});
  els.mic.classList.remove('heard');
  setState('idle');
  setStatus('לחצו על המיקרופון ודברו');
}

async function search() {
  const query = currentText();
  if (!query) return;
  const res = await Otzaria.call('reader.openSearchTab', { query });
  if (!res || !res.success) {
    Otzaria.call('ui.showError', {
      message: 'פתיחת החיפוש נכשלה: ' + (res && res.error ? res.error.message : ''),
    });
  }
}

// --------------------------------------------------------------- lifecycle

async function onBoot(payload) {
  applyTheme(payload.theme);
  const saved = await Otzaria.call('storage.get', { key: 'autoSearch' });
  if (saved && saved.success && typeof saved.data === 'boolean') autoSearch = saved.data;
  els.auto.checked = autoSearch;
  await checkService();
}

function bind() {
  for (const id of ['mic', 'status', 'transcript', 'search', 'clear', 'auto', 'notice', 'retry',
    'download', 'progress', 'bar', 'tip']) {
    els[id] = document.getElementById(id);
  }
  els.noticeTitle = document.getElementById('notice-title');
  els.noticeText = document.getElementById('notice-text');

  els.mic.addEventListener('click', () => (state === 'idle' ? start() : stop()));
  els.search.addEventListener('click', search);
  els.clear.addEventListener('click', () => {
    els.transcript.textContent = '';
    updateSearchButton();
  });
  els.transcript.addEventListener('input', updateSearchButton);
  els.transcript.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      search();
    }
  });
  els.auto.addEventListener('change', () => {
    autoSearch = els.auto.checked;
    Otzaria.call('storage.set', { key: 'autoSearch', value: autoSearch });
  });
  els.retry.addEventListener('click', checkService);
  els.download.addEventListener('click', downloadPack);
  document.getElementById('about-btn').addEventListener('click', () => {
    Otzaria.call('ui.showMessage', {
      message: 'חיפוש בדיבור 1.0.0 · הזיהוי נעשה במחשב שלכם בשירות הדיבור המקומי, ' +
        'וההקלטות אינן נשלחות לשום מקום.',
    });
  });
}

bind();
Otzaria.on('plugin.boot', onBoot);
Otzaria.on('plugin.suspended', () => { cancel(); stopPolling(); });
Otzaria.on('plugin.resumed', () => { if (!session) checkService(); });
Otzaria.on('theme.changed', applyTheme);
