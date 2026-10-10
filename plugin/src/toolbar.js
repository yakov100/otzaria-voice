// ------------------------------------------------- toolbar button and shortcut
//
// The microphone button in the reader's toolbar and the keyboard shortcut
// (default Ctrl+Alt+V; Settings -> Keyboard shortcuts -> plugin shortcuts) both
// run the same thing: listen, show the words as they are recognized, and open a
// search tab that searches the text at once. (A tab opened without searching
// shows an empty page with no visible search field in a narrow window, so the
// user saw nothing at all.)

const TOOLBAR_ID = 'dictate';
const TOOLBAR_IDLE = { icon: 'mic_24_regular', title: 'חיפוש בדיבור' };
const TOOLBAR_ACTIVE = { icon: 'mic_24_filled', title: 'סיום הדיבור' };

let searchSession = false;

function setToolbar(patch) {
  return Otzaria.call('reader.updateToolbarItem', { id: TOOLBAR_ID, patch }).catch(() => {});
}

async function toggleSearchDictation() {
  if (isDictating()) {
    await stopDictation();
    return;
  }
  if (searchSession) return; // still checking the service
  searchSession = true;
  try {
    const st = await readyToDictate();
    if (!st) return;
    setToolbar(TOOLBAR_ACTIVE);
    say('פותח את המיקרופון… אפשר כבר לדבר');
    let result;
    try {
      result = await dictate({}, {
        onReady: () => say('מקשיב — דברו. לחיצה נוספת מסיימת'),
        onText: (text) => caption.show('מקשיב: ' + text),
      });
    } catch (e) {
      caption.cancel();
      await explainFailure(e, st);
      return;
    }
    caption.cancel();
    if (result.reason === 'cancelled') return;
    if (!result.text) {
      say(result.reason === 'no_speech' ? 'לא נשמע דיבור' : 'לא זוהה טקסט');
      return;
    }
    let res;
    try {
      res = await Otzaria.call('reader.openSearchTab', { query: result.text, autoSearch: true });
    } catch (e) {
      res = { success: false, error: { message: String((e && e.message) || e) } };
    }
    if (!res || !res.success) {
      sayError('פתיחת החיפוש נכשלה: ' + (res && res.error ? res.error.message : ''));
      return;
    }
    if (result.reason === 'time_limit') say('הדיבור הסתיים אחרי 100 שניות — מחפש: ' + result.text);
    else say('מחפש: ' + result.text);
    hotkeyTipOnce(st);
  } finally {
    searchSession = false;
    setToolbar(TOOLBAR_IDLE);
  }
}
