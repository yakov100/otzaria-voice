// ------------------------------------------------------------- shortcut
//
// The keyboard shortcut (default Ctrl+Alt+V; Settings -> Keyboard shortcuts ->
// plugin shortcuts): listen, show the words as they are recognized, and open a
// search tab that searches the text at once. (A tab opened without searching
// shows an empty page with no visible search field in a narrow window, so the
// user saw nothing at all.) Until 2.3.0 a microphone button in the reader's
// toolbar ran this too; from 2.3.1 the microphone is inside the search fields
// (field_actions.js), and the toolbar button is gone.

let searchSession = false;

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
  } finally {
    searchSession = false;
  }
}
