// ---------------------------------------------------------------- dictation
//
// One dictation at a time (the service has one microphone). The service ends
// it by itself after a finished sentence and a short silence, or when nothing
// is said at all; a second press ends it at once.

const END_AFTER_SILENCE_MS = 1500;
const NO_SPEECH_MS = 8000;

let current = null; // { id, signal, segments, stopping, cancelled }
let nextId = 1;

function isDictating() {
  return current !== null;
}

function composeText(segments) {
  return Object.keys(segments)
    .map(Number)
    .sort((a, b) => a - b)
    .map((k) => segments[k])
    .filter((t) => t)
    .join(' ')
    .replace(/\s+/g, ' ')
    .trim();
}

/** The last two words before the cursor help the language model continue them. */
function contextWords(text) {
  return String(text || '')
    .split(/\s+/)
    .filter((w) => w)
    .slice(-2);
}

/**
 * Runs one dictation. [hooks]: onReady(), onText(text, isFinal).
 * Resolves to { text, reason } — reason as the service reports it
 * (stopped | silence | no_speech | time_limit | cancelled | error).
 * Throws ServiceError when it could not start; the caller explains it.
 */
async function dictate(options, hooks) {
  if (current) throw new ServiceError('busy', 'already dictating');
  const s = { id: nextId++, signal: {}, segments: {}, stopping: false, cancelled: false };
  current = s;
  const body = JSON.stringify({
    context: contextWords(options && options.before),
    endAfterSilenceMs: END_AFTER_SILENCE_MS,
    noSpeechMs: NO_SPEECH_MS,
  });
  let reason = 'error';
  try {
    for await (const ev of serviceLines('/dictate', { body, signal: s.signal })) {
      if (s.cancelled) break;
      if (ev.type === 'ready') {
        if (hooks.onReady) hooks.onReady();
      } else if (ev.type === 'update') {
        s.segments[ev.segment] = ev.text;
        if (hooks.onText) hooks.onText(composeText(s.segments), ev.final);
      } else if (ev.type === 'error') {
        sayError('זיהוי הדיבור נכשל: ' + ev.message);
      } else if (ev.type === 'end') {
        reason = ev.reason;
      }
    }
  } finally {
    if (current === s) current = null;
  }
  if (s.cancelled) reason = 'cancelled';
  return { text: composeText(s.segments), reason };
}

/** Ends the dictation after the last sentence is recognized. */
async function stopDictation() {
  const s = current;
  if (!s || s.stopping) return;
  s.stopping = true;
  try {
    await serviceCall('/stop', 'POST');
  } catch (_) {
    // the dictation stream reports the outcome either way
  }
}

/** Drops the dictation and its text at once. */
function cancelDictation() {
  const s = current;
  if (!s) return;
  s.cancelled = true;
  current = null;
  if (s.signal.close) s.signal.close();
  serviceCall('/cancel', 'POST').catch(() => {});
}

/**
 * Checks the service before opening the microphone, so that a missing
 * service or voice pack is explained instead of failing silently.
 * Returns the status, or null after explaining the problem.
 */
async function readyToDictate() {
  let st;
  try {
    st = await serviceStatus();
  } catch (e) {
    await explainNoService(platformName);
    return null;
  }
  if (!st.runtime) {
    sayError('שירות הדיבור לא מצא את רכיב ONNX Runtime של אוצריא; ודאו שאוצריא מותקנת כרגיל');
    return null;
  }
  // First, so that a service about to be replaced does not download the old voice pack.
  if (await offerServiceUpdate(st)) return null;
  if (!st.pack || PACK_BUSY.includes(st.packJob && st.packJob.state)) {
    await offerPack(st);
    return null;
  }
  if (st.microphone === false) {
    await explainFailure(new ServiceError('microphone_background', ''), st);
    return null;
  }
  if (await offerPackUpdate(st)) return null;
  return st;
}

/** Once: the service's own key combination types by voice into any field (Windows). */
async function hotkeyTipOnce(st) {
  if (!st || !st.hotkey) return;
  const seen = await Otzaria.call('storage.get', { key: 'hotkeyTipShown' });
  if (seen && seen.success && seen.data) return;
  await Otzaria.call('storage.set', { key: 'hotkeyTipShown', value: true });
  say(
    'טיפ: ' + st.hotkey.replace('Space', 'רווח') +
      ' מפעיל הקלדה בדיבור בכל שדה כתיבה במחשב — גם בשדות החיפוש של אוצריא',
  );
}
