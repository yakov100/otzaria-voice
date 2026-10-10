# Contract: plugin buttons inside Otzaria's search fields

Shared by two parallel pieces of work:

- the Otzaria PR that adds this API (repo `Otzaria/otzaria`, branch from `dev`);
- the voice plugin in this repository (`plugin/`), which uses it when the host
  supports it and falls back gracefully when it does not.

Names below are the proposal. If the Otzaria side must rename something, it
updates this file in the same change so the plugin side follows.

## Manifest

```json
{
  "permissions": ["app.startup_contributions", "search.field_actions", "app.run_on_startup"],
  "contributes": {
    "startup": {
      "searchFieldActions": [
        {
          "id": "dictate",
          "title": "חיפוש בדיבור",
          "icon": "mic_24_regular",
          "activeIcon": "mic_24_filled",
          "fields": ["library", "inBook", "findRef"]
        }
      ]
    }
  }
}
```

- Declarative, like `toolbarItems`: Otzaria draws the button from the manifest
  without starting the plugin. Up to 2 actions per plugin.
- `fields`: which search fields show it. Unknown values are ignored; omitted =
  all supported fields. Supported fields are whatever the host implements; the
  host documents the list.
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
A click while the action's session is active sends instead:

`search.fieldAction.stopRequested` `{ "actionId", "sessionId" }`

`search.fieldAction.ended` `{ "actionId", "sessionId", "reason" }` —
reason: `"plugin"` (plugin ended it), `"closed"` (field disposed / tab closed),
`"edited"` (user typed in the field), `"replaced"` (a new session started).

## Methods (plugin → host), permission `search.field_actions`

- `search.setFieldText({ sessionId, text })` — sets the text this session
  contributes. The host keeps the field's text from before the session (the
  part outside the selection at invocation) and composes
  `before + separator + text + after`; the plugin always sends its full text,
  never deltas. Allowed only while the session is live.
- `search.setFieldActionState({ sessionId, state, tooltip? })` — `state`:
  `"active"` (shows `activeIcon`, highlighted), `"busy"` (progress), `"idle"`.
- `search.endFieldSession({ sessionId, submit? })` — ends the session;
  `submit: true` runs the search as if the user pressed Enter.

Security: a session exists only after a user click on that plugin's button in
that field; methods with an unknown, foreign or ended `sessionId` fail with
`error.not_found`. Calls are rate-limited like any RPC.

## Feature detection

`app.getInfo` version ≥ the release that ships this, or simply: if the call
`search.setFieldText` returns `error.unknown_method`, the host does not support
it. The plugin must keep working without it.
