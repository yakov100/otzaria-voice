// ----------------------------------------------------------------- feedback
//
// The plugin has no page: everything the user sees is Otzaria's own message
// strip (one at a time; a new message replaces the previous one) and its
// confirmation dialog. Messages are plain text, never HTML.

// Kept once the service has answered on this computer. A service that does not answer is then taken
// as installed but not running, and gets the small installer; without it, as not installed. Plugins
// before 2.3.4 kept the other keys only after the service answered (hotkeyTipShown: mid-October
// plugins, when the service offered its key combination), so they count as well. Otzaria keeps a
// plugin's storage across updates and clears it when the plugin is removed.
const SERVICE_SEEN_KEY = 'serviceSeen';
const SERVICE_SEEN_SIGNS = [SERVICE_SEEN_KEY, 'serviceUpdateDeclined', 'packUpdateDeclined', 'hotkeyTipShown'];
// The newest voice service this plugin knows of (kVersion in service/src/common/service.cpp). It
// keys the user's "not now" (one per version), and does NOT decide who is asked: that is
// SERVICE_UPDATE_BELOW(_WINDOWS). A plugin for a newer service raises both, and adds its news.
const LATEST_SERVICE = '2.5.5';
// Only a service older than these is offered the update. On Windows 2.5.2 fixed "cannot load
// ...onnxruntime.dll" (Visual C++ next to otzaria.exe; Windows 11 on ARM); elsewhere the last change
// users notice is 2.5.1 (final beam 48). 2.5.3 added only the models license and a fixed link, so
// a 2.5.2 is not asked. 2.5.4 brought voice pack 5, which the service offers itself; 2.5.5 added
// only what the Otzaria plugin store needs (it answers the store's check, and checks Host).
const SERVICE_UPDATE_BELOW_WINDOWS = '2.5.2';
const SERVICE_UPDATE_BELOW = '2.5.1';
const PACK_BUSY = ['downloading', 'verifying', 'extracting'];

function say(message) {
  return Otzaria.call('ui.showMessage', { message });
}

function sayDone(message) {
  return Otzaria.call('ui.showSuccess', { message });
}

function sayError(message) {
  return Otzaria.call('ui.showError', { message });
}

async function ask(title, content) {
  const res = await Otzaria.call('ui.showConfirm', { title, content });
  return !!(res && res.success && res.data && res.data.confirmed);
}

function megabytes(bytes) {
  return Math.round((bytes || 0) / 1048576);
}

/** Live words while the user speaks; at most a few times a second. */
const caption = (() => {
  let last = 0;
  let timer = null;
  let text = '';
  function flush() {
    timer = null;
    last = Date.now();
    say(text);
  }
  return {
    show(next) {
      text = next;
      if (timer) return;
      const wait = Math.max(0, 600 - (Date.now() - last));
      timer = setTimeout(flush, wait);
    },
    cancel() {
      if (timer) clearTimeout(timer);
      timer = null;
    },
  };
})();

let serviceSeen = false;

/** The service answered (serviceStatus); remembered once per engine. */
function noteServiceSeen() {
  if (serviceSeen) return;
  serviceSeen = true;
  try {
    Promise.resolve(Otzaria.call('storage.set', { key: SERVICE_SEEN_KEY, value: String(Date.now()) })).catch(() => {});
  } catch (_) {}
}

/** True when the service has answered on this computer before (see SERVICE_SEEN_KEY). */
async function serviceSeenBefore() {
  if (serviceSeen) return true;
  for (const key of SERVICE_SEEN_SIGNS) {
    try {
      const saved = await Otzaria.call('storage.get', { key });
      if (saved && saved.success && saved.data) return true;
    } catch (_) {}
  }
  return false;
}

/** Where the recognition runs, and what to do when it is installed but does not answer. */
function serviceWhere(platform) {
  return platform === 'android'
    ? 'הזיהוי נעשה באפליקציה קטנה בשם «שירות הדיבור של אוצריא», בלי אינטרנט.\n' +
      'אם היא מותקנת — פתחו אותה פעם אחת ולחצו על הכפתור שבה.\n'
    : 'הזיהוי נעשה בתוכנה שרצה ברקע במחשב, בלי אינטרנט.\n' +
      'אם היא מותקנת — הפעילו מחדש את המחשב, והיא תעלה מעצמה.\n';
}

/**
 * The service is not answering: not installed, or not started. How the installer reaches the user
 * (getInstaller and the texts) is the edition's: install_github.js or install_store.js.
 */
async function explainNoService(platform) {
  if (installerDownload) {
    say(installerBusyText());
    return;
  }
  const full = await wantsFullInstaller(platform);
  const saved = full ? await recentFullInstaller() : '';
  const { title, content } = noServiceAsk(platform, full, saved);
  if (!(await ask(title, content))) return;
  // Not awaited: the full installer takes minutes, and the toolbar button or the search field must
  // not stay busy meanwhile. Otzaria keeps the background engine alive while a call is running
  // (isBusy in plugin_lazy_activation_service.dart).
  getInstaller(platform, full).catch((e) => console.warn('voice-search: installer', String((e && e.message) || e)));
}

// ------------------------------------------------------------ voice pack

let packWatch = null;

/**
 * The service downloads the pack itself (a request through Otzaria is capped
 * at two minutes; the download may take longer); the plugin reports progress.
 */
function watchPack() {
  if (packWatch) return;
  let shown = -1;
  packWatch = setInterval(async () => {
    let st;
    try {
      st = await serviceStatus();
    } catch (_) {
      clearInterval(packWatch);
      packWatch = null;
      sayError('שירות הדיבור הפסיק לפעול באמצע ההורדה');
      return;
    }
    const job = st.packJob || {};
    if (PACK_BUSY.includes(job.state)) {
      const pct = job.total ? Math.floor((job.received / job.total) * 100) : 0;
      const step = job.state === 'downloading' ? Math.floor(pct / 10) : 100 + PACK_BUSY.indexOf(job.state);
      if (step !== shown) {
        shown = step;
        say(
          job.state === 'downloading'
            ? 'מוריד את חבילת הדיבור… ' + pct + '% (' + megabytes(job.received) + ' מתוך ' +
                megabytes(job.total) + ' מגה-בייט). אפשר להמשיך לעבוד.'
            : job.state === 'verifying'
              ? 'בודק שחבילת הדיבור שהורדה תקינה…'
              : 'מתקין את חבילת הדיבור…',
        );
      }
      return;
    }
    clearInterval(packWatch);
    packWatch = null;
    if (job.state === 'error') sayError('הורדת חבילת הדיבור לא הצליחה: ' + job.error);
    else if (st.pack) sayDone('חבילת הדיבור הותקנה. לחצו שוב על המיקרופון ודברו');
  }, 2000);
}

/** Asks the service to download the pack and reports its progress. True when it started. */
async function startPackDownload() {
  try {
    await serviceCall('/install-pack', 'POST');
    say('מוריד את חבילת הדיבור…');
    watchPack();
    return true;
  } catch (e) {
    sayError('הורדת חבילת הדיבור לא התחילה: ' + e.message);
    return false;
  }
}

async function offerPack(st) {
  if (PACK_BUSY.includes(st.packJob && st.packJob.state)) {
    say('חבילת הדיבור עדיין יורדת; נודיע כשהיא מוכנה');
    watchPack();
    return;
  }
  const ok = await ask(
    'חסרה חבילת הדיבור העברית',
    'כדי לזהות עברית בלי אינטרנט צריך להוריד פעם אחת חבילת דיבור של כ-' +
      megabytes(st.packSize) + ' מגה-בייט.\n' +
      'אחרי ההורדה הזיהוי פועל במחשב עצמו, וההקלטות אינן נשלחות לשום מקום.\n' +
      'להוריד עכשיו?',
  );
  if (!ok) return;
  await startPackDownload();
}

const PACK_UPDATE_ASK_AGAIN_MS = 14 * 24 * 3600 * 1000;

/**
 * The pack in use works, but the service knows a newer one (st.packOutdated). The user is
 * asked once per pack version and again after two weeks; declining costs nothing, the pack
 * in use keeps working. True when a download started, so the dictation must wait for it.
 */
async function offerPackUpdate(st) {
  if (!st.packOutdated) return false;
  const key = 'packUpdateDeclined';
  try {
    const saved = await Otzaria.call('storage.get', { key });
    const [version, at] = String((saved && saved.success && saved.data) || '').split(':').map(Number);
    if (version === st.packLatest && Date.now() - at < PACK_UPDATE_ASK_AGAIN_MS) return false;
  } catch (_) {}
  const ok = await ask(
    'חבילת דיבור מעודכנת',
    'יצאה חבילת דיבור מעודכנת, בגודל כ-' + megabytes(st.packSize) + ' מגה-בייט.\n' +
      'ההורדה לוקחת כמה דקות, ובזמנה אי אפשר להכתיב; אחריה הזיהוי משתמש בחבילה החדשה.\n' +
      'לא חייבים לעדכן: «ביטול» ממשיך עם החבילה הנוכחית, ונציע שוב בעוד שבועיים.\n' +
      'להוריד עכשיו?',
  );
  if (ok) return startPackDownload();
  try {
    await Otzaria.call('storage.set', { key, value: st.packLatest + ':' + Date.now() });
  } catch (_) {}
  return false;
}

/** True when version [a] ("2.1.0") is older than [b]; false when either is not made of plain numbers. */
function versionOlder(a, b) {
  const plain = (v) => /^\d+(\.\d+){0,2}$/.test(String(v));
  if (!plain(a) || !plain(b)) return false;
  const x = String(a).split('.').map(Number);
  const y = String(b).split('.').map(Number);
  for (let i = 0; i < 3; i++) {
    const d = (x[i] || 0) - (y[i] || 0);
    if (d) return d < 0;
  }
  return false;
}

const SERVICE_UPDATE_ASK_AGAIN_MS = 14 * 24 * 3600 * 1000;

/**
 * The installed voice service is older than SERVICE_UPDATE_BELOW(_WINDOWS). It is asked once per
 * LATEST_SERVICE and again after two weeks whether to get the new installer. "Not now" keeps the old
 * service, which works unless it is one that meets "cannot load" (the dialog says so). True when the
 * installer came down or the page opened, so the dictation waits.
 */
async function offerServiceUpdate(st) {
  const platform = st.platform || platformName;
  const windows = platform === 'windows';
  if (!st.version || !versionOlder(st.version, windows ? SERVICE_UPDATE_BELOW_WINDOWS : SERVICE_UPDATE_BELOW)) return false;
  // Installing now would cut a voice pack download short; the update waits for the next press.
  if (PACK_BUSY.includes(st.packJob && st.packJob.state)) return false;
  const key = 'serviceUpdateDeclined';
  try {
    const saved = await Otzaria.call('storage.get', { key });
    const [version, at] = String((saved && saved.success && saved.data) || '').split(':');
    if (version === LATEST_SERVICE && Date.now() - Number(at) < SERVICE_UPDATE_ASK_AGAIN_MS) return false;
  } catch (_) {}
  // Each part of the news goes only to a service that lacks it.
  const cannotLoadFix = windows && versionOlder(st.version, '2.5.2');
  const before251 = versionOlder(st.version, '2.5.1');
  // A service older than 2.5.0 also gets the 2.5.0 news (speed, and the end of typing into other programs).
  const before250 = versionOlder(st.version, '2.5.0');
  const ok = await ask(
    'עדכון לשירות הדיבור',
    // No version number: the installer comes from the latest release, which may be newer than
    // LATEST_SERVICE by the time a user sees this.
    'יצאה גרסה חדשה של שירות הדיבור.\n' +
      (cannotLoadFix
        ? 'היא מתקנת את השגיאה «cannot load … onnxruntime.dll» שהופיעה במחשבים בלי רכיבי Visual C++, ומוסיפה תמיכה ב-Windows 11 עם מעבד ARM.\n'
        : '') +
      (before251
        ? 'ההכתבה בה מדויקת יותר: בבדיקות שלנו (על הקלטות של דובר אחד) היא טועה בכ-18% פחות מילים.\n'
        : '') +
      (before250
        ? 'היא גם מהירה יותר מהגרסה שמותקנת אצלך: המודל נשאר טעון ברקע ולא נטען מחדש אחרי כל הפסקה, תוקנה ב-Windows איטיות שהופיעה כשהשירות עולה עם המחשב, והטקסט מפגר הרבה פחות בהכתבה ארוכה ורציפה.\n' +
          'שימו לב: הוסרה ההקלדה בדיבור לתוך תוכנות אחרות (Ctrl+Alt+M). ההכתבה פועלת בתוך אוצריא.\n'
        : '') +
      updateHow(windows) +
      (cannotLoadFix
        ? 'אם מופיעה אצלכם השגיאה «cannot load», צריך לעדכן כדי שההכתבה תעבוד. אם ההכתבה עובדת, לא חייבים: «ביטול» ממשיך עם הגרסה הנוכחית, ונציע שוב בעוד שבועיים.\n'
        : 'לא חייבים לעדכן: «ביטול» ממשיך עם הגרסה הנוכחית, ונציע שוב בעוד שבועיים.\n') +
      updateQuestion(windows),
  );
  try {
    await Otzaria.call('storage.set', { key, value: LATEST_SERVICE + ':' + Date.now() });
  } catch (_) {}
  if (!ok) return false;
  return getInstaller(platform);
}

/** Why a dictation could not run, in words the user can act on. */
async function explainFailure(e, st) {
  const code = e && e.code;
  if (code === 'unreachable') return explainNoService(st && st.platform);
  if (code === 'pack_missing' || code === 'pack_installing') {
    try {
      return offerPack(await serviceStatus());
    } catch (_) {
      return explainNoService(st && st.platform);
    }
  }
  if (code === 'microphone_background') {
    return sayError('פתחו פעם אחת את האפליקציה «שירות הדיבור של אוצריא» כדי לאפשר לה להשתמש במיקרופון');
  }
  if (code === 'microphone') {
    return sayError(
      e.message === 'no microphone'
        ? 'לא נמצא מיקרופון במחשב'
        : e.message === 'microphone in use'
          ? 'המיקרופון תפוס בתוכנה אחרת'
          : 'אין גישה למיקרופון (' + e.message + ')',
    );
  }
  return sayError('זיהוי הדיבור נכשל: ' + ((e && e.message) || e));
}
