/* ---------------------------------------------------------------------------
   הקראה קולית — headless Otzaria plugin.

   Reads the open text book aloud with the browser's speechSynthesis, section
   by section, highlighting the word being spoken. Can also save a chapter or
   the selection as an MP3 (through the local voice service). Runs only in the background
   engine (no tab), so reading continues while the user looks at the book.

   Flow: toolbar click / context-menu click / shortcut -> boot (lazy) -> event.
   The engine is shut down by Otzaria after ~3 idle minutes, so anything that
   must outlive a pause (the reading position, the rate) goes to storage.
--------------------------------------------------------------------------- */

'use strict';

const TOOLBAR_ID = 'tts-main';
const HIGHLIGHT_ID = 'tts-word';
const MAX_CHUNK = 280;            // chars per utterance; split at sentence ends
const MAX_EMPTY_SECTIONS = 40;    // give up after this many blank lines in a row
const RATES = { 'rate-slow': 0.8, 'rate-normal': 1.0, 'rate-fast': 1.3, 'rate-faster': 1.6 };
const RATE_LABELS = { 'rate-slow': 'איטית', 'rate-normal': 'רגילה', 'rate-fast': 'מהירה', 'rate-faster': 'מהירה מאוד' };
// Voices for saving as MP3: the system voice, or a recorded voice made by the
// local service (slower: preparing takes about the length of the audio).
// The ids are .otzvoice files in the service's voices folders (GET /status
// lists them under "voices"); every installed voice, including one made with
// tools/tts/make_voice_from_wav.py, gets a 'voice-<id>' menu item (see
// refreshVoiceMenu). The manifest declares the menu as it looks before the
// first refresh: the system voice and the voice the model pack ships with.
const SYSTEM_VOICE_ITEM = 'voice-system';
const VOICE_ITEM_PREFIX = 'voice-';
const PACK_VOICES = ['chaim'];                      // always offered: choosing one fetches the pack
const RECORDED_NAMES = { chaim: 'חיים' };           // display names; other ids are shown as they are
// The light voice (ids end in '-light'): a small, fast model trained on the
// recorded voice; its own download (GET /status: lightVoices, lightModel,
// lightModelSize). Always offered too, so that choosing it starts the download.
const LIGHT_SUFFIX = '-light';
const LIGHT_VOICES = ['chaim-light'];
const LIGHT_NAMES = { 'chaim-light': 'חיים קל' };
const MAX_MENU_CHILDREN = 20;                       // Otzaria's limit per menu

function isLightVoice(voice) {
  return typeof voice === 'string' && voice.endsWith(LIGHT_SUFFIX) && voice.length > LIGHT_SUFFIX.length;
}

/** The menu item id of a voice ('' = the system voice). */
function voiceItemId(voice) {
  return voice ? VOICE_ITEM_PREFIX + voice : SYSTEM_VOICE_ITEM;
}

/** The voice of a menu item id, or null when it is not a voice item. */
function voiceOfItem(itemId) {
  if (itemId === SYSTEM_VOICE_ITEM) return '';
  if (typeof itemId === 'string' && itemId.startsWith(VOICE_ITEM_PREFIX) && itemId.length > VOICE_ITEM_PREFIX.length) {
    return itemId.slice(VOICE_ITEM_PREFIX.length);
  }
  return null;
}

/** 'אסף (הקול של Windows)', 'חיים (קול מוקלט)', 'חיים קל (מהיר)', 'moshe (קול מוקלט)'. */
function voiceLabel(voice) {
  if (!voice) return 'אסף (הקול של Windows)';
  if (isLightVoice(voice)) return (LIGHT_NAMES[voice] || voice) + ' (מהיר)';
  return (RECORDED_NAMES[voice] || voice) + ' (קול מוקלט)';
}

/** The menu title of a voice: 'קול לשמירה: חיים (מוקלט)', 'קול לשמירה: חיים קל (מהיר)'. */
function voiceMenuTitle(voice) {
  if (!voice) return 'קול לשמירה: אסף (של Windows)';
  if (isLightVoice(voice)) return 'קול לשמירה: ' + (LIGHT_NAMES[voice] || voice) + ' (מהיר)';
  return 'קול לשמירה: ' + (RECORDED_NAMES[voice] || voice) + ' (מוקלט)';
}

/** A voice id the menu can show: a file name, short, without control characters. */
function isUsableVoiceId(voice) {
  return typeof voice === 'string' && /^[^\u0000-\u001F\u007F]{1,60}$/.test(voice) && voice !== 'system';
}

/**
 * The voices to offer: the light voice first, then the pack's voices (both
 * always, so that choosing one can start its download), then every other
 * installed voice, light ones included.
 */
function menuVoices(installed, installedLight) {
  const out = LIGHT_VOICES.concat(PACK_VOICES);
  const add = list => (Array.isArray(list) ? list : []).forEach(v => {
    if (isUsableVoiceId(v) && !out.includes(v)) out.push(v);
  });
  add(installedLight);
  add(installed);
  return out;
}

/** The children of the toolbar menu, with one item per voice in [voices]. */
function menuChildren(voices) {
  const before = [
    { id: 'play', title: 'הקרא / השהה', icon: 'play_24_regular' },
    { id: 'stop', title: 'עצור והתחל מחדש', icon: 'stop_24_regular' },
    { id: 'save-chapter', title: 'שמור את הפרק כ-MP3', icon: 'fluent:arrow_download_24_regular' },
    { id: 'choose-folder', title: 'בחר תיקייה לשמירת MP3', icon: 'fluent:folder_open_24_regular' },
    { id: 'cancel-export', title: 'בטל את שמירת ה-MP3', icon: 'fluent:dismiss_circle_24_regular' },
    { id: 'rate-slow', title: 'מהירות: איטית' },
    { id: 'rate-normal', title: 'מהירות: רגילה' },
    { id: 'rate-fast', title: 'מהירות: מהירה' },
    { id: 'rate-faster', title: 'מהירות: מהירה מאוד' },
    { id: SYSTEM_VOICE_ITEM, title: voiceMenuTitle('') }
  ];
  const after = [
    { id: 'remove-voice-model', title: 'הסר את הקול המוקלט מהמחשב', icon: 'fluent:delete_24_regular' },
    { id: 'remove-light-model', title: 'הסר את הקול הקל מהמחשב', icon: 'fluent:delete_24_regular' }
  ];
  const room = MAX_MENU_CHILDREN - before.length - after.length;
  const items = voices.slice(0, room).map(v => ({ id: voiceItemId(v), title: voiceMenuTitle(v) }));
  return before.concat(items, after);
}

const state = {
  gen: 0,                 // bumps on every start/stop; stale callbacks compare it
  playing: false,
  bookId: null,
  sectionIndex: 0,
  words: [],              // word tokens of the current section (rendered layer)
  wordPos: 0,             // index into words of the last word spoken
  map: null,              // spoken offset -> original offset (see toSpeech)
  rate: 1.0,
  mp3Voice: '',           // '' = the system voice; otherwise a recorded voice id
  voice: null,
  highlightColor: '#FFD54F66'
};

// ----------------------------------------------------------------- helpers

async function call(method, payload) {
  try {
    return await Otzaria.call(method, payload || {});
  } catch (e) {
    return { success: false, error: { message: String(e) } };
  }
}

function message(text) {
  return call('ui.showMessage', { message: text });
}

function hebrewVoice() {
  const voices = window.speechSynthesis ? speechSynthesis.getVoices() : [];
  return voices.find(v => /^(he|iw)\b/i.test(v.lang) || /hebrew/i.test(v.name)) || null;
}

/** Voices load asynchronously; wait briefly for them the first time. */
function waitForVoice() {
  const v = hebrewVoice();
  if (v || !window.speechSynthesis) return Promise.resolve(v);
  return new Promise(resolve => {
    const done = () => resolve(hebrewVoice());
    speechSynthesis.addEventListener('voiceschanged', done, { once: true });
    setTimeout(done, 2000);
  });
}

/** '#RRGGBB' -> '#RRGGBB55' so the highlight follows the user's theme. */
function themeHighlight(theme) {
  const p = theme && theme.colorScheme && theme.colorScheme.primary;
  return /^#[0-9a-f]{6}$/i.test(p || '') ? p + '55' : '#FFD54F66';
}

async function savePosition() {
  if (!state.bookId) return;
  const word = state.words[state.wordPos];
  await call('storage.set', {
    key: 'position',
    value: {
      bookId: state.bookId,
      sectionIndex: state.sectionIndex,
      utf16: word && word.start ? word.start.utf16 : 0
    }
  });
}

async function loadSettings() {
  const res = await call('storage.get', { key: 'rate' });
  if (res.success && typeof res.data === 'number' && res.data > 0) state.rate = res.data;
  const v = await call('storage.get', { key: 'mp3Voice' });
  if (v.success && typeof v.data === 'string') state.mp3Voice = v.data;
}

async function setToolbarPlaying(playing) {
  // Best effort: a failure here must never stop the reading itself.
  await call('reader.updateToolbarItem', {
    id: TOOLBAR_ID,
    patch: {
      icon: playing ? 'pause_24_regular' : 'speaker_2_24_regular',
      title: playing ? 'השהה הקראה' : 'הקראה קולית'
    }
  });
}

// ------------------------------------------------------------ text loading

const NIKUD = /[ְ-ׇּׁׂ]/;

/** All words + text of one section in one layer, following pagination. */
async function loadLayer(bookId, sectionIndex, layer) {
  let cursor;
  let text = null;
  const words = [];
  for (let page = 0; page < 50; page++) {
    const args = { bookId, sectionIndex, layer, includeWords: true, limit: 2000 };
    if (cursor) args.cursor = cursor;
    const res = await call('reader.getSectionTextMap', args);
    if (!res.success) return { error: res.error };
    const d = res.data;
    if (text === null) text = (layer === 'source' ? d.sourceText : d.renderedText) || '';
    (d.words || []).forEach(w => words.push(w));
    if (!d.hasMore || !d.nextCursor) break;
    cursor = d.nextCursor;
  }
  return { text: text || '', words, layer };
}

/**
 * The text as displayed, unless the display hides nikud the book actually
 * has: the voice pronounces vocalized text far better, so then read the
 * source layer instead. Highlights use each word's sourceRange either way.
 */
async function loadSection(bookId, sectionIndex) {
  const rendered = await loadLayer(bookId, sectionIndex, 'rendered');
  if (rendered.error || NIKUD.test(rendered.text) || !rendered.text.trim()) return rendered;
  const source = await loadLayer(bookId, sectionIndex, 'source');
  return !source.error && NIKUD.test(source.text) ? source : rendered;
}

const DIVINE_NAME = /י[֑-ׇ]*ה[֑-ׇ]*ו[֑-ׇ]*ה[֑-ׇ]*(?![א-ת])/y;

// ------------------------------------------------- abbreviations and numerals

/**
 * Abbreviations read as their words. Keys are plain letters with " before
 * the last letter or ' at the end (the Hebrew gershayim/geresh are folded to
 * these first). Names of books and authors that are said as one word (רש"י,
 * רמב"ם) are in NAMES instead; anything else with " is spelt letter by letter.
 */
const ABBREVIATIONS = {
  // Talmudic discourse
  'ת"ר': 'תנו רבנן', 'ת"ש': 'תא שמע', 'ש"מ': 'שמע מינה', 'קמ"ל': 'קא משמע לן', 'מ"ט': 'מאי טעמא',
  'ה"נ': 'הכי נמי', 'ה"מ': 'הני מילי', 'ה"ק': 'הכי קאמר', 'ה"ד': 'היכי דמי', 'ה"ל': 'הוה ליה',
  'הו"א': 'הוה אמינא', 'סד"א': 'סלקא דעתך אמינא', 'ס"ד': 'סלקא דעתך', 'א"ל': 'אמר ליה', 'א"ר': 'אמר רבי',
  'אר"י': 'אמר רבי יוחנן', 'ארשב"ל': 'אמר ריש לקיש', 'ל"ק': 'לא קשיא', 'ל"ש': 'לא שנא', 'מ"ש': 'מאי שנא',
  'א"נ': 'אי נמי', 'א"כ': 'אם כן', 'אא"כ': 'אלא אם כן', 'אע"ג': 'אף על גב', 'אע"פ': 'אף על פי',
  'אעפ"כ': 'אף על פי כן', 'אפ"ה': 'אפילו הכי', 'ה"ה': 'הוא הדין', 'וה"ה': 'והוא הדין', 'כ"ש': 'כל שכן',
  'וכ"ש': 'וכל שכן', 'וכ"ת': 'וכי תימא', 'כ"ע': 'כולי עלמא', 'דכ"ע': 'דכולי עלמא', 'לכ"ע': 'לכולי עלמא',
  'מ"מ': 'מכל מקום', 'ומ"מ': 'ומכל מקום', 'י"ל': 'יש לומר', 'וי"ל': 'ויש לומר', 'יל"ע': 'יש לעיין',
  'צ"ע': 'צריך עיון', 'וצ"ע': 'וצריך עיון', 'צ"ל': 'צריך לומר', 'וצ"ל': 'וצריך לומר', 'צ"ב': 'צריך ביאור',
  'ע"כ': 'עד כאן', 'וע"כ': 'ועל כרחך', 'ע"ש': 'עיין שם', 'עיי"ש': 'עיין שם', 'ועיי"ש': 'ועיין שם',
  'ע"ע': 'עיין עוד', 'וע"ע': 'ועיין עוד', 'ד"ה': 'דיבור המתחיל', 'בד"ה': 'בדיבור המתחיל', 'ע"א': 'עמוד א',
  'ע"ב': 'עמוד ב', 'ר"ל': 'רוצה לומר', 'ור"ל': 'ורוצה לומר', 'פי"ז': 'פירוש זה', 'ק"ו': 'קל וחומר',
  'וק"ו': 'וקל וחומר', 'ג"ש': 'גזירה שווה', 'ב"ש': 'בית שמאי', 'ב"ה': 'בית הלל', 'ר"ח': 'ראש חודש',
  'ר"ה': 'ראש השנה', 'יו"ט': 'יום טוב', 'יוה"כ': 'יום הכיפורים', 'ער"ה': 'ערב ראש השנה', 'ע"ש': 'עיין שם',
  'ערב"ש': 'ערב שבת', 'ע"ז': 'עבודה זרה', 'ב"ד': 'בית דין', 'ביה"ד': 'בית הדין', 'ב"ח': 'בעל חי',
  'בע"ח': 'בעל חי', 'ביהמ"ק': 'בית המקדש', 'ביהכ"נ': 'בית הכנסת', 'ביהמ"ד': 'בית המדרש', 'ת"ח': 'תלמיד חכם',
  'ת"ת': 'תלמוד תורה', 'ד"ת': 'דברי תורה', 'ס"ת': 'ספר תורה', 'א"י': 'ארץ ישראל', 'בנ"י': 'בני ישראל',
  'ע"ה': 'עליו השלום', 'ז"ל': 'זכרונו לברכה', 'זצ"ל': 'זכר צדיק לברכה', 'זצוק"ל': 'זכר צדיק וקדוש לברכה',
  'זי"ע': 'זכותו יגן עלינו', 'שליט"א': 'שליטא', 'נ"ע': 'נשמתו עדן', 'הי"ד': 'השם יקום דמו', 'הקב"ה': 'הקדוש ברוך הוא',
  'הקדב"ה': 'הקדוש ברוך הוא', 'הקב"ה': 'הקדוש ברוך הוא', 'בעזה"י': 'בעזרת השם יתברך', 'בס"ד': 'בסייעתא דשמיא',
  'בע"ה': 'בעזרת השם', 'אי"ה': 'אם ירצה השם', 'בל"נ': 'בלי נדר', 'ח"ו': 'חס ושלום', 'ח"ח': 'חפץ חיים',
  'ש"ס': 'שס', 'ש"ע': 'שולחן ערוך', 'שו"ע': 'שולחן ערוך', 'או"ח': 'אורח חיים', 'יו"ד': 'יורה דעה',
  'אה"ע': 'אבן העזר', 'אבה"ע': 'אבן העזר', 'חו"מ': 'חושן משפט', 'סי"ק': 'סעיף קטן', 'ס"ק': 'סעיף קטן',
  'סק"א': 'סעיף קטן א', 'ס"ס': 'סוף סימן', 'ר"ס': 'ריש סימן', 'רס"י': 'ריש סימן', 'סו"ס': 'סוף סוף',
  'ה"ג': 'הכי גרסינן', 'ל"ג': 'לא גרסינן', 'מ"ד': 'מאן דאמר', 'למ"ד': 'למאן דאמר', 'כמ"ד': 'כמאן דאמר',
  'מד"א': 'מאי דכתיב', 'ת"ל': 'תלמוד לומר', 'ות"ל': 'ותלמוד לומר', 'שנא"': 'שנאמר', 'ר"א': 'רבי אליעזר',
  'ר"מ': 'רבי מאיר', 'ר"י': 'רבי יהודה', 'ר"ש': 'רבי שמעון', 'ר"ע': 'רבי עקיבא', 'ר"ג': 'רבן גמליאל',
  'ריב"ל': 'רבי יהושע בן לוי', 'רשב"ג': 'רבן שמעון בן גמליאל', 'רשב"י': 'רבי שמעון בר יוחאי', 'ריב"ז': 'רבן יוחנן בן זכאי',
  'ר"נ': 'רב נחמן', 'ר"פ': 'רב פפא', 'רה"ג': 'רב האי גאון', 'רי"ף': 'ריף', 'רא"ש': 'ראש',
  'בעה"ב': 'בעל הבית', 'בעה"ת': 'בעל התוספות', 'ב"ב': 'בבא בתרא', 'ב"מ': 'בבא מציעא', 'ב"ק': 'בבא קמא',
  'ע"ז': 'עבודה זרה', 'עה"ת': 'על התורה', 'עה"פ': 'על הפסוק', 'עפ"י': 'על פי', 'ע"פ': 'על פי', 'ע"י': 'על ידי',
  'ע"ג': 'על גבי', 'ע"ד': 'על דרך', 'ע"מ': 'על מנת', 'ע"כ': 'עד כאן', 'עכ"ל': 'עד כאן לשונו', 'עכ"פ': 'על כל פנים',
  'ועכ"פ': 'ועל כל פנים', 'עכצ"ל': 'על כרחך צריך לומר', 'משא"כ': 'מה שאין כן', 'ומשא"כ': 'ומה שאין כן',
  'וא"ת': 'ואם תאמר', 'וי"א': 'ויש אומרים', 'י"א': 'יש אומרים', 'י"מ': 'יש מפרשים', 'א"ש': 'אתי שפיר',
  'ל"ל': 'למה לי', 'דל"ל': 'דלמה לי', 'הל"ל': 'הוה ליה למימר', 'ה"א': 'הוה אמינא', 'א"ב': 'איכא בינייהו',
  'איב"ע': 'איבעית אימא', 'ואב"א': 'ואיבעית אימא', 'איב"א': 'איבעית אימא', 'מ"ה': 'מה הן', 'ת"ק': 'תנא קמא',
  'ד"א': 'דבר אחר', 'דא"כ': 'דאם כן', 'דא"ל': 'דאמר ליה', 'וא"ל': 'ואמר ליה', 'א"א': 'אי אפשר',
  'אל"כ': 'אם לא כן', 'וא"כ': 'ואם כן', 'וה"נ': 'והכי נמי', 'וה"ק': 'והכי קאמר', 'ה"ז': 'הרי זה', 'ה"ח': 'הרי חייב',
  'ח"א': 'חלק א', 'ח"ב': 'חלק ב', 'ח"ג': 'חלק ג', 'פ"א': 'פרק א', 'פ"ב': 'פרק ב', 'פ"ג': 'פרק ג',
  'מ"א': 'משנה א', 'מ"ב': 'משנה ב', 'ה"א': 'הלכה א', 'ה"ב': 'הלכה ב', 'וגו"': 'וגומר',
  'ל"ת': 'לא תעשה', 'מ"ע': 'מצות עשה', 'מצ"ע': 'מצות עשה', 'תמ"ה': 'תרי"ג מצוות', 'ג"כ': 'גם כן',
  'וג"כ': 'וגם כן', 'כ"כ': 'כל כך', 'וכ"כ': 'וכן כתב', 'כ"מ': 'כסף משנה', 'כמ"ש': 'כמו שכתב', 'וכמ"ש': 'וכמו שכתב',
  'כמש"כ': 'כמו שכתב', 'מש"כ': 'מה שכתב', 'ומש"כ': 'ומה שכתב', 'שכ"כ': 'שכן כתב', 'כ"ז': 'כל זה', 'כה"ג': 'כהאי גוונא',
  'בכה"ג': 'בכהאי גוונא', 'ואפ"ה': 'ואפילו הכי', 'ודו"ק': 'ודייק', 'וק"ל': 'וקל להבין', 'כנ"ל': 'כנזכר לעיל',
  'וכנ"ל': 'וכנזכר לעיל', 'נ"ל': 'נראה לי', 'ונ"ל': 'ונראה לי', 'נלע"ד': 'נראה לעניות דעתי', 'לפענ"ד': 'לפי עניות דעתי',
  'לענ"ד': 'לעניות דעתי', 'לפ"ז': 'לפי זה', 'ולפ"ז': 'ולפי זה', 'עפ"ז': 'על פי זה', 'ז"א': 'זאת אומרת',
  'ר"ת': 'ראשי תיבות', 'בפ"ע': 'בפני עצמו', 'בהמ"ז': 'ברכת המזון', 'ק"ש': 'קריאת שמע', 'ש"צ': 'שליח ציבור',
  'שו"ת': 'שות', 'י"ח': 'ידי חובה', 'יד"ח': 'ידי חובה', 'ר"ת': 'ראשי תיבות', 'ת"ב': 'תשעה באב',
  'חוה"מ': 'חול המועד', 'ע"ח': 'עץ חיים', 'ש"ך': 'שך', 'ט"ז': 'טז', 'מג"א': 'מגן אברהם', 'משנ"ב': 'משנה ברורה',
  'מ"ב': 'משנה ברורה', 'שעה"צ': 'שער הציון', 'בה"ל': 'ביאור הלכה', 'ביה"ל': 'ביאור הלכה', 'פמ"ג': 'פרי מגדים',
  'ב"י': 'בית יוסף', 'ד"מ': 'דרכי משה', 'ב"ח': 'בית חדש', 'כה"ח': 'כף החיים', 'אג"מ': 'אגרות משה',
  // with a final geresh
  "וכו'": 'וכולי', "וכדו'": 'וכדומה', "פי'": 'פירוש', "ופי'": 'ופירוש', "סי'": 'סימן', "בסי'": 'בסימן',
  "ובסי'": 'ובסימן', "עי'": 'עיין', "ועי'": 'ועיין', "ר'": 'רבי', "הר'": 'הרב', "פ'": 'פרק', "ס'": 'סעיף',
  "מס'": 'מסכת', "במס'": 'במסכת', "ה'": 'השם', "מ'": 'משנה', "תוס'": 'תוספות', "ותוס'": 'ותוספות', "שנא'": 'שנאמר',
  "גמ'": 'גמרא', "בגמ'": 'בגמרא', "מתני'": 'מתניתין', "במתני'": 'במתניתין', "ממתני'": 'ממתניתין', "ברייתא'": 'ברייתא',
  "וגו'": 'וגומר', "כו'": 'כולי', "הל'": 'הלכות', "ע'": 'עמוד', "הג'": 'הגאון', "הגר'": 'הגאון רבי', "דר'": 'דרבי',
  "ור'": 'ורבי', "לר'": 'לרבי', "כר'": 'כרבי', "אמ'": 'אמר', "ואמ'": 'ואמר', "פס'": 'פסוק', "מהר'": 'מהרב'
};

/** [plain] in [table] as is, or with one prefix letter (בתוס', הרמב"ם) before it. */
function lookup(table, plain) {
  const direct = table instanceof Set ? (table.has(plain) ? plain.replace(/["']/g, '') : null) : table[plain];
  if (direct) return direct;
  if (plain.length > 2 && 'ובלכמשהד'.includes(plain[0])) {
    const rest = plain.slice(1);
    const found = table instanceof Set ? (table.has(rest) ? rest.replace(/["']/g, '') : null) : table[rest];
    if (found) return plain[0] + found;
  }
  return null;
}

/** Names said as one word: only the quote marks are dropped. */
const NAMES = new Set(('רש"י רמב"ם רמב"ן רשב"א ריטב"א ר"ן רא"ש רי"ף ראב"ד רשב"ם רד"ק רלב"ג מהרש"א מהרש"ל מהר"ל מהרי"ל ' +
  'מהר"ם מהרי"ק מהרשד"ם רמ"א ש"ך ט"ז סמ"ע סמ"ג סמ"ק רא"ה רע"ב רשב"ץ ריב"ש רי"ד ראבי"ה אבודרה"ם שד"ל ' +
  'מלבי"ם חת"ס רעק"א נתיה"מ קצוה"ח רש"ש רי"ו ב"ח ט"ז שליט"א זצ"ל').split(' '));

/** The letters by name (vocalized: unvocalized וו, הא, פא are misread). */
const LETTER_NAMES = {
  'א': 'אָלֶף', 'ב': 'בֵּית', 'ג': 'גִּימֶל', 'ד': 'דָּלֶת', 'ה': 'הֵא', 'ו': 'וָו', 'ז': 'זַיִן', 'ח': 'חֵית',
  'ט': 'טֵית', 'י': 'יוֹד', 'כ': 'כַּף', 'ך': 'כַּף', 'ל': 'לָמֶד', 'מ': 'מֵם', 'ם': 'מֵם', 'נ': 'נוּן', 'ן': 'נוּן',
  'ס': 'סָמֶךְ', 'ע': 'עַיִן', 'פ': 'פֵּא', 'ף': 'פֵּא', 'צ': 'צָדִי', 'ץ': 'צָדִי', 'ק': 'קוֹף', 'ר': 'רֵישׁ',
  'ש': 'שִׁין', 'ת': 'תָּו'
};

// Words after which a short word of letters is a numeral (דף ב, פרק יד).
const NUMERAL_HEADS = /^(דף|פרק|סימן|סעיף|הלכה|משנה|אות|פסוק|שער|חלק|עמוד|שורה|סוגיא|פיסקא|ס"ק|סי'|פ'|ס'|ה'|מ'|ע')$/;
const GEMATRIA = /^[קרשת]?[יכלמנסעפצ]?[אבגדהוזחט]?$/;  // hundreds, tens, units, in order

/** Common words that look like numerals but are read as words after a head. */
const NOT_NUMERALS = new Set(['יד', 'יש', 'כן', 'לא', 'לב', 'מה', 'מי', 'סח', 'עד', 'על', 'עם', 'פה', 'צד', 'רב', 'רד', 'שב', 'שם', 'תם', 'נא', 'סג', 'קב', 'קו', 'רז', 'שד', 'תו']);

function spellLetters(word) {
  return word.split('').map(ch => LETTER_NAMES[ch] || ch).join(' ');
}

/** A numeral of letters; after a head word (דף יד) common words count too. */
function isNumeral(word, afterHead) {
  return word.length >= 1 && word.length <= 3 && GEMATRIA.test(word) && (afterHead || !NOT_NUMERALS.has(word));
}

/**
 * The words of [orig] that are said differently: abbreviations, names with
 * quote marks, and numerals (letters as numbers, said by letter name:
 * "דף ב" -> "דף בית", "פרק י"ד" -> "פרק יוד דלת", "ב:" after a tractate or
 * "דף" -> "בית עמוד בית"). Returns start -> { length, text }.
 */
function rewrites(orig) {
  const out = new Map();
  const WORD = /[א-ת][֑-ׇ]*(?:[א-ת"'״׳][֑-ׇ]*)*/g;
  let prev = '';
  let m;
  while ((m = WORD.exec(orig))) {
    const raw = m[0];
    const plain = raw.replace(/[֑-ׇ]/g, '').replace(/[״]/g, '"').replace(/[׳]/g, "'");
    const letters = plain.replace(/["']/g, '');
    const afterHead = NUMERAL_HEADS.test(prev);
    let text = null;
    if (/["']/.test(plain)) {
      const expansion = lookup(ABBREVIATIONS, plain);
      const name = expansion ? null : lookup(NAMES, plain);
      if (expansion) text = expansion;
      else if (name) text = name;
      else if (/^[א-ת]{1,4}"[א-ת]$/.test(plain)) text = spellLetters(letters);  // unknown: by letter
      else if (/^[א-ת]'$/.test(plain)) text = spellLetters(letters);
      else text = letters;  // a longer word with a geresh: the word itself
    } else if (afterHead && isNumeral(letters, true)) {
      text = spellLetters(letters);
    }
    // "ב." (after דף) or "ב:" (anywhere, a tractate name before it): a daf
    // with its side, when the mark ends the word and nothing follows it.
    const after = orig[m.index + raw.length];
    if ((text === null || afterHead) && !/["']/.test(plain) && (after === ':' || after === '.') &&
        isNumeral(letters, afterHead) && (afterHead || after === ':') && /\s/.test(orig[m.index - 1] || ' ')) {
      const next = orig[m.index + raw.length + 1];
      if (next === undefined || /\s/.test(next)) {
        text = spellLetters(letters) + (after === ':' ? ' עַמּוּד בֵּית' : ' עַמּוּד אָלֶף');
        out.set(m.index, { length: raw.length + 1, text });
        prev = plain;
        continue;
      }
    }
    if (text !== null && text !== raw) out.set(m.index, { length: raw.length, text });
    prev = plain;
  }
  return out;
}

/**
 * Text to hand the voice, plus map[i] = offset in the original text of spoken
 * char i (map[length] = original length). Removes teamim and meteg, which the
 * voice does not use and which can derail it; turns maqaf/paseq into spaces;
 * reads the Tetragrammaton as "השם"; says abbreviations and numerals in words
 * (rewrites).
 */
function toSpeech(orig) {
  let text = '';
  const map = [];
  const rewritten = rewrites(orig);
  let i = 0;
  while (i < orig.length) {
    const rw = rewritten.get(i);
    if (rw) {
      for (const ch of rw.text) { text += ch; map.push(i); }
      i += rw.length;
      continue;
    }
    DIVINE_NAME.lastIndex = i;
    const name = DIVINE_NAME.exec(orig);
    if (name) {
      for (const ch of 'השם') { text += ch; map.push(i); }
      i += name[0].length;
      continue;
    }
    const ch = orig[i];
    if (/[֑-ֽ֯]/.test(ch)) { i++; continue; }
    text += /[־׀]/.test(ch) ? ' ' : ch;
    map.push(i);
    i++;
  }
  map.push(orig.length);
  return { text, map };
}

/** First spoken offset whose original offset is >= [origOffset]. */
function toSpoken(map, origOffset) {
  let lo = 0, hi = map.length - 1;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (map[mid] < origOffset) lo = mid + 1; else hi = mid;
  }
  return lo;
}

/**
 * Split [from, text.length) into utterance chunks that end on a sentence
 * boundary where possible. Offsets are UTF-16, matching boundary charIndex.
 */
function chunksOf(text, from, to) {
  const out = [];
  let start = from;
  const end = to == null ? text.length : to;
  while (start < end) {
    let stop = Math.min(start + MAX_CHUNK, end);
    if (stop < end) {
      const slice = text.slice(start, stop);
      const m = slice.match(/^[\s\S]*[.:;?!׃]\s/);
      if (m && m[0].length > 40) {
        stop = start + m[0].length;
      } else {
        const sp = slice.lastIndexOf(' ');
        if (sp > 40) stop = start + sp + 1;
      }
    }
    const piece = text.slice(start, stop);
    if (piece.trim()) out.push({ base: start, text: piece });
    start = stop;
  }
  return out;
}

/** Last word whose start <= utf16 offset (binary search). */
function wordAt(offset) {
  const w = state.words;
  let lo = 0, hi = w.length - 1, ans = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    const s = w[mid].start && w[mid].start.utf16 != null ? w[mid].start.utf16 : 0;
    if (s <= offset) { ans = mid; lo = mid + 1; } else hi = mid - 1;
  }
  return ans;
}

async function highlightWord(index) {
  const w = state.words[index];
  if (!w) return;
  const range = w.sourceRange || w.renderedRange;
  if (!range) return;
  await call('reader.setHighlight', {
    highlightId: HIGHLIGHT_ID,
    bookId: state.bookId,
    sectionIndex: state.sectionIndex,
    range,
    style: { backgroundColor: state.highlightColor, borderRadius: 3, priority: 50 }
  });
}

async function clearHighlight() {
  await call('reader.clearHighlight', { highlightId: HIGHLIGHT_ID });
}

// --------------------------------------------------------------- speaking

function speakChunk(chunk, gen) {
  return new Promise(resolve => {
    const u = new SpeechSynthesisUtterance(chunk.text);
    u.voice = state.voice;
    u.lang = state.voice ? state.voice.lang : 'he-IL';
    u.rate = state.rate;
    u.onboundary = e => {
      if (gen !== state.gen || e.name === 'sentence' || !state.map) return;
      const spoken = Math.min(chunk.base + e.charIndex, state.map.length - 1);
      const i = wordAt(state.map[spoken]);
      if (i >= 0 && i !== state.wordPos) {
        state.wordPos = i;
        highlightWord(i);
      }
    };
    u.onend = () => resolve(true);
    u.onerror = e => resolve(e.error === 'interrupted' || e.error === 'canceled' ? false : true);
    speechSynthesis.speak(u);
  });
}

/** Is the reading tab still the active one? Avoid scrolling another book. */
async function activeBookIs(bookId) {
  const res = await call('reader.getCurrentRef');
  return res.success && res.data && res.data.currentBookId === bookId;
}

/**
 * Read from (bookId, sectionIndex, utf16) to the end of the book or until
 * stopped. Each await re-checks the generation so a stop wins immediately.
 */
async function readFrom(bookId, sectionIndex, utf16) {
  const gen = ++state.gen;
  speechSynthesis.cancel();
  state.playing = true;
  state.bookId = bookId;
  setToolbarPlaying(true);

  let section = sectionIndex;
  let offset = utf16 || 0;
  let empty = 0;
  let first = true;

  while (gen === state.gen) {
    const loaded = await loadSection(bookId, section);
    if (gen !== state.gen) return;
    if (loaded.error) {
      // Past the last line, or the book went away.
      if (!first) message('ההקראה הגיעה לסוף הספר.');
      else message('לא ניתן לקרוא את הקטע: ' + (loaded.error.message || ''));
      break;
    }
    state.sectionIndex = section;
    state.words = loaded.words;
    state.wordPos = Math.max(0, wordAt(offset));

    if (!loaded.text.trim() || !loaded.words.length) {
      if (++empty > MAX_EMPTY_SECTIONS) break;
      section++; offset = 0; first = false;
      continue;
    }
    empty = 0;

    if (!first && await activeBookIs(bookId)) {
      call('reader.scrollToSection', { sectionIndex: section });
    }

    const speech = toSpeech(loaded.text);
    state.map = speech.map;
    for (const chunk of chunksOf(speech.text, toSpoken(speech.map, offset))) {
      const ok = await speakChunk(chunk, gen);
      if (!ok || gen !== state.gen) return;
    }
    section++; offset = 0; first = false;
  }

  if (gen === state.gen) await stop(false);
}

async function stop(keepPosition) {
  state.gen++;
  if (window.speechSynthesis) speechSynthesis.cancel();
  if (keepPosition) await savePosition();
  else await call('storage.remove', { key: 'position' });
  state.playing = false;
  await clearHighlight();
  await setToolbarPlaying(false);
  call('plugin.backgroundDone');
}

/** Speak plain text with no word tracking (fallback for selections). */
function speakPlain(text) {
  const gen = ++state.gen;
  speechSynthesis.cancel();
  state.playing = true;
  state.bookId = null;
  state.words = [];
  state.map = null;
  setToolbarPlaying(true);
  const chunks = chunksOf(toSpeech(text).text, 0);
  (async () => {
    for (const c of chunks) {
      const ok = await speakChunk(c, gen);
      if (!ok || gen !== state.gen) return;
    }
    if (gen === state.gen) stop(false);
  })();
}

// --------------------------------------------------------------- commands

async function ensureVoice() {
  if (!window.speechSynthesis) {
    message('ההקראה אינה נתמכת במערכת זו.');
    return false;
  }
  state.voice = await waitForVoice();
  if (!state.voice) {
    message('לא נמצא קול עברי במחשב. יש להתקין אותו: הגדרות Windows ← זמן ושפה ← דיבור ← הוספת קולות ← עברית.');
    return false;
  }
  return true;
}

/** Toolbar primary click / shortcut: play, pause, or resume. */
async function toggle(ctx) {
  await settingsReady;
  if (state.playing) {
    await stop(true);
    return;
  }
  if (!(await ensureVoice())) return;
  if (ctx && ctx.currentType && ctx.currentType !== 'text') {
    message('הקראה זמינה בספרי טקסט בלבד (לא ב-PDF).');
    return;
  }
  const bookId = ctx && ctx.currentBookId;
  if (!bookId) {
    message('יש לפתוח ספר כדי להקריא.');
    return;
  }
  // Resume a paused position in the same book if it is still near the view.
  const saved = await call('storage.get', { key: 'position' });
  const pos = saved.success ? saved.data : null;
  const here = typeof ctx.currentIndex === 'number' ? ctx.currentIndex : 0;
  if (pos && pos.bookId === bookId && Math.abs(pos.sectionIndex - here) <= 3) {
    readFrom(bookId, pos.sectionIndex, pos.utf16);
  } else {
    readFrom(bookId, here, 0);
  }
}

async function readSelection(ctx) {
  await settingsReady;
  if (!(await ensureVoice())) return;
  const sel = ctx && ctx.selection;
  const sections = sel && Array.isArray(sel.sections) && sel.sections.length
    ? sel.sections
    : (sel && sel.renderedRange ? [sel] : null);

  // Precise path: read the selected range with word highlighting.
  if (sections && sections[0].renderedRange && sections[0].bookId) {
    readRanges(sections);
    return;
  }
  let text = ctx && (ctx.selectedText || (sel && sel.text));
  if (!text) {
    const res = await call('reader.getSelection');
    text = res.success && res.data ? res.data.text : '';
  }
  if (!text || !text.trim()) {
    message('כדי להקריא קטע: סמנו אותו, לחצו עליו לחיצה ימנית ובחרו "הקרא בקול".');
    return;
  }
  speakPlain(text);
}

/** Read exactly the selected ranges (one per paragraph), with highlighting. */
async function readRanges(sections) {
  const gen = ++state.gen;
  speechSynthesis.cancel();
  state.playing = true;
  setToolbarPlaying(true);
  for (const s of sections) {
    const loaded = await loadSection(s.bookId, s.sectionIndex);
    if (gen !== state.gen) return;
    if (loaded.error) continue;
    state.bookId = s.bookId;
    state.sectionIndex = s.sectionIndex;
    state.words = loaded.words;
    // Selection offsets are per layer; use the one matching the loaded text.
    const range = (loaded.layer === 'source' ? s.sourceRange : s.renderedRange) || s.renderedRange;
    const speech = toSpeech(loaded.text);
    state.map = speech.map;
    const from = toSpoken(speech.map, range.start && range.start.utf16 || 0);
    const to = range.end && range.end.utf16 != null ? toSpoken(speech.map, range.end.utf16) : undefined;
    for (const chunk of chunksOf(speech.text, from, to)) {
      const ok = await speakChunk(chunk, gen);
      if (!ok || gen !== state.gen) return;
    }
  }
  if (gen === state.gen) stop(false);
}

/**
 * Rebuilds the toolbar menu so that it offers every voice the service has
 * installed (a file dropped into the voices folder included). [status] is
 * the service's /status answer, fetched here when not given. Without the
 * service the menu is left as it is. Best effort: never throws.
 */
let shownVoices = null;
async function refreshVoiceMenu(status) {
  let st = status;
  if (!st) {
    try {
      st = await serviceStatus();
    } catch (_) {
      return false;
    }
  }
  const voices = menuVoices(st && st.voices, st && st.lightVoices);
  const key = voices.join('\n');
  if (key === shownVoices) return true;
  const res = await call('reader.updateToolbarItem', { id: TOOLBAR_ID, patch: { children: menuChildren(voices) } });
  if (res.success) shownVoices = key;
  return res.success;
}

async function setVoice(voice) {
  state.mp3Voice = voice;
  await call('storage.set', { key: 'mp3Voice', value: state.mp3Voice });
  message('הקול לשמירה כ-MP3: ' + voiceLabel(voice));
  let st = null;
  try {
    st = await serviceStatus();
  } catch (_) {
    // A recorded voice needs the service; the system voice needs it only when saving.
    if (voice) await offerServiceInstall();
    return;
  }
  refreshVoiceMenu(st);
  if (!voice) return;
  // A voice needs its model; fetch it now rather than at the first save, so
  // that the save itself is not held up by a long download.
  if (isLightVoice(voice)) {
    if (st && !(st.lightVoices || []).includes(voice)) await offerLightModel(st);
  } else if (st && st.voiceModel === false) {
    await offerVoiceModel(st);
  }
}

/** Deletes the light voice (and its diacritizer, when nothing else uses it). */
async function removeLightModel() {
  let st = null;
  try {
    st = await serviceStatus();
  } catch (_) {
    message('שירות הדיבור אינו פועל.');
    return;
  }
  if (!st || !st.lightModel) {
    message('הקול הקל אינו מותקן במחשב הזה.');
    return;
  }
  const ok = await call('ui.showConfirm', {
    title: 'הסרת הקול הקל',
    content: 'הקול הקל יימחק מהמחשב. אפשר להוריד אותו שוב בכל עת.\n\nלהסיר את הקול?'
  });
  if (!ok.success || !ok.data || !ok.data.confirmed) return;
  try {
    const lines = await serviceRequest('/remove-light-model', 'POST');
    const r = lines[lines.length - 1];
    const mb = r && r.freedBytes ? Math.round(r.freedBytes / 1048576) : 0;
    if (isLightVoice(state.mp3Voice)) {
      state.mp3Voice = '';
      await call('storage.set', { key: 'mp3Voice', value: '' });
    }
    message('הקול הקל הוסר' + (mb ? ' (פונו ' + mb + ' מגה-בייט)' : '') + '.' +
      (isLightVoice(state.mp3Voice) ? '' : ' הקול לשמירה כ-MP3 הוא עכשיו ' + voiceLabel(state.mp3Voice) + '.'));
    refreshVoiceMenu();
  } catch (e) {
    message(e && e.code === 'busy' ? 'יש להמתין לסיום ההורדה או השמירה, ואז לנסות שוב.' : 'ההסרה נכשלה: ' + String((e && e.message) || e));
  }
}

let downloadingLight = false;

/** Asks, then has the service download the light voice, showing progress. */
async function offerLightModel(status) {
  if (downloadingLight) {
    message('הורדת הקול הקל כבר מתבצעת.');
    return;
  }
  let st = status;
  if (!st) {
    try {
      st = await serviceStatus();
    } catch (_) {
      await offerServiceInstall();
      return;
    }
  }
  if (st && st.lightPublished === false) {
    message('הקול הקל עדיין לא פורסם. בינתיים אפשר להשתמש בקול המוקלט.');
    return;
  }
  if (st && st.lightModelSize === undefined) {
    message('שירות הדיבור המותקן ישן מדי לקול הקל. יש לעדכן את התוסף "חיפוש בדיבור" ואת השירות שלו.');
    return;
  }
  const mb = st && st.lightModelSize ? Math.round(st.lightModelSize / 1048576) : 0;
  const ok = await call('ui.showConfirm', {
    title: 'הורדת הקול הקל',
    content: 'הקול הקל דורש הורדה חד-פעמית של כ-' + mb + ' מגה-בייט' +
      (st && st.nakdan === false ? ' (רובה המנקד, שמשותף לקול המוקלט)' : '') +
      '. אחרי ההורדה הוא פועל במחשב עצמו, בלי אינטרנט, והכנת MP3 אורכת כדקה לשעת הקראה.\n\nלהוריד עכשיו?'
  });
  if (!ok.success || !ok.data || !ok.data.confirmed) return;
  downloadingLight = true;
  try {
    await serviceRequest('/install-light-model', 'POST');
    let job = null;
    let lastShown = -1;
    showExportProgress(0);
    while (true) {
      await serviceRequest('/light-model-wait', 'POST', null, obj => {
        job = obj;
        if (obj.state !== 'downloading') return;
        const pct = obj.total ? Math.floor(100 * obj.received / obj.total) : 0;
        showExportProgress(pct);
        if (pct - lastShown >= 10) {
          lastShown = pct;
          message('מוריד את הקול הקל… ' + pct + '%');
        }
      });
      if (!job || job.state === 'done' || job.state === 'error' || job.state === 'idle') break;
    }
    showExportProgress(null);
    if (job && job.state === 'done') {
      message('הקול הקל הותקן. אפשר לשמור כ-MP3.');
      refreshVoiceMenu();
    } else {
      message('הורדת הקול הקל נכשלה: ' + ((job && job.error) || 'שגיאה לא ידועה'));
    }
  } catch (e) {
    showExportProgress(null);
    message(e && e.code === 'not_published' ? 'הקול הקל עדיין לא פורסם.' : 'הורדת הקול הקל נכשלה: ' + String((e && e.message) || e));
  } finally {
    downloadingLight = false;
  }
}

/** Deletes the recorded voice's model pack (about 900MB) from the computer. */
async function removeVoiceModel() {
  let st = null;
  try {
    st = await serviceStatus();
  } catch (_) {
    message('שירות הדיבור אינו פועל. להסרה מלאה: הגדרות Windows ← אפליקציות ← Otzaria Voice ← הסרה.');
    return;
  }
  if (!st || !st.voiceModel) {
    message('הקול המוקלט אינו מותקן במחשב הזה.');
    return;
  }
  const ok = await call('ui.showConfirm', {
    title: 'הסרת הקול המוקלט',
    content: 'הקול המוקלט יימחק מהמחשב ויפנה כ-900 מגה-בייט. אפשר להוריד אותו שוב בכל עת.\n\n' +
      'להסרה של שירות הדיבור כולו: הגדרות Windows ← אפליקציות ← Otzaria Voice ← הסרה.\n\nלהסיר את הקול?'
  });
  if (!ok.success || !ok.data || !ok.data.confirmed) return;
  try {
    const lines = await serviceRequest('/remove-voice-model', 'POST');
    const r = lines[lines.length - 1];
    const mb = r && r.freedBytes ? Math.round(r.freedBytes / 1048576) : 0;
    if (state.mp3Voice) {
      state.mp3Voice = '';
      await call('storage.set', { key: 'mp3Voice', value: '' });
    }
    message('הקול המוקלט הוסר' + (mb ? ' (פונו ' + mb + ' מגה-בייט)' : '') + '. הקול לשמירה כ-MP3 הוא עכשיו אסף.');
    refreshVoiceMenu();  // the pack's voices are gone
  } catch (e) {
    message(e && e.code === 'busy' ? 'יש להמתין לסיום ההורדה או השמירה, ואז לנסות שוב.' : 'ההסרה נכשלה: ' + String((e && e.message) || e));
  }
}

let downloadingModel = false;

/** Asks, then has the service download the model pack, showing progress. */
async function offerVoiceModel(status) {
  if (downloadingModel) {
    message('הורדת הקול כבר מתבצעת.');
    return;
  }
  let st = status;
  if (!st) {
    try {
      st = await serviceStatus();
    } catch (_) {
      await offerServiceInstall();
      return;
    }
  }
  const mb = st && st.voiceModelSize ? Math.round(st.voiceModelSize / 1048576) : 900;
  const ok = await call('ui.showConfirm', {
    title: 'הורדת הקול המוקלט',
    content: 'הקול המוקלט דורש הורדה חד-פעמית של כ-' + mb + ' מגה-בייט. אחרי ההורדה הוא פועל במחשב עצמו, בלי אינטרנט.\n\nלהוריד עכשיו?'
  });
  if (!ok.success || !ok.data || !ok.data.confirmed) return;
  downloadingModel = true;
  try {
    await serviceRequest('/install-voice-model', 'POST');
    let job = null;
    let lastShown = -1;
    showExportProgress(0);
    while (true) {
      await serviceRequest('/voice-model-wait', 'POST', null, obj => {
        job = obj;
        if (obj.state !== 'downloading') return;
        const pct = obj.total ? Math.floor(100 * obj.received / obj.total) : 0;
        showExportProgress(pct);
        if (pct - lastShown >= 10) {
          lastShown = pct;
          message('מוריד את הקול המוקלט… ' + pct + '%');
        }
      });
      if (!job || !['downloading', 'verifying', 'extracting'].includes(job.state)) break;
      if (job.state !== 'downloading') message(job.state === 'verifying' ? 'בודק שהקובץ שירד תקין…' : 'מתקין את הקול…');
    }
    await endExportProgress();
    if (job && job.state === 'done') {
      call('ui.showSuccess', { message: 'הקול המוקלט מותקן. אפשר לשמור כ-MP3.' });
      refreshVoiceMenu();  // the pack's voices are installed now
    }
    else message('הורדת הקול לא הצליחה' + (job && job.error ? ': ' + job.error : '.'));
  } catch (e) {
    await endExportProgress();
    message('הורדת הקול נכשלה: ' + String((e && e.message) || e));
  } finally {
    downloadingModel = false;
    if (!exporting) call('plugin.backgroundDone');
  }
}

async function setRate(id) {
  state.rate = RATES[id];
  await call('storage.set', { key: 'rate', value: state.rate });
  message('מהירות ההקראה: ' + RATE_LABELS[id]);
  // Apply immediately: restart from the current word.
  if (state.playing && state.bookId) {
    const w = state.words[state.wordPos];
    readFrom(state.bookId, state.sectionIndex, w && w.start ? w.start.utf16 : 0);
  }
}

// ------------------------------------------------------------ save to MP3
//
// The WebView's speechSynthesis plays to the speakers only, so a file is made
// by the local voice service (otzaria-voice, installed with "חיפוש בדיבור"):
// it reads the text with the same Windows voice and encodes an MP3. The
// service works in the background; we poll it, because one request through
// network.fetchStream may not run past 120 seconds.

const SERVICE = 'http://127.0.0.1:47811';
const SERVICE_HEADERS = { 'X-Otzaria-Voice': '1', 'Content-Type': 'application/json' };
const MAX_EXPORT_SECTIONS = 3000;
const MAX_EXPORT_CHARS = 1500000;   // about 3 hours of speech
let exporting = false;

/** One request to the service; returns the parsed JSON lines of the answer. */
/** The service's /status answer; throws when the service is not running. */
async function serviceStatus() {
  const lines = await serviceRequest('/status', 'GET');
  return lines[lines.length - 1];
}

async function serviceRequest(path, method, body, onLine) {
  const stream = Otzaria.call('network.fetchStream', {
    url: SERVICE + path,
    method,
    headers: SERVICE_HEADERS,
    body: body ? JSON.stringify(body) : '',
    timeoutMs: 120000
  });
  const lines = [];
  let pending = '';
  let status = 200;
  let errorBody = '';
  const take = line => {
    if (!line.trim()) return;
    const obj = JSON.parse(line);
    if (onLine) onLine(obj); else lines.push(obj);
  };
  for await (const chunk of stream) {
    if (chunk.type === 'response') { status = chunk.status; continue; }
    if (status !== 200) { errorBody += chunk.body; continue; }
    pending += chunk.body;
    const parts = pending.split('\n');
    pending = parts.pop() || '';
    parts.forEach(take);
  }
  if (status !== 200) {
    let err = {};
    try { err = JSON.parse(errorBody); } catch (_) { /* not JSON */ }
    const e = new Error(err.message || 'HTTP ' + status);
    e.code = err.error || 'http_' + status;
    throw e;
  }
  take(pending);
  return lines;
}

/** The headings of the book as a flat list of { index, level, text }. */
function flattenToc(entries, out) {
  (entries || []).forEach(e => {
    if (e && typeof e.index === 'number') out.push({ index: e.index, level: e.level || 1, text: e.text || '' });
    if (e && Array.isArray(e.children)) flattenToc(e.children, out);
  });
  return out;
}

/**
 * The sections of the heading the reader is in: from that heading up to the
 * next heading of the same or a higher level. Without headings: from here on.
 */
async function chapterBounds(bookId, here) {
  const res = await call('library.getBookToc', { bookId });
  const toc = res.success ? flattenToc(res.data, []).sort((a, b) => a.index - b.index) : [];
  let head = null;
  toc.forEach(e => { if (e.index <= here) head = e; });
  if (!head) return { from: here, to: here + 200, title: '' };
  const next = toc.find(e => e.index > head.index && e.level <= head.level);
  return { from: head.index, to: next ? next.index : head.index + MAX_EXPORT_SECTIONS, title: head.text };
}

/** The text of sections [from, to) as the voice should read it. */
async function collectText(bookId, from, to) {
  const parts = [];
  let chars = 0;
  for (let i = from; i < to && i - from < MAX_EXPORT_SECTIONS; i++) {
    const loaded = await loadSection(bookId, i);
    if (loaded.error) break;  // past the end of the book
    const text = toSpeech(loaded.text).text.trim();
    if (!text) continue;
    parts.push(text);
    chars += text.length;
    if (chars > MAX_EXPORT_CHARS) break;
  }
  return parts.join('\n');
}

/** The selected text, vocalized when the book has nikud. */
async function selectionText(ctx) {
  const sel = ctx && ctx.selection;
  const sections = sel && Array.isArray(sel.sections) && sel.sections.length ? sel.sections : null;
  if (sections && sections[0].renderedRange && sections[0].bookId) {
    const parts = [];
    for (const s of sections) {
      const loaded = await loadSection(s.bookId, s.sectionIndex);
      if (loaded.error) continue;
      const range = (loaded.layer === 'source' ? s.sourceRange : s.renderedRange) || s.renderedRange;
      const from = range.start && range.start.utf16 || 0;
      const to = range.end && range.end.utf16 != null ? range.end.utf16 : loaded.text.length;
      parts.push(toSpeech(loaded.text.slice(from, to)).text.trim());
    }
    return parts.filter(Boolean).join('\n');
  }
  let text = ctx && (ctx.selectedText || (sel && sel.text));
  if (!text) {
    const res = await call('reader.getSelection');
    text = res.success && res.data ? res.data.text : '';
  }
  return text ? toSpeech(text).text.trim() : '';
}

function fileNameOf(text) {
  return (text || 'הקראה').replace(/[\\/:*?"<>|]+/g, ' ').replace(/\s+/g, ' ').trim().slice(0, 80) || 'הקראה';
}

const SERVICE_INSTALLER = "https://github.com/yakov100/otzaria-voice/releases/latest/download/otzaria-voice-windows-x64-without-models.exe";
const SERVICE_PAGE = 'https://github.com/yakov100/otzaria-voice/releases/latest';

/**
 * The service is not running: offer its installer. network.download puts it
 * in the Downloads folder (once Otzaria's allowlist names the repository);
 * until then, or if that fails, the release page opens in the browser.
 */
async function offerServiceInstall() {
  const ok = await call('ui.showConfirm', {
    title: 'שירות הדיבור אינו מותקן',
    content: 'שמירה כ-MP3 נעשית בשירות הדיבור המקומי, תוכנה קטנה שמותקנת פעם אחת ועולה מעצמה עם Windows. ' +
      'ההקראה והקבצים נעשים במחשב עצמו, בלי אינטרנט.\n\nלהוריד את תוכנת ההתקנה עכשיו?'
  });
  if (!ok.success || !ok.data || !ok.data.confirmed) return;
  const dl = await call('network.download', { url: SERVICE_INSTALLER });
  if (dl.success && dl.data && dl.data.path) {
    message('תוכנת ההתקנה ירדה: ' + dl.data.path + '. יש להפעיל אותה פעם אחת, ואז לנסות שוב.');
    return;
  }
  const opened = await call('app.openUrl', { url: SERVICE_PAGE });
  if (!opened.success) message('לא ניתן לפתוח את דף ההורדה: ' + SERVICE_PAGE);
  else message('נפתח דף ההורדה. יש להוריד את תוכנת ההתקנה ל-Windows, להפעיל אותה פעם אחת, ואז לנסות שוב.');
}

function serviceProblem(e) {
  const code = e && e.code;
  if (code === 'unsupported') return 'שמירה כ-MP3 זמינה כרגע רק ב-Windows.';
  const msg = e && e.message;
  if (msg === 'unknown_voice') return 'הקול שנבחר אינו מותקן. אפשר לבחור קול אחר בתפריט ההקראה.';
  if (msg === 'nakdan_missing') return 'הקול הקל צריך את המנקד, שאינו מותקן. יש להסיר את הקול הקל ולהוריד אותו שוב.';
  if (code === 'no_hebrew_voice' || (e && e.message === 'no_hebrew_voice')) {
    return 'לא נמצא קול עברי במחשב. יש להתקין אותו: הגדרות Windows ← זמן ושפה ← דיבור ← הוספת קולות ← עברית.';
  }
  if (code === 'not_found' || code === 'http_404') {
    return 'שירות הדיבור המותקן ישן מדי לשמירה כ-MP3. יש לעדכן את התוסף "חיפוש בדיבור" ואת השירות שלו.';
  }
  return null;
}

/**
 * The folder MP3s are saved in: chosen once (ui.pickFolder) and remembered.
 * The service writes the file there itself, so nothing large passes through
 * this engine and the file is saved even if Otzaria shuts the engine down
 * before the (long) job ends.
 */
async function mp3Folder(askAgain) {
  if (!askAgain) {
    const res = await call('storage.get', { key: 'mp3Folder' });
    if (res.success && typeof res.data === 'string' && res.data) return res.data;
  }
  const picked = await call('ui.pickFolder', { title: 'בחירת תיקייה לשמירת קובצי ההקראה' });
  if (!picked.success) {
    message('לא ניתן לשמור בתיקייה הזאת. מומלץ ליצור תיקייה חדשה, למשל בתוך "מסמכים".');
    return null;
  }
  const path = picked.data && picked.data.path;
  if (!path) return null;
  await call('storage.set', { key: 'mp3Folder', value: path });
  return path;
}

async function chooseFolder() {
  const path = await mp3Folder(true);
  if (path) message('קובצי ה-MP3 יישמרו בתיקייה: ' + path);
}

/** Reads [text] into an MP3 through the service, saved into the chosen folder. */
async function exportMp3(text, name) {
  if (exporting) {
    message('שמירה כ-MP3 כבר מתבצעת. יש להמתין לסיומה.');
    return;
  }
  if (!text.trim()) {
    message('אין טקסט לשמירה.');
    return;
  }
  exporting = true;
  try {
    const folder = await mp3Folder(false);
    if (!folder) return;
    try {
      await serviceRequest('/speak', 'POST', {
        text, rate: state.rate, voice: state.mp3Voice, saveDir: folder, saveName: fileNameOf(name)
      });
    } catch (e) {
      if (e && e.code === 'bad_folder') {
        await call('storage.remove', { key: 'mp3Folder' });
        message('תיקיית השמירה כבר לא קיימת. יש לבחור תיקייה חדשה ולנסות שוב.');
        return;
      }
      if (e && e.message === 'voice_model_missing') { offerVoiceModel(); return; }
      if (e && e.message === 'light_model_missing') { offerLightModel(); return; }
      const known = serviceProblem(e);
      if (known) { message(known); return; }
      await offerServiceInstall();
      return;
    }
    await call('storage.set', { key: 'pendingExport', value: { name } });
    message(isLightVoice(state.mp3Voice)
      ? 'מכין קובץ MP3 בקול ' + voiceLabel(state.mp3Voice) + '. זה מהיר: כדקה לשעת הקראה. הקובץ יישמר בתיקייה: ' + folder
      : state.mp3Voice
        ? 'מכין קובץ MP3 בקול המוקלט. זה לוקח בערך כאורך ההקראה. הקובץ יישמר בתיקייה: ' + folder
        : 'מכין קובץ MP3. הוא יישמר בתיקייה: ' + folder);
    await finishExport();
  } catch (e) {
    message('שמירה כ-MP3 נכשלה: ' + String((e && e.message) || e));
  } finally {
    exporting = false;
    call('plugin.backgroundDone');
  }
}

/**
 * Progress where it stays visible: plugins have no progress bar, so the
 * toolbar button becomes an hourglass that fills, with the percentage in its
 * title. A message lasts only a few seconds and is easily missed.
 */
let shownProgress = -1;
async function showExportProgress(pct) {
  if (pct === shownProgress) return;
  shownProgress = pct;
  const icon = pct < 34 ? 'fluent:hourglass_one_quarter_24_regular'
    : pct < 67 ? 'fluent:hourglass_half_24_regular'
      : 'fluent:hourglass_three_quarter_24_regular';
  await call('reader.updateToolbarItem', {
    id: TOOLBAR_ID,
    patch: { icon, title: 'מכין קובץ MP3… ' + pct + '%' }
  });
}

async function endExportProgress() {
  shownProgress = -1;
  await setToolbarPlaying(state.playing);
}

/**
 * Waits for the service's job with one open request at a time (/speak-wait,
 * up to 100 seconds each), then reports where the service saved the file.
 */
async function finishExport() {
  let st = null;
  let lastShown = 0;
  showExportProgress(0);
  try {
    while (true) {
      await serviceRequest('/speak-wait', 'POST', null, obj => {
        st = obj;
        if (obj.state !== 'working') return;
        const pct = obj.total ? Math.floor(100 * obj.done / obj.total) : 0;
        showExportProgress(pct);
        if (pct - lastShown >= 25) {
          lastShown = pct;
          message('מכין קובץ MP3… ' + pct + '%');
        }
      });
      if (!st || st.state !== 'working') break;
    }
  } finally {
    await endExportProgress();
  }
  await call('storage.remove', { key: 'pendingExport' });
  if (st && st.state === 'done' && st.savedPath) {
    const minutes = Math.round(st.seconds / 60);
    const text = 'הקובץ נשמר: ' + st.savedPath + (minutes ? ' (כ-' + minutes + ' דקות)' : '');
    call('ui.showSuccess', { message: text });
    return;
  }
  if (st && st.state === 'cancelled') return;
  const known = st && st.error && serviceProblem({ message: st.error });
  message(known || 'יצירת הקובץ נכשלה' + (st && st.error ? ': ' + st.error : '.'));
}

/** Stops the service's MP3 job; the waiting finishExport then ends quietly. */
async function cancelExport() {
  let st = null;
  try {
    const lines = await serviceRequest('/speak-status', 'GET');
    st = lines[lines.length - 1];
  } catch (_) {
    message('שירות הדיבור אינו פועל, כך שאין שמירה לבטל.');
    return;
  }
  if (!st || st.state !== 'working') {
    message('אין שמירת MP3 פעילה.');
    return;
  }
  await serviceRequest('/speak-cancel', 'POST');
  await call('storage.remove', { key: 'pendingExport' });
  await endExportProgress();
  message('שמירת ה-MP3 בוטלה.');
  if (!exporting) call('plugin.backgroundDone');
}

/** On boot: a job this plugin started before its engine was shut down. */
async function resumePendingExport() {
  const res = await call('storage.get', { key: 'pendingExport' });
  if (!(res.success && res.data) || exporting) return;
  let st = null;
  try {
    const lines = await serviceRequest('/speak-status', 'GET');
    st = lines[lines.length - 1];
  } catch (_) {
    return;  // the service is not running; try again on the next boot
  }
  if (!st || (st.state !== 'working' && st.state !== 'done')) {
    await call('storage.remove', { key: 'pendingExport' });
    return;
  }
  exporting = true;
  try {
    await finishExport();
  } catch (e) {
    message('שמירה כ-MP3 נכשלה: ' + String((e && e.message) || e));
  } finally {
    exporting = false;
    call('plugin.backgroundDone');
  }
}

async function saveChapter(ctx) {
  await settingsReady;
  if (ctx && ctx.currentType && ctx.currentType !== 'text') {
    message('שמירה כ-MP3 זמינה בספרי טקסט בלבד (לא ב-PDF).');
    return;
  }
  const bookId = ctx && ctx.currentBookId;
  if (!bookId) {
    message('יש לפתוח ספר כדי לשמור.');
    return;
  }
  const here = typeof ctx.currentIndex === 'number' ? ctx.currentIndex : 0;
  message('אוסף את הטקסט של הפרק…');
  const bounds = await chapterBounds(bookId, here);
  const text = await collectText(bookId, bounds.from, bounds.to);
  const book = ctx.currentBook || bookId;
  exportMp3(text, bounds.title ? book + ' - ' + bounds.title : book);
}

async function saveSelection(ctx) {
  await settingsReady;
  const text = await selectionText(ctx);
  if (!text) {
    message('כדי לשמור קטע: סמנו אותו, לחצו עליו לחיצה ימנית ובחרו "שמור כ-MP3".');
    return;
  }
  const book = (ctx && (ctx.currentBook || ctx.currentBookId)) || '';
  exportMp3(text, book ? book + ' - קטע' : 'הקראה');
}

// ----------------------------------------------------------------- events

let settingsReady = Promise.resolve();

Otzaria.on('plugin.boot', payload => {
  state.highlightColor = themeHighlight(payload && payload.theme);
  settingsReady = loadSettings();
  settingsReady.then(resumePendingExport);
  refreshVoiceMenu();  // offer every installed voice, not only the manifest's
});

Otzaria.on('theme.changed', theme => {
  state.highlightColor = themeHighlight(theme);
});

Otzaria.on('reader.toolbar_item_clicked', async data => {
  const id = data && data.itemId;
  if (id === TOOLBAR_ID || id === 'play') return toggle(data);
  if (id === 'read-selection') return readSelection(data);
  if (id === 'stop') return stop(false);
  if (id === 'save-chapter') return saveChapter(data);
  if (id === 'save-selection') return saveSelection(data);
  if (RATES[id]) return setRate(id);
  const voice = voiceOfItem(id);
  if (voice !== null) return setVoice(voice);
  if (id === 'choose-folder') return chooseFolder();
  if (id === 'cancel-export') return cancelExport();
  if (id === 'remove-voice-model') return removeVoiceModel();
  if (id === 'remove-light-model') return removeLightModel();
});

Otzaria.on('reader.context_menu_item_clicked', data => {
  if (data && data.itemId === 'tts-read-selection') readSelection(data);
  if (data && data.itemId === 'tts-save-selection') saveSelection(data);
});

Otzaria.on('app.command', async data => {
  if (!data) return;
  if (data.command === 'toggle') {
    const res = await call('reader.getCurrentRef');
    toggle(res.success ? res.data : null);
  } else if (data.command === 'stop') {
    stop(false);
  }
});
