// ------------------------------------------------------------------ service
//
// The speech is recognized by the local voice service (otzaria-voice,
// http://127.0.0.1:47811): it records from the microphone and streams text
// back while the user speaks. The plugin only starts and stops it.

const SERVICE = 'http://127.0.0.1:47811';
const HEADERS = { 'X-Otzaria-Voice': '1', 'Content-Type': 'application/json' };

class ServiceError extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
}

/**
 * One HTTP exchange with the service through network.fetchStream, yielding the
 * newline-delimited JSON objects of the body as they arrive. Chunk boundaries
 * do not follow lines, so partial lines are carried over.
 */
async function* serviceLines(path, options) {
  const o = options || {};
  let it;
  try {
    const stream = Otzaria.call('network.fetchStream', {
      url: SERVICE + path,
      method: o.method || 'POST',
      headers: HEADERS,
      body: o.body || '',
      timeoutMs: o.timeoutMs || 120000,
    });
    it = stream[Symbol.asyncIterator]();
  } catch (e) {
    throw new ServiceError('unreachable', String((e && e.message) || e));
  }
  if (o.signal) o.signal.close = () => it.return && it.return();
  let pending = '';
  let status = 200;
  let errorBody = '';
  try {
    while (true) {
      let step;
      try {
        step = await it.next();
      } catch (e) {
        throw new ServiceError('unreachable', String((e && e.message) || e));
      }
      if (step.done) break;
      const chunk = step.value;
      if (chunk.type === 'response') {
        status = chunk.status;
        continue;
      }
      if (status !== 200) {
        errorBody += chunk.body;
        continue;
      }
      pending += chunk.body;
      const lines = pending.split('\n');
      pending = lines.pop() || '';
      for (const line of lines) if (line.trim()) yield JSON.parse(line);
    }
  } finally {
    if (o.signal) o.signal.close = null;
  }
  if (status !== 200) {
    let err = {};
    try {
      err = JSON.parse(errorBody);
    } catch (_) {
      // not JSON
    }
    throw new ServiceError(err.error || 'http_' + status, err.message || 'HTTP ' + status);
  }
  if (pending.trim()) yield JSON.parse(pending);
}

/** A short request; returns the last JSON object of the answer. */
async function serviceCall(path, method) {
  let last = null;
  for await (const obj of serviceLines(path, { method, timeoutMs: 5000 })) last = obj;
  return last;
}

/**
 * Loads the recognition model while the plugin starts (it takes 1-4 s after
 * a pause). An older service answers 404 and a missing one is explained later
 * by readyToDictate, so failures are ignored here.
 */
function warmService() {
  serviceCall('/warm', 'POST').catch(() => {});
}

async function serviceStatus() {
  const st = await serviceCall('/status', 'GET');
  if (!st || st.service !== 'otzaria-voice') throw new ServiceError('unreachable', 'no service');
  return st;
}
