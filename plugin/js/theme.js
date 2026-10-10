'use strict';

// Helpers and theme adoption from the Otzaria plugin template.

function esc(s) {
  const d = document.createElement('div');
  d.textContent = s == null ? '' : String(s);
  return d.innerHTML;
}

function hexToRgba(hex, alpha) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return 'rgba(' + r + ', ' + g + ', ' + b + ', ' + alpha + ')';
}

// ------------------------------------------------------------------- theme

/**
 * Push the host's theme into the CSS variables declared in style.css.
 *
 * Newer color roles are optional: a plugin running on an older Otzaria simply
 * won't receive them, so guard each one rather than assuming it's there.
 */
function applyTheme(theme) {
  if (!theme || !theme.colorScheme) return;
  const cs = theme.colorScheme;
  const root = document.documentElement;

  const set = (name, value) => {
    if (value) root.style.setProperty(name, value);
  };

  set('--color-primary',    cs.primary);
  set('--color-on-primary', cs.onPrimary);
  set('--color-secondary',    cs.secondary);
  set('--color-on-secondary', cs.onSecondary);
  set('--color-surface',            cs.surface);
  set('--color-on-surface',         cs.onSurface);
  set('--color-on-surface-variant', cs.onSurfaceVariant);
  set('--color-surface-container-high',    cs.surfaceContainerHigh);
  set('--color-surface-container-highest', cs.surfaceContainerHighest);
  set('--color-error',    cs.error);
  set('--color-on-error', cs.onError);
  set('--color-outline',  cs.outline);

  if (cs.primary) set('--color-primary-subtle', hexToRgba(cs.primary, 0.12));

  document.body.classList.toggle('dark-mode', theme.mode === 'dark');

  if (theme.typography) {
    const t = theme.typography;
    if (t.uiFontFamily) set('--font-ui', "'" + t.uiFontFamily + "', system-ui, sans-serif");
    if (t.fontFamily)   set('--font-main', "'" + t.fontFamily + "', 'David', serif");
    // typography.fontSize is the user's BOOK font size and can reach 36px, so it
    // drives reading text only. Feeding it into the UI base would inflate the
    // title bar and every control along with it.
    if (t.fontSize)     set('--font-size-reading', t.fontSize + 'px');
    if (t.lineHeight)   set('--line-height', String(t.lineHeight));
  }
}

