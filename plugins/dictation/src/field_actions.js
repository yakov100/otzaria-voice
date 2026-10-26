// -------------------------------------------- microphone inside search fields
//
// Otzaria 0.9.99 and later draw the plugin's microphone inside its search
// fields (contributes.startup.searchFieldActions). A click opens a session for
// that field; the recognized words are written into the field as they come,
// around what was there before. A second click, or a pause after a sentence,
// ends it; the user presses Enter to search. See
// docs/dictation/search-field-actions-contract.md.
//
// Only the 0.9.99 build of the plugin contains this file (plugins/dictation/build.py).

const FIELD_ACTION_ID = 'dictate';
let fieldSession = null; // { sessionId, sent, pending, writing }

function fieldCall(method, payload) {
  return Otzaria.call(method, payload).then((res) => res || { success: false });
}

/** Writes the newest text into the field; one call at a time, newest wins. */
function writeField(fs, text) {
  fs.pending = text;
  if (!fs.writing) fs.writing = drainWrites(fs);
  return fs.writing;
}

async function drainWrites(fs) {
  await null; // let writeField store this promise before the loop can finish
  try {
    while (fs.pending !== null && fieldSession === fs) {
      const next = fs.pending;
      fs.pending = null;
      if (next === fs.sent) continue;
      const res = await fieldCall('search.setFieldText', { sessionId: fs.sessionId, text: next });
      if (res.success) {
        fs.sent = next;
      } else if (res.error && res.error.code === 'error.not_found') {
        // The field is gone or the user typed in it: stop listening.
        if (fieldSession === fs) {
          fieldSession = null;
          cancelDictation();
        }
      }
    }
  } finally {
    fs.writing = null;
  }
}

async function onFieldActionInvoked(ev) {
  if (!ev || ev.actionId !== FIELD_ACTION_ID) return;
  // A click in another field replaces the session there (the host ends it).
  if (isDictating()) cancelDictation();
  const fs = { sessionId: ev.sessionId, sent: '', pending: null, writing: null };
  fieldSession = fs;
  const end = () => fieldCall('search.endFieldSession', { sessionId: fs.sessionId, submit: false });
  await fieldCall('search.setFieldActionState', {
    sessionId: fs.sessionId,
    state: 'busy',
    tooltip: 'פותח את המיקרופון…',
  });
  const st = await readyToDictate();
  if (!st || fieldSession !== fs) {
    if (fieldSession === fs) fieldSession = null;
    await end();
    return;
  }
  const before = String(ev.text || '').slice(0, Math.max(0, ev.selectionStart || 0));
  let result;
  try {
    result = await dictate({ before }, {
      onReady: () =>
        fieldCall('search.setFieldActionState', {
          sessionId: fs.sessionId,
          state: 'active',
          tooltip: 'מקשיב — דברו. לחיצה נוספת מסיימת',
        }),
      onText: (text) => {
        if (fieldSession === fs) writeField(fs, text);
      },
    });
  } catch (e) {
    if (fieldSession === fs) fieldSession = null;
    await end();
    await explainFailure(e, st);
    return;
  }
  if (fieldSession !== fs) return; // cancelled: the host already ended it
  if (result.text) await writeField(fs, result.text);
  fieldSession = null;
  await end();
  if (result.reason === 'no_speech') say('לא נשמע דיבור');
  else if (result.reason === 'time_limit') say('הדיבור הסתיים אחרי 100 שניות');
}

function onFieldActionStop(ev) {
  if (fieldSession && ev && ev.sessionId === fieldSession.sessionId) stopDictation();
}

function onFieldActionEnded(ev) {
  if (!fieldSession || !ev || ev.sessionId !== fieldSession.sessionId) return;
  if (ev.reason === 'plugin') return;
  // closed / edited / replaced: the text is no longer ours to write.
  fieldSession = null;
  cancelDictation();
}

function installFieldActions() {
  Otzaria.on('search.fieldAction.invoked', onFieldActionInvoked);
  Otzaria.on('search.fieldAction.stopRequested', onFieldActionStop);
  Otzaria.on('search.fieldAction.ended', onFieldActionEnded);
}
