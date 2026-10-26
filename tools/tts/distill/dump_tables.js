// Writes the plugin's abbreviation, name, letter-name and numeral tables to
// speech_tables.json beside this script, so the Python text pipeline
// (speech_text.py) says exactly what the plugin says:
//     node tools/tts/distill/dump_tables.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const src = fs.readFileSync(path.join(__dirname, '..', '..', '..', 'plugins', 'read-aloud', 'main.js'), 'utf8');
const from = src.indexOf('const ABBREVIATIONS');
const to = src.indexOf('function spellLetters');
const ctx = {};
vm.createContext(ctx);
vm.runInContext(src.slice(from, to) +
  '\nthis.out = { abbreviations: ABBREVIATIONS, names: [...NAMES], letterNames: LETTER_NAMES, ' +
  'numeralHeads: NUMERAL_HEADS.source, gematria: GEMATRIA.source, notNumerals: [...NOT_NUMERALS] };', ctx);
fs.writeFileSync(path.join(__dirname, 'speech_tables.json'), JSON.stringify(ctx.out, null, 1));
console.log('speech_tables.json: ' + Object.keys(ctx.out.abbreviations).length + ' abbreviations, ' +
  ctx.out.names.length + ' names');
