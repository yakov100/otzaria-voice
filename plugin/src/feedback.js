// ----------------------------------------------------------------- feedback
//
// The plugin has no page: everything the user sees is Otzaria's own message
// strip (one at a time; a new message replaces the previous one) and its
// confirmation dialog. Messages are plain text, never HTML.

const DOWNLOAD_PAGE = 'https://github.com/yakov100/otzaria-voice/releases/latest';
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
  try {
    await serviceCall('/install-pack', 'POST');
    say('מוריד את חבילת הדיבור…');
    watchPack();
  } catch (e) {
    sayError('הורדת חבילת הדיבור לא התחילה: ' + e.message);
  }
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
