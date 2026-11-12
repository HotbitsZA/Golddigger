/* GolddiggerUI - the front end.
 *
 * A small dependency free single page application over the Drogon JSON API.
 * The timeframe drives everything: the server resolves the matching .dat
 * model, reads the newest candles and runs the inference. Nothing here keeps
 * a database; settings and orders are files on the server.
 */
'use strict';

const TIMEFRAMES = ['m15', 'h1', 'd1'];
const TIMEFRAME_LABEL = { m15: 'M15', h1: 'H1', d1: 'D1' };

const state = {
  timeframe: 'm15',
  source: 'file',
  status: null,
  settings: {
    instrument: 'xauusd',
    dataSource: 'file',
    executionMode: 'manual',
    autoArmed: false,
    paperTrading: true,
    volumeLots: 0.10,
    estimatedSpread: 0.40,
    pollSeconds: 15,
    autoTimeframes: ['m15'],
  },
  providers: {},
  candles: null,
  candlesError: '',
  signal: null,
  signalError: '',
  orders: null,
  ordersError: '',
  busy: false,
  timer: null,
  pollMs: 15000,
};

const $ = (id) => document.getElementById(id);

/* ------------------------------------------------------------------ utils */

function toast(message, kind) {
  const element = $('toast');
  element.textContent = message;
  element.className = 'toast' + (kind ? ' ' + kind : '');
  element.classList.remove('hidden');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => element.classList.add('hidden'), 4500);
}

async function api(path, options) {
  const init = Object.assign({}, options || {});
  init.headers = Object.assign({ Accept: 'application/json' }, init.headers || {});
  if (init.body && !init.headers['Content-Type']) {
    init.headers['Content-Type'] = 'application/json';
  }

  const response = await fetch(path, init);
  let payload = null;
  const text = await response.text();
  if (text) {
    try {
      payload = JSON.parse(text);
    } catch (error) {
      payload = null;
    }
  }

  if (!response.ok) {
    const message = payload && payload.error ? payload.error : ('request failed: ' + response.status);
    throw new Error(message);
  }
  return payload;
}

function clear(node) {
  while (node && node.firstChild) node.removeChild(node.firstChild);
}

function setText(id, text) {
  const node = $(id);
  if (node) node.textContent = text === null || text === undefined ? '' : String(text);
}

function num(value, digits) {
  if (value === null || value === undefined || value === '') return '—';
  const parsed = Number(value);
  if (!isFinite(parsed)) return '—';
  return parsed.toLocaleString(undefined, {
    minimumFractionDigits: digits === undefined ? 2 : digits,
    maximumFractionDigits: digits === undefined ? 2 : digits,
  });
}

function signed(value, digits) {
  if (value === null || value === undefined || value === '') return '—';
  const parsed = Number(value);
  if (!isFinite(parsed)) return '—';
  return (parsed >= 0 ? '+' : '') + num(parsed, digits === undefined ? 2 : digits);
}

function pct(value) {
  if (value === null || value === undefined || value === '') return '—';
  const parsed = Number(value);
  if (!isFinite(parsed)) return '—';
  return (parsed >= 0 ? '+' : '') + num(parsed, 2) + '%';
}

function formatDateTime(value) {
  if (value === null || value === undefined || value === '') return '—';
  const date = typeof value === 'number' ? new Date(value) : new Date(value);
  if (isNaN(date.getTime())) return String(value);
  return date.toLocaleString([], {
    month: 'short', day: '2-digit', hour: '2-digit', minute: '2-digit',
  });
}

function formatClock(value) {
  if (value === null || value === undefined || value === '') return '—';
  const date = typeof value === 'number' ? new Date(value) : new Date(value);
  if (isNaN(date.getTime())) return '—';
  return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

function chartLabel(timestamp) {
  const date = new Date(timestamp);
  if (isNaN(date.getTime())) return '';
  if (state.timeframe === 'd1') {
    return date.toLocaleDateString([], { month: '2-digit', day: '2-digit' });
  }
  return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function formatAge(seconds) {
  const value = Number(seconds);
  if (!isFinite(value) || value < 0) return '—';
  if (value < 60) return Math.round(value) + 's';
  if (value < 3600) return Math.round(value / 60) + 'm';
  if (value < 86400) return Math.round(value / 3600) + 'h';
  return Math.round(value / 86400) + 'd';
}

function formatInterval(ms) {
  const minutes = Number(ms) / 60000;
  if (!isFinite(minutes) || minutes <= 0) return '—';
  if (minutes < 60) return minutes + ' minutes';
  if (minutes < 1440) return (minutes / 60) + ' hours';
  return (minutes / 1440) + ' days';
}

function currentSummary() {
  if (!state.status || !Array.isArray(state.status.timeframes)) return null;
  return state.status.timeframes.find((entry) => entry.id === state.timeframe) || null;
}

function timeframeLabel(id) {
  return TIMEFRAME_LABEL[id] || String(id || '').toUpperCase();
}

/* --------------------------------------------------------------- status */

async function loadStatus() {
  const payload = await api('/api/status');
  state.status = payload;
  state.providers = payload.providers || state.providers || {};
  if (payload.settings) {
    state.settings = Object.assign({}, state.settings, payload.settings);
  }

  const ids = (payload.timeframes || []).map((entry) => entry.id);
  if (ids.length && ids.indexOf(state.timeframe) === -1) {
    state.timeframe = ids[0];
    localStorage.setItem('golddigger.timeframe', state.timeframe);
  }

  renderPills();
  renderTimeframes();
  renderHero();
  renderProviderReadiness();
  renderFoot();
}

async function loadSettings() {
  const payload = await api('/api/settings');
  state.settings = payload.settings || state.settings;
  state.providers = payload.providers || state.providers || {};
  populateSettings();
  renderProviderReadiness();
  startPolling();
}

/* ------------------------------------------------------------- rendering */

function renderPills() {
  const settings = state.settings || {};
  setText('pill-instrument', (settings.instrument || 'xauusd').toUpperCase());

  const mode = settings.executionMode === 'auto'
    ? (settings.autoArmed ? 'auto · armed' : 'auto · unarmed')
    : 'manual';
  setText('pill-mode', mode);

  setText('pill-paper', settings.paperTrading === false ? 'LIVE' : 'paper');
  $('pill-paper').className = 'pill ' + (settings.paperTrading === false ? 'live' : 'paper');

  setText('pill-now', new Date().toLocaleTimeString([], {
    hour: '2-digit', minute: '2-digit', second: '2-digit',
  }));
}

function renderTimeframes() {
  const host = $('timeframe-switch');
  clear(host);
  if (!host) return;

  const summaries = (state.status && state.status.timeframes) || [];
  const ids = summaries.length ? summaries.map((entry) => entry.id) : TIMEFRAMES;

  ids.forEach((id) => {
    const summary = summaries.find((entry) => entry.id === id) || null;
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'segment' + (id === state.timeframe ? ' active' : '');
    button.dataset.timeframe = id;
    button.setAttribute('role', 'tab');
    button.setAttribute('aria-selected', id === state.timeframe ? 'true' : 'false');

    const label = document.createElement('span');
    label.textContent = timeframeLabel(id);
    button.appendChild(label);

    const dot = document.createElement('span');
    const modelReady = summary && summary.model && summary.model.exists;
    const stale = summary && summary.data && summary.data.stale;
    dot.className = 'dot ' + (!modelReady ? 'missing' : (stale ? 'stale' : 'ok'));
    dot.title = !summary ? 'waiting for status' :
      (!modelReady ? 'no model found' : (stale ? 'candles are stale' : 'model and candles ready'));
    button.appendChild(dot);

    host.appendChild(button);
  });
}

function heroCard(label, value, hint, kind, title) {
  const card = document.createElement('div');
  card.className = 'hero-stat' + (kind ? ' ' + kind : '');
  if (title) card.title = title;

  const caption = document.createElement('div');
  caption.className = 'hero-stat-label';
  caption.textContent = label;
  card.appendChild(caption);

  const amount = document.createElement('div');
  amount.className = 'hero-stat-value';
  amount.textContent = value;
  card.appendChild(amount);

  const detail = document.createElement('div');
  detail.className = 'hero-stat-hint';
  detail.textContent = hint;
  card.appendChild(detail);
  return card;
}

function renderHero() {
  const summary = currentSummary();
  const settings = state.settings || {};

  setText('hero-label', summary ? summary.label : timeframeLabel(state.timeframe));
  setText('hero-sub', summary
    ? (settings.instrument || 'xauusd').toUpperCase() + ' · ' + formatInterval(summary.intervalMs) +
      ' candles · the model is selected automatically from the timeframe'
    : 'Choose a timeframe; the matching model is selected automatically.');

  const stale = !!(summary && summary.data && summary.data.stale);
  $('hero-stale').classList.toggle('hidden', !stale);

  const host = $('hero-cards');
  clear(host);
  if (!summary) {
    host.appendChild(heroCard('Model', '—', 'waiting for the server', ''));
    return;
  }

  const model = summary.model || {};
  const data = summary.data || {};

  host.appendChild(heroCard(
    'Model',
    model.exists ? (model.name || 'ready') : 'missing',
    model.exists ? model.name : 'expected: ' + (model.name || model.path || '—'),
    model.exists ? 'ok' : 'bad',
    model.path || ''
  ));

  host.appendChild(heroCard(
    'Candles',
    data.candles === undefined ? '0' : String(data.candles),
    data.latestLocal || data.latestUtc || 'no candle file',
    data.exists ? 'ok' : 'bad',
    data.path || ''
  ));

  host.appendChild(heroCard(
    'Freshness',
    data.stale ? 'stale' : 'fresh',
    (data.ageSeconds === undefined ? '—' : formatAge(data.ageSeconds) + ' old'),
    data.stale ? 'warn' : 'ok',
    data.latestUtc || ''
  ));

  const mode = settings.executionMode === 'auto'
    ? (settings.autoArmed ? 'auto · armed' : 'auto · unarmed')
    : 'manual';
  host.appendChild(heroCard(
    'Execution',
    mode,
    (settings.paperTrading === false ? 'live via MT5' : 'paper trading') +
      ' · ' + num(settings.volumeLots, 2) + ' lots',
    settings.executionMode === 'auto' && settings.autoArmed ? 'warn' : '',
    'safe defaults: manual, paper, unarmed'
  ));
}

function renderProviderReadiness() {
  const host = $('provider-readiness');
  if (!host) return;
  clear(host);

  const providers = state.providers || {};
  const names = Object.keys(providers);
  if (!names.length) {
    const span = document.createElement('span');
    span.className = 'hint';
    span.textContent = 'No provider information yet.';
    host.appendChild(span);
    return;
  }

  names.forEach((name) => {
    const info = providers[name] || {};
    const span = document.createElement('span');
    span.className = 'provider ' + (info.ready ? 'ok' : 'bad');
    span.title = info.detail || '';
    const dot = document.createElement('span');
    dot.className = 'dot ' + (info.ready ? 'ok' : 'missing');
    span.appendChild(dot);
    span.appendChild(document.createTextNode(name));
    host.appendChild(span);
  });
}

function renderFoot() {
  const paths = (state.status && state.status.paths) || null;
  if (!paths) return;
  setText('foot-paths', 'data: ' + paths.dataDir + ' · models: ' + paths.modelsDir);
}

/* ---------------------------------------------------------------- candles */

async function loadCandles() {
  const query = '/api/candles?timeframe=' + encodeURIComponent(state.timeframe) +
    '&count=5&source=' + encodeURIComponent(state.source);
  try {
    state.candles = await api(query);
    state.candlesError = '';
  } catch (error) {
    state.candlesError = error.message;
  }
  renderCandles();
}

function renderCandles() {
  const payload = state.candles;
  const candles = (payload && payload.candles) || [];

  if ($('candle-source')) $('candle-source').value = state.source;

  const hint = $('candles-hint');
  if (hint) {
    if (payload) {
      hint.textContent = 'source: ' + payload.source + ' · ' + (payload.dataPath || 'csv files');
      hint.title = payload.dataPath || '';
    } else {
      hint.textContent = 'The newest completed candles of the selected timeframe.';
    }
  }

  const warning = $('candles-warning');
  const warningText = state.candlesError || (payload && payload.warning) || '';
  warning.textContent = warningText;
  warning.classList.toggle('hidden', !warningText);

  const last = candles.length ? candles[candles.length - 1] : null;
  setText('last-price', last ? num(last.close) : '--');
  setText('last-change', last ? pct(last.changePct) : '--');
  $('last-change').className = 'change ' + (!last ? '' : (Number(last.changePct) >= 0 ? 'positive' : 'negative'));

  if (last) {
    setText('last-meta', (last.local || formatDateTime(last.timestamp)) +
      ' · O ' + num(last.open) + ' H ' + num(last.high) +
      ' L ' + num(last.low) + ' C ' + num(last.close) +
      ' · vol ' + num(last.volume, 0));
  } else {
    setText('last-meta', payload ? 'no candles available' : '');
  }

  renderChart(candles);
  renderCandleTable(candles);
}

function renderChart(candles) {
  const host = $('candle-chart');
  if (!host) return;
  clear(host);

  if (!candles.length) {
    const empty = document.createElement('div');
    empty.className = 'chart-empty';
    empty.textContent = 'No candles to draw. Reload the files or fetch live data.';
    host.appendChild(empty);
    return;
  }

  const width = 640;
  const height = 190;
  const padX = 44;
  const padY = 20;
  const highs = candles.map((candle) => Number(candle.high));
  const lows = candles.map((candle) => Number(candle.low));
  const max = Math.max.apply(null, highs);
  const min = Math.min.apply(null, lows);
  const span = (max - min) || Math.max(1, Math.abs(max) * 0.001);
  const plotWidth = width - padX * 2;
  const plotHeight = height - padY * 2 - 16;
  const slot = plotWidth / candles.length;
  const bodyWidth = Math.max(8, Math.min(46, slot * 0.52));

  const y = (value) => padY + ((max - value) / span) * plotHeight;

  const ns = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('viewBox', '0 0 ' + width + ' ' + height);
  svg.setAttribute('preserveAspectRatio', 'none');
  svg.setAttribute('role', 'img');
  svg.setAttribute('aria-label', 'last five candles');

  [max, (max + min) / 2, min].forEach((value) => {
    const line = document.createElementNS(ns, 'line');
    line.setAttribute('x1', String(padX - 6));
    line.setAttribute('x2', String(width - padX + 6));
    line.setAttribute('y1', String(y(value)));
    line.setAttribute('y2', String(y(value)));
    line.setAttribute('class', 'chart-grid');
    svg.appendChild(line);

    const label = document.createElementNS(ns, 'text');
    label.setAttribute('x', '4');
    label.setAttribute('y', String(y(value) + 4));
    label.setAttribute('class', 'chart-axis');
    label.textContent = num(value, 2);
    svg.appendChild(label);
  });

  candles.forEach((candle, index) => {
    const x = padX + slot * (index + 0.5);
    const bullish = Number(candle.close) >= Number(candle.open);
    const className = bullish ? 'chart-bull' : 'chart-bear';

    const wick = document.createElementNS(ns, 'line');
    wick.setAttribute('x1', String(x));
    wick.setAttribute('x2', String(x));
    wick.setAttribute('y1', String(y(Number(candle.high))));
    wick.setAttribute('y2', String(y(Number(candle.low))));
    wick.setAttribute('class', 'chart-wick ' + className);
    svg.appendChild(wick);

    const top = y(Math.max(Number(candle.open), Number(candle.close)));
    const bottom = y(Math.min(Number(candle.open), Number(candle.close)));
    const body = document.createElementNS(ns, 'rect');
    body.setAttribute('x', String(x - bodyWidth / 2));
    body.setAttribute('y', String(top));
    body.setAttribute('width', String(bodyWidth));
    body.setAttribute('height', String(Math.max(2, bottom - top)));
    body.setAttribute('rx', '2');
    body.setAttribute('class', 'chart-body ' + className);
    svg.appendChild(body);

    const label = document.createElementNS(ns, 'text');
    label.setAttribute('x', String(x));
    label.setAttribute('y', String(height - 3));
    label.setAttribute('text-anchor', 'middle');
    label.setAttribute('class', 'chart-axis');
    label.textContent = chartLabel(candle.timestamp);
    svg.appendChild(label);
  });

  host.appendChild(svg);
}

function renderCandleTable(candles) {
  const body = $('candles-table').querySelector('tbody');
  clear(body);

  if (!candles.length) {
    const row = document.createElement('tr');
    const cell = document.createElement('td');
    cell.colSpan = 9;
    cell.className = 'empty-cell';
    cell.textContent = 'No candles loaded.';
    row.appendChild(cell);
    body.appendChild(row);
    return;
  }

  candles.forEach((candle, index) => {
    const row = document.createElement('tr');
    const latest = index === candles.length - 1;
    if (latest) row.className = 'latest-row';

    const ordinal = document.createElement('td');
    ordinal.className = 'num';
    ordinal.textContent = latest ? 'latest' : String(index + 1);
    row.appendChild(ordinal);

    const time = document.createElement('td');
    time.textContent = candle.local || formatDateTime(candle.timestamp);
    row.appendChild(time);

    [candle.open, candle.high, candle.low, candle.close].forEach((value, column) => {
      const cell = document.createElement('td');
      cell.className = 'num' + (column === 3 ? (candle.bullish ? ' positive' : ' negative') : '');
      cell.textContent = num(value);
      row.appendChild(cell);
    });

    const change = document.createElement('td');
    change.className = 'num ' + (Number(candle.changePct) >= 0 ? 'positive' : 'negative');
    change.textContent = pct(candle.changePct);
    row.appendChild(change);

    const range = document.createElement('td');
    range.className = 'num';
    range.textContent = num(candle.range);
    row.appendChild(range);

    const volume = document.createElement('td');
    volume.className = 'num muted';
    volume.textContent = num(candle.volume, 0);
    row.appendChild(volume);

    body.appendChild(row);
  });
}

/* ----------------------------------------------------------------- signal */

async function loadSignal() {
  const query = '/api/signal?timeframe=' + encodeURIComponent(state.timeframe) +
    '&source=' + encodeURIComponent(state.source);
  try {
    state.signal = await api(query);
    state.signalError = '';
  } catch (error) {
    state.signalError = error.message;
  }
  renderSignal();
}

function renderSignal() {
  const signal = state.signal;
  const available = !!(signal && signal.available);
  const action = available ? signal.action : 'HOLD';

  const actionNode = $('signal-action');
  actionNode.textContent = available ? action : '—';
  actionNode.className = 'signal-action ' +
    (action === 'BUY' ? 'buy' : (action === 'SELL' ? 'sell' : 'hold'));

  if (!signal) {
    setText('signal-target', '--');
    setText('signal-change', '--');
    setText('signal-edge', '--');
    setText('signal-horizon', '--');
    setText('signal-candle', '--');
    setText('signal-model', state.signalError || 'Waiting for the server.');
    setText('signal-mode', '--');
    $('signal-message').classList.add('hidden');
    $('execute-signal').disabled = true;
    $('execute-signal').textContent = 'Execute signal';
    $('auto-check').disabled = true;
    setText('signal-auto', '');
    return;
  }

  const prediction = signal.prediction || {};
  const execution = signal.execution || {};

  setText('signal-target', available ? num(prediction.targetPrice) : '--');
  setText('signal-change', available
    ? signed(prediction.change) + ' (' + pct(prediction.changePct) + ')'
    : '--');

  const edge = Number(signal.signalStrength);
  setText('signal-edge', available && isFinite(edge)
    ? (edge >= 0 ? '+' : '') + num(edge, 2) + '×'
    : '--');

  setText('signal-horizon', prediction.horizonEndUtc ? formatDateTime(prediction.horizonEndUtc) : '—');
  setText('signal-candle', signal.candle && (signal.candle.local || signal.candle.utc)
    ? (signal.candle.local || signal.candle.utc)
    : '—');

  setText('signal-model', signal.modelName
    ? signal.modelName + ' · ' + formatAge(signal.candleAgeSeconds) + ' old candle'
    : 'no model resolved for this timeframe');

  const mode = execution.mode === 'auto'
    ? (execution.armed ? 'auto · armed' : 'auto · unarmed')
    : 'manual';
  setText('signal-mode', mode + ' · ' + (execution.paperTrading === false ? 'LIVE' : 'paper'));
  $('signal-mode').className = 'badge ' +
    (execution.paperTrading === false ? 'sell' : (execution.mode === 'auto' && execution.armed ? 'hold' : ''));

  const message = $('signal-message');
  let messageText = '';
  let messageKind = '';
  if (state.signalError) {
    messageText = state.signalError;
    messageKind = 'error';
  } else if (signal.error) {
    messageText = signal.error;
    messageKind = 'error';
  } else if (signal.stale) {
    messageText = 'Stale candle (' + formatAge(signal.candleAgeSeconds) +
      ' old). The automatic trader will not execute a stale signal; fetch live candles first.';
    messageKind = 'warn';
  } else if (signal.warning) {
    messageText = signal.warning;
    messageKind = 'warn';
  }
  message.textContent = messageText;
  message.className = 'notice' + (messageKind ? ' ' + messageKind : '') +
    (messageText ? '' : ' hidden');

  const canExecute = available && action !== 'HOLD' && !signal.stale;
  $('execute-signal').disabled = !canExecute;
  $('execute-signal').textContent = available && action !== 'HOLD'
    ? (signal.stale ? 'Signal is stale' : 'Execute ' + action)
    : (available ? 'No actionable signal' : 'Execute signal');

  const autoMode = execution.mode === 'auto';
  $('auto-check').disabled = !autoMode;

  let autoText;
  if (!autoMode) {
    autoText = 'Manual mode: use the execute button or the order ticket below.';
  } else if (!execution.armed) {
    autoText = 'Automatic mode is on, but the trader is not armed.';
  } else if (execution.autoWouldExecute) {
    autoText = 'The server side auto trader would execute this candle on its next cycle.';
  } else {
    autoText = 'Auto trader armed; waiting for a fresh, actionable signal.';
  }
  setText('signal-auto', autoText);
}

/* ---------------------------------------------------------------- settings */

function populateSettings() {
  const settings = state.settings || {};

  $('instrument').value = settings.instrument || 'xauusd';
  $('exec-mode').value = settings.executionMode || 'manual';
  $('auto-armed').checked = !!settings.autoArmed;
  $('paper-trading').checked = settings.paperTrading !== false;
  $('live-confirm').checked = false;

  $('volume-lots').value = settings.volumeLots === undefined ? '0.10' : settings.volumeLots;
  $('estimated-spread').value = settings.estimatedSpread === undefined ? '0.40' : settings.estimatedSpread;
  $('stop-loss').value = settings.stopLossDistance === undefined ? '0' : settings.stopLossDistance;
  $('take-profit').value = settings.takeProfitDistance === undefined ? '0' : settings.takeProfitDistance;
  $('poll-seconds').value = settings.pollSeconds === undefined ? '15' : settings.pollSeconds;
  $('availability-delay').value = settings.availabilityDelaySeconds === undefined
    ? '60' : settings.availabilityDelaySeconds;

  $('provider').value = settings.provider || 'dukascopy';
  $('data-source').value = settings.dataSource || 'file';

  $('alpha-key').value = '';
  $('alpha-key').placeholder = settings.alphaVantageApiKeySet
    ? 'configured — leave blank to keep'
    : 'paste an Alpha Vantage key';

  const autoTimeframes = settings.autoTimeframes || [];
  document.querySelectorAll('[data-auto-tf]').forEach((input) => {
    input.checked = autoTimeframes.indexOf(input.dataset.autoTf) !== -1;
  });

  $('ticket-volume').value = settings.volumeLots === undefined ? '0.10' : settings.volumeLots;
  $('ticket-hint').textContent = 'Timeframe: ' + timeframeLabel(state.timeframe);

  updateExecutionUi();
}

function updateExecutionUi() {
  const mode = $('exec-mode').value;
  const paper = $('paper-trading').checked;
  const armed = $('auto-armed').checked;

  $('auto-armed').disabled = mode !== 'auto';
  $('live-confirm-row').classList.toggle('hidden', paper);
  $('live-confirm').disabled = paper;

  const settingsMode = mode === 'auto'
    ? (armed ? 'auto · armed' : 'auto · unarmed')
    : 'manual';
  setText('settings-mode', settingsMode);
  $('settings-mode').className = 'badge ' + (mode === 'auto' && armed ? 'hold' : '');
}

function collectSettingsPatch() {
  const paper = $('paper-trading').checked;
  const confirmed = $('live-confirm').checked;

  if (!paper && !confirmed) {
    throw new Error('Tick the live confirmation before switching paper trading off.');
  }

  const patch = {
    instrument: $('instrument').value.trim(),
    executionMode: $('exec-mode').value,
    autoArmed: $('auto-armed').checked,
    paperTrading: paper,
    volumeLots: Number($('volume-lots').value),
    estimatedSpread: Number($('estimated-spread').value),
    stopLossDistance: Number($('stop-loss').value) || 0,
    takeProfitDistance: Number($('take-profit').value) || 0,
    pollSeconds: Number($('poll-seconds').value),
    availabilityDelaySeconds: Number($('availability-delay').value),
    provider: $('provider').value,
    dataSource: $('data-source').value,
    autoTimeframes: Array.from(document.querySelectorAll('[data-auto-tf]:checked'))
      .map((input) => input.dataset.autoTf),
  };

  if (!paper) patch.confirmLive = true;

  const key = $('alpha-key').value.trim();
  if (key) patch.alphaVantageApiKey = key;

  return patch;
}

async function saveSettings() {
  const button = $('save-settings');
  button.disabled = true;
  button.textContent = 'Saving…';
  state.busy = true;
  try {
    const patch = collectSettingsPatch();
    const payload = await api('/api/settings', {
      method: 'PUT',
      body: JSON.stringify(patch),
    });
    state.settings = payload.settings || state.settings;
    state.providers = payload.providers || state.providers || {};
    state.source = state.settings.dataSource || 'file';

    populateSettings();
    renderProviderReadiness();
    renderPills();
    renderHero();
    startPolling();

    toast('Settings saved. Default source is now ' + state.source + '.', 'success');
    await Promise.allSettled([loadStatus(), loadSignal(), loadCandles()]);
  } catch (error) {
    toast(error.message, 'error');
  } finally {
    state.busy = false;
    button.disabled = false;
    button.textContent = 'Save settings';
  }
}

async function testProvider() {
  const button = $('test-provider');
  button.disabled = true;
  button.textContent = 'Testing…';
  try {
    const payload = await api('/api/provider/test', {
      method: 'POST',
      body: JSON.stringify({ timeframe: state.timeframe }),
    });
    const candles = payload.candles || [];
    const last = candles.length ? candles[candles.length - 1] : null;
    toast(payload.provider + ' returned ' + payload.count + ' candle(s)' +
      (last ? ' · last close ' + num(last.close) : ''), 'success');
  } catch (error) {
    toast(error.message, 'error');
  } finally {
    button.disabled = false;
    button.textContent = 'Test provider';
  }
}

/* ----------------------------------------------------------------- orders */

async function loadOrders() {
  try {
    state.orders = await api('/api/orders?limit=50');
    state.ordersError = '';
  } catch (error) {
    state.ordersError = error.message;
  }
  renderOrders();
}

function statusClass(status) {
  if (status === 'filled' || status === 'paper') return 'positive';
  if (status === 'rejected' || status === 'error') return 'negative';
  return '';
}

function renderOrders() {
  const payload = state.orders;
  const orders = (payload && payload.orders) || [];
  const body = $('orders-table').querySelector('tbody');
  clear(body);

  setText('orders-count', payload ? String(payload.total) : '0');
  setText('orders-path', payload ? payload.ordersFile : 'state/orders.csv');

  if (state.ordersError) {
    const row = document.createElement('tr');
    const cell = document.createElement('td');
    cell.colSpan = 10;
    cell.className = 'empty-cell negative';
    cell.textContent = state.ordersError;
    row.appendChild(cell);
    body.appendChild(row);
    $('orders-empty').classList.add('hidden');
    return;
  }

  $('orders-empty').classList.toggle('hidden', orders.length > 0);

  orders.forEach((order) => {
    const row = document.createElement('tr');

    const time = document.createElement('td');
    time.textContent = formatDateTime(order.utc || order.timestamp);
    time.title = order.utc || '';
    row.appendChild(time);

    const kind = document.createElement('td');
    const kindBadge = document.createElement('span');
    kindBadge.className = 'badge ' + (order.automatic ? 'hold' : '');
    kindBadge.textContent = order.automatic ? 'auto' : 'manual';
    kind.appendChild(kindBadge);
    row.appendChild(kind);

    const timeframe = document.createElement('td');
    timeframe.textContent = timeframeLabel(order.timeframe);
    row.appendChild(timeframe);

    const side = document.createElement('td');
    side.className = order.side === 'BUY' ? 'positive' : 'negative';
    side.textContent = order.side || '—';
    row.appendChild(side);

    const volume = document.createElement('td');
    volume.className = 'num';
    volume.textContent = num(order.volumeLots, 2);
    row.appendChild(volume);

    const price = document.createElement('td');
    price.className = 'num';
    price.textContent = num(order.referencePrice);
    row.appendChild(price);

    const sl = document.createElement('td');
    sl.className = 'num muted';
    sl.textContent = Number(order.stopLoss) > 0 ? num(order.stopLoss) : '—';
    row.appendChild(sl);

    const tp = document.createElement('td');
    tp.className = 'num muted';
    tp.textContent = Number(order.takeProfit) > 0 ? num(order.takeProfit) : '—';
    row.appendChild(tp);

    const status = document.createElement('td');
    const statusBadge = document.createElement('span');
    statusBadge.className = 'badge ' + statusClass(order.status);
    statusBadge.textContent = order.status || '—';
    status.appendChild(statusBadge);
    row.appendChild(status);

    const message = document.createElement('td');
    message.className = 'muted';
    message.textContent = order.message || (order.ticket ? 'ticket ' + order.ticket : '—');
    message.title = order.message || '';
    row.appendChild(message);

    body.appendChild(row);
  });
}

/* ---------------------------------------------------------------- actions */

async function selectTimeframe(id) {
  if (!TIMEFRAMES.includes(id) || id === state.timeframe) return;
  state.timeframe = id;
  localStorage.setItem('golddigger.timeframe', id);

  state.candles = null;
  state.signal = null;
  state.candlesError = '';
  state.signalError = '';

  $('ticket-hint').textContent = 'Timeframe: ' + timeframeLabel(id);
  renderTimeframes();
  renderHero();
  renderCandles();
  renderSignal();

  await Promise.allSettled([loadStatus(), loadCandles(), loadSignal()]);
}

async function reloadCandles() {
  const button = $('reload-candles');
  button.disabled = true;
  button.textContent = 'Loading…';
  try {
    await Promise.allSettled([loadStatus(), loadCandles(), loadSignal()]);
    toast('Candle files reloaded.', 'success');
  } finally {
    button.disabled = false;
    button.textContent = 'Reload';
  }
}

async function fetchLive() {
  const button = $('fetch-live');
  button.disabled = true;
  button.textContent = 'Fetching…';
  state.busy = true;
  try {
    const payload = await api('/api/candles/refresh', {
      method: 'POST',
      body: JSON.stringify({ timeframe: state.timeframe, count: 5, persist: true }),
    });

    state.candles = {
      timeframe: payload.timeframe,
      source: 'live',
      dataPath: payload.path,
      count: (payload.candles || []).length,
      warning: '',
      candles: payload.candles || [],
    };
    state.candlesError = '';
    renderCandles();

    await Promise.allSettled([loadSignal(), loadStatus()]);
    toast('Fetched ' + payload.fetched + ' candle(s): ' + payload.added + ' new, ' +
      payload.replaced + ' replaced, ' + payload.total + ' rows in the file.', 'success');
  } catch (error) {
    toast(error.message, 'error');
  } finally {
    state.busy = false;
    button.disabled = false;
    button.textContent = 'Fetch live';
  }
}

async function executeSignal() {
  const signal = state.signal;
  if (!signal || !signal.available || signal.action === 'HOLD') return;

  if (signal.stale) {
    toast('The signal is stale; fetch live candles before executing.', 'error');
    return;
  }

  const button = $('execute-signal');
  button.disabled = true;
  state.busy = true;
  try {
    const payload = await api('/api/orders', {
      method: 'POST',
      body: JSON.stringify({
        timeframe: state.timeframe,
        side: signal.action,
        volumeLots: Number(state.settings.volumeLots),
        price: 0,
        comment: 'signal ' + signal.action + ' from GolddiggerUI',
      }),
    });
    const order = payload.order || {};
    toast('Order ' + (order.status || 'placed') + ': ' + signal.action + ' ' +
      num(order.volumeLots, 2) + ' lots at ' + num(order.referencePrice), 'success');
    await Promise.allSettled([loadOrders(), loadSignal(), loadStatus()]);
  } catch (error) {
    toast(error.message, 'error');
  } finally {
    state.busy = false;
    button.disabled = false;
  }
}

async function runAutoCheck() {
  const button = $('auto-check');
  button.disabled = true;
  button.textContent = 'Checking…';
  state.busy = true;
  try {
    const payload = await api('/api/signal/evaluate', {
      method: 'POST',
      body: JSON.stringify({ timeframe: state.timeframe, execute: true }),
    });

    if (payload.signal) {
      state.signal = payload.signal;
      state.signalError = '';
      renderSignal();
    }

    const execution = payload.execution || {};
    if (execution.executed) {
      const order = execution.order || {};
      toast('Automatic order ' + (order.status || 'placed') + ': ' + (order.side || '') + ' ' +
        num(order.volumeLots, 2) + ' lots.', 'success');
    } else {
      toast(execution.reason || 'No automatic order was placed.', 'info');
    }

    await Promise.allSettled([loadOrders(), loadStatus()]);
  } catch (error) {
    toast(error.message, 'error');
  } finally {
    state.busy = false;
    button.disabled = false;
    button.textContent = 'Run auto check';
  }
}

async function submitTicket(event) {
  event.preventDefault();
  const volume = Number($('ticket-volume').value);
  if (!isFinite(volume) || volume <= 0) {
    toast('Enter a volume greater than zero.', 'error');
    return;
  }

  const price = Number($('ticket-price').value) || 0;
  const stopLoss = Number($('ticket-sl').value) || 0;
  const takeProfit = Number($('ticket-tp').value) || 0;
  const comment = $('ticket-comment').value.trim() || 'manual order from GolddiggerUI';

  const submit = $('ticket-form').querySelector('button[type="submit"]');
  submit.disabled = true;
  state.busy = true;
  try {
    const payload = await api('/api/orders', {
      method: 'POST',
      body: JSON.stringify({
        timeframe: state.timeframe,
        side: $('ticket-side').value,
        volumeLots: volume,
        price: price,
        stopLoss: stopLoss,
        takeProfit: takeProfit,
        comment: comment,
      }),
    });
    const order = payload.order || {};
    toast('Manual order ' + (order.status || 'placed') + ' at ' + num(order.referencePrice) +
      (order.message ? ' · ' + order.message : ''), 'success');
    $('ticket-price').value = '';
    $('ticket-sl').value = '';
    $('ticket-tp').value = '';
    $('ticket-comment').value = '';
    await Promise.allSettled([loadOrders(), loadStatus()]);
  } catch (error) {
    toast(error.message, 'error');
  } finally {
    state.busy = false;
    submit.disabled = false;
  }
}

/* ---------------------------------------------------------------- polling */

function startPolling() {
  if (state.timer) clearInterval(state.timer);
  const seconds = Number(state.settings.pollSeconds);
  state.pollMs = Math.max(5000, (isFinite(seconds) && seconds > 0 ? seconds : 15) * 1000);
  state.timer = setInterval(poll, state.pollMs);
}

async function poll() {
  if (document.hidden || state.busy || !state.status) return;
  try {
    await loadStatus();
    const jobs = [loadSignal(), loadOrders()];
    if (state.source === 'file') jobs.push(loadCandles());
    await Promise.allSettled(jobs);
  } catch (error) {
    console.warn('GolddiggerUI poll failed:', error);
  }
}

/* ------------------------------------------------------------------- boot */

async function boot() {
  const saved = localStorage.getItem('golddigger.timeframe');
  if (TIMEFRAMES.includes(saved)) state.timeframe = saved;

  try {
    await loadStatus();
    await loadSettings();
    state.source = state.settings.dataSource || 'file';
    $('candle-source').value = state.source;

    const summary = currentSummary();
    if (summary && summary.label) {
      $('ticket-hint').textContent = 'Timeframe: ' + summary.label;
    }

    await Promise.allSettled([loadCandles(), loadSignal(), loadOrders()]);
    startPolling();
  } catch (error) {
    toast(error.message, 'error');
  }
}

function setup() {
  $('timeframe-switch').addEventListener('click', (event) => {
    const button = event.target.closest('button[data-timeframe]');
    if (button) selectTimeframe(button.dataset.timeframe);
  });

  $('candle-source').addEventListener('change', async (event) => {
    state.source = event.target.value;
    state.candles = null;
    await Promise.allSettled([loadCandles(), loadSignal()]);
  });

  $('reload-candles').addEventListener('click', reloadCandles);
  $('fetch-live').addEventListener('click', fetchLive);

  $('execute-signal').addEventListener('click', executeSignal);
  $('auto-check').addEventListener('click', runAutoCheck);

  $('exec-mode').addEventListener('change', updateExecutionUi);
  $('paper-trading').addEventListener('change', updateExecutionUi);
  $('auto-armed').addEventListener('change', updateExecutionUi);
  $('save-settings').addEventListener('click', saveSettings);
  $('test-provider').addEventListener('click', testProvider);

  $('ticket-form').addEventListener('submit', submitTicket);
  $('reload-orders').addEventListener('click', loadOrders);

  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && !state.busy) poll();
  });

  boot();
}

document.addEventListener('DOMContentLoaded', setup);
