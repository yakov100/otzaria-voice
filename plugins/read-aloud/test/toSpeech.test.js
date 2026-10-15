// Checks toSpeech (abbreviations, numerals, the map) without Otzaria:
//     node plugins/read-aloud/test/toSpeech.test.js
// main.js is loaded as text and its text helpers evaluated here; the parts
// that need Otzaria are never reached.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const src = fs.readFileSync(path.join(__dirname, '..', 'main.js'), 'utf8');
const from = src.indexOf('const DIVINE_NAME');
const to = src.indexOf('/** First spoken offset');
const ctx = {};
vm.createContext(ctx);
vm.runInContext(src.slice(from, to) + '\nthis.toSpeech = toSpeech; this.rewrites = rewrites;', ctx);
const { toSpeech } = ctx;

let failed = 0;
function same(input, expected) {
  const { text, map } = toSpeech(input);
  const ok = text === expected && map.length === text.length + 1 && map[text.length] === input.length &&
    map.every((v, i) => i === 0 || v >= map[i - 1]);
  if (!ok) {
    failed++;
    console.log('FAIL', JSON.stringify(input), '\n  got     ', JSON.stringify(text), '\n  expected', JSON.stringify(expected),
      map.length === text.length + 1 ? '' : '(map length ' + map.length + ')');
  }
}

// abbreviations
same('ת"ר מאימתי', 'תנו רבנן מאימתי');
same('וכו\' ואמר', 'וכולי ואמר');
same('אע"ג דאמר', 'אף על גב דאמר');
same('עי\' בתוס\' שם', 'עיין בתוספות שם');
same('כמ"ש הרמב"ם', 'כמו שכתב הרמבם');
same('וכתב רש"י', 'וכתב רשי');
same('שו"ע או"ח סי\' ז', 'שולחן ערוך אורח חיים סימן זַיִן');
same('ת״ר (gershayim U+05F4)', 'תנו רבנן (gershayim U+05F4)');
same('וכו׳ (geresh U+05F3)', 'וכולי (geresh U+05F3)');
same('הקב"ה', 'הקדוש ברוך הוא');
// unknown abbreviations: by letter
same('פ"ז כתב', 'פֵּא זַיִן כתב');
same('בח"ק', 'בֵּית חֵית קוֹף');
// a longer unknown word with a geresh keeps its letters
same('ירושלמ\'', 'ירושלמ');
// numerals
same('דף ב', 'דף בֵּית');
same('פרק ג\'', 'פרק גִּימֶל');
same('פרק י"ד', 'פרק יוֹד דָּלֶת');
same('סימן קכ"ג', 'סימן קוֹף כַּף גִּימֶל');
same('דף ב.', 'דף בֵּית עַמּוּד אָלֶף');
same('ברכות ב:', 'ברכות בֵּית עַמּוּד בֵּית');
same('דף יד:', 'דף יוֹד דָּלֶת עַמּוּד בֵּית');
same('לב: אמר', 'לב: אמר');  // not a numeral: a common word
// plain words that look like numerals stay words
same('על יד הבית', 'על יד הבית');
same('גל עיני', 'גל עיני');
same('הלכה יד', 'הלכה יוֹד דָּלֶת');  // but after a head it is a numeral
same('דף יש', 'דף יש');
// sentence punctuation after a short word is not a daf
same('אמר רב.', 'אמר רב.');
same('מאי טעמא דרב. אמר', 'מאי טעמא דרב. אמר');
// the old behaviour stays
same('יְהוָה אלהים', 'השם אלהים');
same('בְּרֵאשִׁ֖ית בָּרָ֣א', 'בְּרֵאשִׁית בָּרָא');
same('את־השמים', 'את השמים');
// vocalized abbreviation stays as a word (rare, left alone)
same('רַבִּי', 'רַבִּי');

if (failed) {
  console.log(failed + ' failed');
  process.exit(1);
}
console.log('toSpeech: all passed');
