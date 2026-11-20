// ------------------------------------------------- the service's installer (GitHub edition)
//
// build.py puts this file in the GitHub edition ("released", id org.otzaria.voice-search) only.
// The store edition carries install_store.js instead, with the same functions: the Otzaria plugin
// store takes no plugin that downloads a program, so nothing here reaches it.

const DOWNLOAD_PAGE = 'https://github.com/yakov100/otzaria-voice/releases/latest';
// The Windows installers of the newest service. Otzaria downloads them itself (network.download) into
// the Downloads folder. GitHub redirects these addresses to releases/download/<tag>/..., and Otzaria
// checks every hop, so both prefixes are in the manifest's network.allowlist (and in Otzaria's
// official allowlist, plugin_network_allowlist.txt).
// A first installation gets the full installer, with the voice pack inside: one step, and the
// dictation works at once, also offline. An update or a reinstall gets the small one: the pack is
// already there, and the full installer does not replace a pack of version 3 or later anyway
// (PackNeeded in installer/windows/otzaria-voice.iss).
const WINDOWS_INSTALLER_FULL =
  'https://github.com/yakov100/otzaria-voice/releases/latest/download/otzaria-voice-windows-x64-with-models.exe';
const WINDOWS_INSTALLER =
  'https://github.com/yakov100/otzaria-voice/releases/latest/download/otzaria-voice-windows-x64-without-models.exe';
// Their sizes in MiB, like the plugin's other sizes (megabytes()), from service 2.5.3: 328,590,957
// and 2,830,183 bytes (313.4 and 2.7 MiB). The full one changes only with the voice pack.
const FULL_INSTALLER_SIZE = 'כ-315 מגה-בייט';
const SMALL_INSTALLER_SIZE = 'כ-3 מגה-בייט';
// Otzaria reports no progress of a download, and its messages fade after a few seconds.
const INSTALLER_REMINDER_MS = 60 * 1000;
// Where the last full installer was saved, and when ({ path, at }). A press soon after it, while the
// service still does not answer, points to that file before offering another 315MB copy (which
// Otzaria would save as "... (1).exe").
const FULL_INSTALLER_SAVED_KEY = 'fullInstallerSaved';
const FULL_INSTALLER_KEEP_MS = 24 * 3600 * 1000;

let installerDownload = null; // the running getInstaller, so that one download runs at a time
let installerPhase = ''; // of the running getInstaller: 'downloading', then 'saved' (its dialog) or 'page'
let fullInstallerSaved = null; // { path, at } of this engine's last full download

/** The path of a full installer saved in the last FULL_INSTALLER_KEEP_MS, or ''. */
async function recentFullInstaller() {
  let v = fullInstallerSaved;
  if (!v) {
    try {
      const saved = await Otzaria.call('storage.get', { key: FULL_INSTALLER_SAVED_KEY });
      v = saved && saved.success && saved.data;
    } catch (_) {}
  }
  return v && v.path && Date.now() - Number(v.at) < FULL_INSTALLER_KEEP_MS ? String(v.path) : '';
}

function rememberFullInstaller(path) {
  fullInstallerSaved = { path, at: Date.now() };
  try {
    Promise.resolve(Otzaria.call('storage.set', { key: FULL_INSTALLER_SAVED_KEY, value: fullInstallerSaved })).catch(() => {});
  } catch (_) {}
}

/** Where the installer is and how to open it. */
function howToOpen(path, full) {
  return 'הקובץ נשמר בתיקיית ההורדות:\n' + path + '\n\n' +
    'פתחו אותו פעם אחת (לחיצה כפולה). אם Windows מציגה «Windows הגן על המחשב», לוחצים «מידע נוסף» ואז «הפעל בכל זאת».\n' +
    (full ? 'חבילת הדיבור כלולה בו, ואין צורך להוריד אותה בנפרד.\n' : '') +
    'אחרי ההתקנה לחצו שוב על המיקרופון. את הקובץ שבתיקיית ההורדות אפשר אז למחוק.';
}

/**
 * Gets the service's installer to the user from inside Otzaria. On Windows Otzaria downloads it
 * into the Downloads folder; a plugin cannot start a program, so the user opens the file once.
 * [full]: the installer with the voice pack inside (a first installation), else the small one.
 * Elsewhere (no single installer file to fetch), or when the download fails, the download page
 * opens in the browser instead. True when the installer was downloaded or the page opened.
 */
function getInstaller(platform, full) {
  if (!installerDownload) {
    installerDownload = fetchInstaller(platform, !!full).finally(() => {
      installerDownload = null;
      installerPhase = '';
    });
  }
  return installerDownload;
}

async function fetchInstaller(platform, full) {
  if (platform === 'windows') {
    say(
      full
        ? 'מוריד את תוכנת ההתקנה של שירות הדיבור, כולל חבילת הדיבור (' + FULL_INSTALLER_SIZE + '). ' +
            'זה יכול לקחת כמה דקות. אפשר להמשיך לעבוד, אבל אל תסגרו את אוצריא עד שתופיע הודעה שההורדה הסתיימה.'
        : 'מוריד את תוכנת ההתקנה של שירות הדיבור…',
    );
    const reminder = full
      ? setInterval(() => say('תוכנת ההתקנה של שירות הדיבור עדיין יורדת… אל תסגרו את אוצריא'), INSTALLER_REMINDER_MS)
      : null;
    let res = null;
    installerPhase = 'downloading';
    try {
      res = await Otzaria.call('network.download', { url: full ? WINDOWS_INSTALLER_FULL : WINDOWS_INSTALLER });
    } catch (e) {
      res = { success: false, error: { message: String((e && e.message) || e) } };
    } finally {
      installerPhase = '';
      if (reminder) clearInterval(reminder);
    }
    // Why the page opens instead (permission, allowlist, network) shows in Otzaria's log.
    if (!res || !res.success) console.warn('voice-search: installer download failed', JSON.stringify(res && res.error));
    if (res && res.success && res.data && res.data.path) {
      installerPhase = 'saved';
      if (full) rememberFullInstaller(res.data.path);
      await ask('תוכנת ההתקנה ירדה', howToOpen(res.data.path, full));
      return true;
    }
    say('ההורדה מתוך אוצריא לא הצליחה, ולכן נפתח דף ההורדה בדפדפן');
  }
  installerPhase = 'page';
  const res = await Otzaria.call('app.openUrl', { url: DOWNLOAD_PAGE });
  if (res && res.success) return true;
  sayError('לא ניתן לפתוח את דף ההורדה: ' + DOWNLOAD_PAGE);
  return false;
}

/** True when the installer to offer is the full one: Windows, and no service ever answered here. */
async function wantsFullInstaller(platform) {
  return platform === 'windows' && !(await serviceSeenBefore());
}

/** What a press says while getInstaller runs. */
function installerBusyText() {
  return installerPhase === 'downloading'
    ? 'תוכנת ההתקנה של שירות הדיבור עדיין יורדת; נודיע כשהיא מוכנה. אל תסגרו את אוצריא'
    : installerPhase === 'saved'
      ? 'תוכנת ההתקנה של שירות הדיבור כבר ירדה לתיקיית ההורדות; פתחו אותה פעם אחת'
      : 'סגרו קודם את ההודעה של שירות הדיבור שעל המסך';
}

/** The question when the service does not answer ([saved]: a full installer that came down lately). */
function noServiceAsk(platform, full, saved) {
  if (saved) {
    // The full installer came down lately and the service still does not answer: most likely the
    // file was not opened yet.
    return {
      title: 'תוכנת ההתקנה כבר ירדה',
      content: howToOpen(saved, true) + '\n\n' +
        'אם כבר התקנתם, הפעילו מחדש את המחשב.\n' +
        'אם הקובץ כבר לא שם, לחצו «אישור» כדי להוריד אותו שוב (' + FULL_INSTALLER_SIZE + '). אחרת לחצו «ביטול».',
    };
  }
  return {
    title: 'שירות הדיבור לא פועל',
    content: serviceWhere(platform) +
      (platform === 'android'
        ? 'אם לא — לחצו «אישור» כדי לפתוח את דף ההורדה.'
        : platform !== 'windows'
          ? 'אם לא — לחצו «אישור» כדי לפתוח את דף ההורדה, הורידו את תוכנת ההתקנה והפעילו אותה פעם אחת.'
          : full
            ? 'אם לא — לחצו «אישור», ואוצריא תוריד את תוכנת ההתקנה, כולל חבילת הדיבור (' + FULL_INSTALLER_SIZE +
              '), לתיקיית ההורדות. ההורדה יכולה לקחת כמה דקות. פותחים את הקובץ פעם אחת, וזהו.'
            : 'אם לא — לחצו «אישור», ואוצריא תוריד את תוכנת ההתקנה (' + SMALL_INSTALLER_SIZE +
              ') לתיקיית ההורדות. פותחים את הקובץ פעם אחת, וזהו.'),
  };
}

/** How the update reaches the user, and the closing question (offerServiceUpdate). */
function updateHow(windows) {
  return windows
    ? 'השירות רץ במחשב ואינו מתעדכן דרך חנות התוספים. לעדכון, אוצריא תוריד את תוכנת ההתקנה החדשה (' + SMALL_INSTALLER_SIZE + ') לתיקיית ההורדות, ופותחים אותה פעם אחת. אם צריך, יוצע אחר כך להוריד חבילת דיבור חדשה.\n'
    : 'השירות רץ במחשב ואינו מתעדכן דרך חנות התוספים. לעדכון נפתח דף ההורדה; מורידים את הגרסה החדשה ומתקינים אותה פעם אחת. אם צריך, יוצע אחר כך להוריד חבילת דיבור חדשה.\n';
}

function updateQuestion(windows) {
  return windows ? 'להוריד את העדכון עכשיו?' : 'לפתוח את דף ההורדה?';
}
