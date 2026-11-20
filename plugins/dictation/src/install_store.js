// -------------------------------------------------- the service's installer (store edition)
//
// build.py puts this file in the store edition ("store", id com.yaakovisrael.voice-search) in
// place of install_github.js, with the same functions. The Otzaria plugin store takes a plugin that
// comes with a service only if it downloads no program, and its installer brings the service and
// this plugin together. So where the GitHub edition gets an installer, this one opens the plugin's
// page in the store, where the installer is.

const STORE_PAGE = 'https://otzaria.org/plugins/com.yaakovisrael.voice-search';

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
  sayError('לא ניתן לפתוח את דף התוסף בחנות התוספים: ' + STORE_PAGE);
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

function noServiceAsk(platform) {
  return {
    title: 'שירות הדיבור לא פועל',
    content: serviceWhere(platform) +
      'אם לא — לחצו «אישור» כדי לפתוח את דף התוסף בחנות התוספים, הורידו משם את תוכנת ההתקנה והפעילו אותה פעם אחת.',
  };
}

function updateHow() {
  return 'השירות רץ במחשב ומתעדכן מדף התוסף בחנות התוספים: מורידים משם את תוכנת ההתקנה ומפעילים אותה פעם אחת. אם צריך, יוצע אחר כך להוריד חבילת דיבור חדשה.\n';
}

function updateQuestion() {
  return 'לפתוח את דף התוסף בחנות?';
}
