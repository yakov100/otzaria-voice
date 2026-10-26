// ----------------------------------------------------------------- feedback
//
// The plugin has no page: everything the user sees is Otzaria's own message
// strip (one at a time; a new message replaces the previous one) and its
// confirmation dialog. Messages are plain text, never HTML.

const DOWNLOAD_PAGE = 'https://github.com/yakov100/otzaria-voice/releases/latest';
// The newest voice service this plugin knows of (kVersion in service/src/common/service.cpp). The
// store updates the plugin but not the service, so an older one is offered the download page.
const LATEST_SERVICE = '2.5.0';
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

/** The service is not answering: not installed, or not started. */
async function explainNoService(platform) {
  const content =
    platform === 'android'
      ? 'הזיהוי נעשה באפליקציה קטנה בשם «שירות הדיבור של אוצריא», בלי אינטרנט.\n' +
        'אם היא מותקנת — פתחו אותה פעם אחת ולחצו על הכפתור שבה.\n' +
        'אם לא — לחצו «אישור» כדי לפתוח את דף ההורדה.'
      : 'הזיהוי נעשה בתוכנה קטנה שרצה ברקע במחשב, בלי אינטרנט.\n' +
        'אם היא מותקנת — הפעילו מחדש את המחשב, והיא תעלה מעצמה.\n' +
        'אם לא — לחצו «אישור» כדי לפתוח את דף ההורדה, הורידו את תוכנת ההתקנה והפעילו אותה פעם אחת.';
  if (await ask('שירות הדיבור לא פועל', content)) {
    const res = await Otzaria.call('app.openUrl', { url: DOWNLOAD_PAGE });
    if (!res || !res.success) sayError('לא ניתן לפתוח את דף ההורדה: ' + DOWNLOAD_PAGE);
  }
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
 * The installed voice service is older than LATEST_SERVICE. It is asked once per version and again
 * after two weeks whether to open the download page; the old service keeps working either way.
 * True when the page was opened, so the dictation waits for the installation.
 */
async function offerServiceUpdate(st) {
  if (!st.version || !versionOlder(st.version, LATEST_SERVICE)) return false;
  const key = 'serviceUpdateDeclined';
  try {
    const saved = await Otzaria.call('storage.get', { key });
    const [version, at] = String((saved && saved.success && saved.data) || '').split(':');
    if (version === LATEST_SERVICE && Date.now() - Number(at) < SERVICE_UPDATE_ASK_AGAIN_MS) return false;
  } catch (_) {}
  const ok = await ask(
    'עדכון לשירות הדיבור',
    'יצאה גרסה חדשה של שירות הדיבור (' + LATEST_SERVICE + '), וההכתבה בה מהירה יותר: המודל נשאר טעון ברקע ולא נטען מחדש אחרי כל הפסקה, תוקנה ב-Windows איטיות שהופיעה כשהשירות עולה עם המחשב, והטקסט מפגר הרבה פחות בהכתבה ארוכה ורציפה.\n' +
      'שימו לב: בגרסה הזו הוסרה ההקלדה בדיבור לתוך תוכנות אחרות (Ctrl+Alt+M). ההכתבה פועלת בתוך אוצריא.\n' +
      'השירות רץ במחשב ואינו מתעדכן דרך חנות התוספים. לעדכון נפתח דף ההורדה; מורידים את הגרסה החדשה ומתקינים אותה פעם אחת. אם צריך, יוצע אחר כך להוריד חבילת דיבור חדשה.\n' +
      'לא חייבים לעדכן: «ביטול» ממשיך עם הגרסה הנוכחית, ונציע שוב בעוד שבועיים.\n' +
      'לפתוח את דף ההורדה?',
  );
  try {
    await Otzaria.call('storage.set', { key, value: LATEST_SERVICE + ':' + Date.now() });
  } catch (_) {}
  if (!ok) return false;
  const res = await Otzaria.call('app.openUrl', { url: DOWNLOAD_PAGE });
  if (res && res.success) return true;
  sayError('לא ניתן לפתוח את דף ההורדה: ' + DOWNLOAD_PAGE);
  return false;
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
