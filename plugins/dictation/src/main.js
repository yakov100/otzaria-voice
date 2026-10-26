// --------------------------------------------------------------- lifecycle
//
// A headless plugin: Otzaria draws its buttons from the manifest and starts
// this script in a hidden background engine only when one is used (needs the
// "run in background" permission); the engine closes itself after a few idle
// minutes. Handlers are registered before plugin.boot so that the event that
// woke the engine is not missed.

let platformName = '';

Otzaria.on('plugin.boot', (payload) => {
  platformName = (payload && payload.app && payload.app.platform) || '';
  warmService();
});

Otzaria.on('app.command', (ev) => {
  if (ev && ev.command === 'dictate') toggleSearchDictation();
});

if (typeof installFieldActions === 'function') installFieldActions();
