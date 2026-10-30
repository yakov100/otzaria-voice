// -------------------------------------------- microphone inside search fields
//
// Otzaria 0.9.98.1 and later draw the plugin's microphone inside its search
// fields (contributes.startup.searchFieldActions). A click opens a session for
// that field; the recognized words are written into the field as they come,
// around what was there before. A second click, or a pause after a sentence,
// ends it; the user presses Enter to search. See
// docs/dictation/search-field-actions-contract.md.
//
// Every build contains this file since plugin 2.3.0, whose minAppVersion is 0.9.98.1
// (plugins/dictation/build.py); users of Otzaria 0.9.98 keep plugin 2.2.x.

const FIELD_ACTION_ID = 'dictate';
// Otzaria refuses search.setFieldText with a longer text (the contract;
// PluginSearchFieldSessionService.maxTextLength). The plugin always sends its
// whole text, so a longer dictation cannot be written in parts: it stops there,
// the user is told, and the field keeps the last text that fit.
const FIELD_TEXT_MAX = 1000;
let fieldSession = null; // { sessionId, sent, pending, writing, full }

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
      if (next === fs.sent || fs.full) continue;
      if (next.length > FIELD_TEXT_MAX) {
        fs.full = true;
        say('שדה החיפוש מקבל עד 1,000 תווים, ולכן ההכתבה נעצרה. מה שנכתב עד כאן נשאר בשדה.');
        cancelDictation();
        continue;
      }
      const res = await fieldCall('search.setFieldText', { sessionId: fs.sessionId, text: next });
      if (res.success) {
        fs.sent = next;
      } else if (res.error && res.error.code === 'error.not_found') {
        // The field is gone or the user typed in it: stop listening.
        if (fieldSession === fs) {
          fieldSession = null;
          cancelDictation();
        }
      } else {
        console.warn('voice-search: the field refused the text', JSON.stringify(res.error));
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
  const fs = { sessionId: ev.sessionId, sent: '', pending: null, writing: null, full: false };
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
