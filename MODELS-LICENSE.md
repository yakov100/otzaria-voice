# רישיון שימוש מוגבל: מודל השפה והקול "חיים" של שירות הדיבור לאוצריא

גרסה 1.0, ‏4.11.2026. © 2026 yakov100 ("בעל הזכויות").

**בקצרה:** אם אתם משתמשים בשירות דרך אוצריא והתוספים המאושרים, אין לכם מה לעשות, רק לא להפיץ קובצי שמע בקול "חיים". אסור לקחת את הקבצים שבסעיף 1 לשימוש מחוץ לשירות ולאוצריא.

הנוסח העברי הוא המחייב. תרגום לאנגלית נמצא בסוף הקובץ.

## 1. על מה הרישיון חל

הרישיון חל על הקבצים האלה ("הקבצים המוגבלים"):

1. **מודל השפה** (`lm/library.otzlm`) בכל חבילת קול, כולל `voice-pack-v3` ו-`voice-pack-v4`. בחבילות האלה לא נכתב לו רישיון קודם.
2. **הקול "חיים"** (`voices/chaim.otzvoice`), כולל העותק שבחבילת ההקראה `voice-model-v1`, ועותקים וגרסאות של הקובץ הזה.

## 2. על מה הרישיון לא חל

הקבצים האלה נשארים ברישיון שלהם בלבד, והרישיון הזה לא מוסיף להם שום תנאי:

- התוכן של המאגר, שברישיון MIT (הקובץ [`LICENSE`](LICENSE)).
- מודל זיהוי הדיבור (`ctc/`) שבחבילות הקול. בחבילות `voice-pack-v3` ו-`voice-pack-v4` הוא ברישיון MIT. בחבילות הבאות הוא ברישיון שכתוב בקובץ `LICENSES.txt` שלהן.
- קבצים ברישיונות Creative Commons. למשל: רשימות הצליל (`phrases/talmud_sounds.tsv`, ‏`phrases/word_sounds.tsv`) ומילון הניקוד, שהם ב-CC BY-SA 4.0, ו-`nakdan_tokenizer.json` של דיקטה, שהוא ב-CC BY 4.0.
- מודלים של צד שלישי שבחבילות, כמו Chatterbox ו-Phonikud, וכל קובץ אחר שקובץ `LICENSES.txt` בחבילה מייחס לו רישיון אחר.
- הקול הקל "חיים קל" ומודלים עתידיים אחרים, חוץ ממודל השפה. הרישיון שלהם ייכתב בחבילה שלהם.

## 3. מה מותר

מותר להוריד את הקבצים המוגבלים ולהשתמש בהם בלי תשלום, רק באחת מהדרכים האלה:

1. **בשירות הדיבור לאוצריא,** כפי שבעל הזכויות מפרסם אותו ב-https://github.com/yakov100/otzaria-voice, במחשב או בטלפון של המשתמש, כולל מצבי שורת הפקודה שלו. רק הגרסה הרשמית של אוצריא והתוספים המאושרים שבסעיף 4 רשאים לפנות לשירות בבקשות שמשתמשות בקבצים המוגבלים.
2. **בגרסה הרשמית של אוצריא:** התוכנה שמפרסם פרויקט אוצריא ב-https://github.com/Otzaria/otzaria, והגרסאות שהוא עצמו מפרסם.
3. **לפיתוח ולבדיקה:** מותר להריץ את הקבצים בבנייה מקוד המקור של השירות, של תוסף מאושר או של אוצריא, כדי לפתח ולבדוק אותם. אסור להפיץ את הבנייה הזו עם הקבצים.

ההיתר כולל את העותקים שהשירות והמתקינים של בעל הזכויות יוצרים במחשב או בטלפון.

חוץ מפיתוח ובדיקה לפי פסקה 3, עותק משונה (fork) של השירות, של אוצריא או של תוסף מאושר אינו נחשב לאחד מאלה.

מותר להעתיק ולהעביר לאחרים מתקינים וחבילות קול שבעל הזכויות פרסם, בלי לשנות אותם, כדי שישתמשו בהם לפי הסעיף הזה.

מותר להוריד חבילה גם כדי להשתמש בקבצים שבה שאינם מוגבלים, לפי הרישיון שלהם. במקרה כזה אין להשתמש בקבצים המוגבלים שבה.

הטקסט שהשירות מקליד בהכתבה שייך למשתמש, והרישיון הזה לא מגביל אותו. קובצי שמע שהשירות יוצר בהקראה בקול "חיים" מותרים לשימוש אישי בלבד: מותר לשמוע אותם בכל נגן, ואסור להפיץ אותם.

## 4. התוספים המאושרים

| שם | מזהה | מפתח |
|---|---|---|
| חיפוש בדיבור | `org.otzaria.voice-search` | בעל הזכויות |
| הקראה קולית | `org.otzaria.read-aloud` | בעל הזכויות |
| וורד לאוצריא | `com.otzaria_word_editor.superdoc` | Y-PLONI |

תוסף מאושר הוא רק כפי שמפרסם אותו המפתח שלו. בעל הזכויות רשאי להוסיף תוספים לרשימה, או להוציא תוספים ממנה, בעדכון של הקובץ הזה.

## 5. מה אסור

בלי אישור בכתב מבעל הזכויות, אסור:

1. להעתיק או להפיץ את הקבצים המוגבלים, כולם או חלקם, חוץ ממה שסעיף 3 מתיר.
2. להריץ אותם בכל תוכנה אחרת, או לשלב אותם בתוכנה, באתר או בשירות אחרים.
3. לשנות אותם, להמיר אותם לפורמט אחר, או לחלץ מהם חלקים.
4. להשתמש בהם כדי לאמן, לכוונן או לזקק (distill) מודל אחר.
5. להשתמש בקול "חיים" כדי להשמיע דיבור בקול הזה מחוץ להקראה בשירות, חוץ מקובצי השמע לשימוש אישי שבסעיף 3.

## 6. הודעות של צד שלישי

הרישיונות והקרדיטים של שאר הקבצים בכל חבילה נמצאים בקובץ `LICENSES.txt` שבה, וחובה להשאיר אותם עם הקבצים.

## 7. בלי אחריות

הקבצים ניתנים "כמו שהם" (AS IS), בלי שום אחריות. בעל הזכויות אינו אחראי לשום נזק שנגרם מהשימוש בהם.

## 8. סיום

מי שמפר את הרישיון מאבד מיד את ההיתר שבסעיף 3.

## 9. דין וסמכות

על הרישיון חלים דיני מדינת ישראל. הסמכות הבלעדית נתונה לבתי המשפט המוסמכים בישראל.

## 10. בקשות לאישור

לבקשה לשימוש אחר יש לפתוח issue במאגר: https://github.com/yakov100/otzaria-voice/issues

---

## English translation (for convenience; the Hebrew text governs)

**Restricted use license: the language model and the voice "חיים" (Chaim) of the Otzaria voice service.** Version 1.0, November 4, 2026. © 2026 yakov100 (the "Owner").

**In short:** if you use the service through Otzaria and the approved plugins, there is nothing you need to do, except not distributing audio files in the voice "חיים". The files in section 1 may not be taken for use outside the service and Otzaria.

1. **Scope.** This license covers the following "Restricted Files":
   (a) the language model (`lm/library.otzlm`) in every voice pack, including `voice-pack-v3` and `voice-pack-v4`, where no license had been stated for it;
   (b) the voice "חיים" (`voices/chaim.otzvoice`), including the copy in the reading-aloud pack `voice-model-v1`, and copies and versions of that file.

2. **Not covered.** The following files stay under their own licenses only, and this license adds no terms to them:
   - the contents of the repository (MIT, see `LICENSE`);
   - the speech recognition model (`ctc/`) in the voice packs: MIT in `voice-pack-v3` and `voice-pack-v4`, and in later packs the license stated in their `LICENSES.txt`;
   - files under Creative Commons licenses (the sound lists, the nikud lexicon, Dicta's `nakdan_tokenizer.json`);
   - third-party models (such as Chatterbox and Phonikud), and any file that a pack's `LICENSES.txt` puts under another license;
   - the light voice and other future models, other than the language model, whose license will be stated in their own pack.

3. **Permitted use.** You may download and use the Restricted Files free of charge, only:
   (a) in the Otzaria voice service, as published by the Owner at https://github.com/yakov100/otzaria-voice, on the user's own computer or phone, including its command-line modes; requests to the service that use the Restricted Files may come only from the official Otzaria release and the approved plugins in section 4;
   (b) in the official release of Otzaria: the software that the Otzaria project publishes at https://github.com/Otzaria/otzaria, and the releases it publishes itself;
   (c) for development and testing: you may run the files in a build from the source code of the service, of an approved plugin or of Otzaria, to develop and test it. Such a build may not be distributed with the files.

   This permission includes the copies that the service and the Owner's installers make on the computer or phone.

   Except for development and testing under (c), a fork of the service, of Otzaria or of an approved plugin is not any of these.

   You may copy and pass on installers and voice packs published by the Owner, unmodified, for use under this section.

   You may also download a pack to use its unrestricted files under their own licenses; in that case, do not use the Restricted Files in it.

   Text that the service types in dictation belongs to the user and is not restricted by this license. Audio files that the service creates when reading aloud in the voice "חיים" are for personal use only: you may listen to them in any player, and you may not distribute them.

4. **Approved plugins.**
   - `org.otzaria.voice-search` (the Owner)
   - `org.otzaria.read-aloud` (the Owner)
   - `com.otzaria_word_editor.superdoc` (Y-PLONI)

   An approved plugin is one as published by its developer. The Owner may add plugins to this list, or remove them, by updating this file.

5. **Prohibited without the Owner's written permission:**
   (1) copying or distributing the Restricted Files, in whole or in part, except as section 3 permits;
   (2) running them in any other software, or including them in any other software, website or service;
   (3) modifying them, converting them, or extracting parts of them;
   (4) using them to train, fine-tune or distill another model;
   (5) using the voice "חיים" to produce speech in that voice outside reading aloud in the service, except the audio files for personal use in section 3.

6. **Third-party notices.** The licenses and credits of the other files in each pack are in its `LICENSES.txt`, and must be kept with the files.

7. **No warranty.** The files are provided "AS IS", without warranty of any kind. The Owner is not liable for any damage arising from their use.

8. **Termination.** Any breach immediately ends the permission in section 3.

9. **Law and jurisdiction.** This license is governed by the laws of the State of Israel. The competent courts in Israel have exclusive jurisdiction.

10. **Requests.** To ask for any other use, open an issue at https://github.com/yakov100/otzaria-voice/issues
