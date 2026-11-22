// -------------------------------------------------- the service's installer (store edition)
//
// build.py puts this file in the store edition ("store", id com.yaakovisrael.voice-search) in
// place of install_github.js, with the same functions. The Otzaria plugin store takes a plugin that
// comes with a service only if it downloads no program, and its installer brings the service and
// this plugin together. So where the GitHub edition gets an installer, this one opens the store,
// where the installer is. A plugin's own page there is addressed by the store's internal id
// (/plugins/<24 hex digits>, src/lib/pluginRef.js of Otzaria_Website), not by the manifest id,
// so until that id is known the store's front page opens, and the text says what to look for.

const STORE_PAGE = 'https://otzaria.org/plugins';

let installerDownload = null; // the running getInstaller (the page opening), one at a time
let installerPhase = '';

/** Opens the plugin's page in the store. True when it opened. */
function getInstaller() {
  if (!installerDownload) {
    installerDownload = openStorePage().finally(() => {
      installerDownload = null;
      installerPhase = '';
    });
  }
  return installerDownload;
}

async function openStorePage() {
  installerPhase = 'page';
  const res = await Otzaria.call('app.openUrl', { url: STORE_PAGE });
  if (res && res.success) return true;
  sayError('לא ניתן לפתוח את חנות התוספים: ' + STORE_PAGE);
  return false;
}

async function wantsFullInstaller() {
  return false;
}

async function recentFullInstaller() {
  return '';
}

function installerBusyText() {
  return 'סגרו קודם את ההודעה של שירות הדיבור שעל המסך';
}

// The store offers the service's installer for Windows only (so far): elsewhere the page is opened
// too, but the text does not promise an installer there.
function noServiceAsk(platform) {
  return {
    title: 'שירות הדיבור לא פועל',
    content: serviceWhere(platform) +
      (platform === 'windows'
        ? 'אם לא — לחצו «אישור» כדי לפתוח את חנות התוספים. חפשו שם «חיפוש בדיבור», הורידו את תוכנת ההתקנה והפעילו אותה פעם אחת. חבילת הדיבור כלולה בה.'
        : 'אם לא — בחנות התוספים יש כרגע תוכנת התקנה של השירות ל-Windows בלבד. לחצו «אישור» כדי לפתוח את חנות התוספים (חפשו שם «חיפוש בדיבור»).'),
  };
}

function updateHow() {
  return 'השירות רץ במחשב ומתעדכן מחנות התוספים: בדף «חיפוש בדיבור» מורידים את תוכנת ההתקנה ומפעילים אותה פעם אחת.\n';
}

function updateQuestion() {
  return 'לפתוח את חנות התוספים?';
}
