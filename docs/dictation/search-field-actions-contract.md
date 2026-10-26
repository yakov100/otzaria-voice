# Contract: plugin buttons inside Otzaria's search fields

Shared by two parallel pieces of work:

- the Otzaria PR that adds this API (repo `Otzaria/otzaria`, branch from `dev`);
- the voice plugin in this repository (`plugin/`), which uses it when the host
  supports it and falls back gracefully when it does not.

Names below are what the Otzaria PR implements (branch
`feat/plugin-search-field-actions`, merged into `dev` as Otzaria/otzaria#2320 on
2026-10-10; Otzaria marks these APIs as **0.9.98.1**, the next release after 0.9.98). Sections marked
"Implemented as" record where the Otzaria side refined the original proposal.

## Manifest

```json
{
  "minAppVersion": "0.9.98.1",
  "permissions": ["app.startup_contributions", "search.field_actions", "app.run_on_startup"],
  "contributes": {
    "startup": {
      "searchFieldActions": [
        {
          "id": "dictate",
          "title": "חיפוש בדיבור",
          "icon": "mic_24_regular",
          "activeIcon": "mic_24_filled",
          "fields": ["fullText", "library", "inBook", "findRef"]
        }
      ]
    }
  }
}
```

- Declarative, like `toolbarItems`: Otzaria draws the button from the manifest
  without starting the plugin. Up to 2 actions per plugin.
- `fields`: which search fields show it. Unknown values are ignored (the
  packager warns); omitted = all supported fields. Supported in 0.9.98.1:
  - `fullText` — the full-text library search field (search dialog / tab);
  - `library` — "find a book or author" in the library browser;
  - `inBook` — search inside an open book (text and PDF);
  - `findRef` — the "find reference" dialog.

  Implemented as: `library` is the book/author locator, *not* full-text search
  (Otzaria has both); full-text search is the separate `fullText` value.
- Item schema: `id` (required, `[A-Za-z0-9._-]`, ≤64), `title` (required,
  ≤120, single line), `icon` / `activeIcon` (optional icon names, resolved like
  `toolbarItems`), `fields`. Any other key rejects the item.
- At most 3 plugin buttons are shown in one field (all plugins together).
- New permission `search.field_actions` (user-grantable, default on like other
  UI contributions, shown in the install prompt).
- Clicking wakes the plugin's background engine on demand (same lazy activation
  as toolbar items). A `headless` plugin is the expected shape.

## Events (host → plugin)

`search.fieldAction.invoked`
```json
{ "actionId": "dictate", "sessionId": "s_…", "field": "library",
  "text": "current field text", "selectionStart": 0, "selectionEnd": 0 }
```
Sent like a toolbar-item click: it wakes the background engine (needs
`app.run_on_startup`); without that permission the plugin page is opened and
receives the event. `text`/`selection*` describe the field at the click; an
invalid selection (field never focused) is reported as the end of the text.

A click while the action's session is active sends instead (the session stays
open until the plugin ends it):

`search.fieldAction.stopRequested` `{ "actionId", "sessionId" }`

`search.fieldAction.ended` `{ "actionId", "sessionId", "reason" }` —
reason: `"plugin"` (plugin ended it), `"closed"` (field disposed / tab closed),
`"edited"` (user typed in the field), `"replaced"` (a new session started).

Implemented as:
- `"closed"` also covers: the plugin was disabled / lost the permission / the
  button was removed from the manifest, and the plugin instance bound to the
  session went away (e.g. the lazy background engine was shut down after 3
  minutes without RPC activity — keep calling the API during long dictation, or
  use `keepAlive`).
- `"replaced"`: one live session per field **and** per plugin — a new click in
  the same field, or this plugin's button in another field, replaces it.
- `ended` is sent for every reason, including `"plugin"` after
  `search.endFieldSession`.

## Methods (plugin → host), permission `search.field_actions`

- `search.setFieldText({ sessionId, text })` — sets the text this session
  contributes. The host keeps the field's text from before the session (the
  part outside the selection at invocation) and composes
  `before + separator + text + after`; the plugin always sends its full text,
  never deltas. Allowed only while the session is live.

  Implemented as: a single space is inserted on **each** side only where the
  boundary has no whitespace already (`before␠text␠after`); caret goes after
  `text`. `text` must be a string of ≤1000 characters (`error.invalid_params`
  otherwise); control characters (newlines etc.) become spaces. Text containing
  `://` is rejected (`error.invalid_params`): Enter in the library and find-ref
  fields follows `otzaria://` links, which would let a plugin bypass its
  permissions (e.g. open another plugin). The field reacts
  as to typing (live search in `library` / `inBook` / `findRef`); `fullText`
  only searches on `submit`.
- `search.setFieldActionState({ sessionId, state, tooltip? })` — `state`:
  `"active"` (shows `activeIcon`, highlighted), `"busy"` (progress), `"idle"`.
  Implemented as: the button is highlighted for the whole life of the session
  (also `idle`); `tooltip` ≤120 chars, single line, replaces the title while the
  session lives. Bad values → `error.invalid_params`.
- `search.endFieldSession({ sessionId, submit? })` — ends the session;
  `submit: true` runs the search as if the user pressed Enter.

All three return `true` on success.

Security: a session exists only after a user click on that plugin's button in
that field; methods with an unknown, foreign or ended `sessionId` fail with
`error.not_found`. Calls are rate-limited like any RPC.

Implemented as: the first plugin instance (background engine or a tab) that
calls a method on a session locks it; calls from another instance of the same
plugin get `error.not_found`, and later events for the session go to that
instance only. Without the granted permission: `permission_denied`.

## Feature detection

`app.getInfo` version ≥ the release that ships this, or simply: if the call
`search.setFieldText` returns `error.unknown_method`, the host does not support
it. The plugin must keep working without it.

Plugin side (as built): runtime detection alone is not enough, because one
package cannot serve both hosts — 0.9.98 rejects the unknown permission
`search.field_actions` at install, and the packager rejects calls to
`search.setFieldText` & co. under `minAppVersion` 0.9.98. So `plugins/dictation/build.py`
produces two packages from one source: `released` (0.9.98, toolbar button +
shortcut, no field-action code) and `next` (0.9.98.1, adds the permission, the
`searchFieldActions` item with `fields: fullText, library, inBook, findRef`, and
the handlers). The next package keeps the toolbar button and shortcut too.
