# חיים קל: זיקוק הקול המוקלט למודל קטן ומהיר

הקול המוקלט "חיים" רץ על Chatterbox (900MB, בקצב של אורך ההקראה). כאן הוא משמש
רק אצלנו: הוא מקריא עשרות שעות של טקסט תורני, ועל ההקלטות האלה מאמנים מודל
VITS קטן (Piper, ~63MB, פי 30 מזמן אמת) שמדבר באותו קול. התכנית המלאה:
`docs/tts/light-voice.md` (ותיעוד הרעיון: `docs/tts/ideas.md`).

תיקיית העבודה: `%LOCALAPPDATA%\OtzariaVoice\distill` (להלן `<work>`), עם עותק של
`otzaria-voice.exe` (בנייה שיש בה `--vocalize --marks` ו-`--narrate` עם batching בין שורות).

## הצינור

1. **טבלאות הדיבור** (פעם אחת, אחרי כל שינוי ב-`main.js`):
   `node tools/tts/distill/dump_tables.js` → `speech_tables.json`.
   `speech_text.py` הוא ה-port של `toSpeech` של התוסף (ר"ת, שמות, גימטריה, שם ה').
2. **בחירת משפטים:** `python select_text.py --dicta <Talmud-Bavli-with-Nikud> --out <work>\text`
   45% גמרא (דיקטה, מנוקד), 20% משנה, 15% תנ"ך וסידור, 10% רש"י, 10% הלכה; 40–200 תווים;
   300 אלף מילים ≈ 34 שעות. פלט: `sentences.tsv`, `sentences.plain.txt`, `pilot.txt`.
3. **ניקוד וסימני הגייה:** `otzaria-voice.exe --vocalize <pack> sentences.plain.txt sentences.vocalized.txt --marks`
   (מילון + מודל; ֫ U+05AB הטעמה, ֽ U+05BD שווא נע; כ-50 דקות ל-17 אלף שורות).
4. **ייצור האודיו:** `python generate.py --work <work> [--limit-lines N]` או בלילה
   `powershell -File run_nights.ps1 -StopAt 07:00`. shards של 300 שורות, המשך אוטומטי,
   `STOP` עוצר אחרי ה-shard הנוכחי. פלט: `wavs/` (22.05 kHz), `wav16/`, `clips.jsonl`.
5. **ההקלטות האמיתיות:** `python import_recordings.py --rec C:\Users\fried\otzaria-stt\rec --pron regular`
   (92 דקות, 16 kHz; `r-<id>`, source=real).
6. **בקרת איכות:** `python qc.py --work <work>` (זיהוי דיבור של השירות על `wav16`, CER, מהירות,
   שקט, קיצוץ) → `metadata.csv`, `qc.json`, `rejected.jsonl`.
7. **אריזה לאימון:** `python make_dataset_zip.py --work <work> [--max-hours 2]` → `dataset.zip`
   (להעלות ל-Google Drive, `otzaria-light/`).
8. **אימון ב-Colab:** `train_colab.ipynb` (Colab Pro, GPU). ניסוי: בסיס he_IL-saspeech (לא-מסחרי,
   לניסוי בלבד), 3000 epochs; מלא: בסיס en_US-lessac (חופשי). ייצוא ל-`chaim-light.onnx` + JSON,
   ודוגמאות להאזנה ב-`runs/<run>/samples`.

## רישיונות
Chatterbox (MIT) הוא מקור האודיו הסינתטי; Piper (GPL-3) משמש לאימון בלבד, הרשת בשירות
רצה על ONNX Runtime בלי קוד Piper; הטקסטים: ש"ס דיקטה (CC BY-SA) וספרים חופשיים
מהספרייה; הקול: חיים, בהסכמתו.

## Uploading to Drive by browser, and the light-voice test mode (16.10)
- `split_for_drive.py` re-packs `dataset.zip` as FLAC and splits it into 9MB parts
  (`dataset-flac.zip.NNN` + `.sha256`); `train_colab.ipynb` cell 3 joins and checks them.
  Needed when the upload goes through a browser automation that takes 10MB a call.
- `otzaria-voice --light <model.onnx> <text.txt> <out dir> [--raw]` speaks each line with a
  light voice (vocalized by the installed diacritizer first; `--raw` feeds the lines as symbols),
  writing NN.wav and report.jsonl with the symbols, id counts and seconds.
