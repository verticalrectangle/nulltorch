/* NullTorch results explorer — vanilla JS, no dependencies.
   Reads ./results.json, renders leaderboard, gauntlet, race, gap,
   cost, compare, model drawer and provenance. */
(function () {
'use strict';

/* ================= constants ================= */
var TIERS = ['T1', 'T2', 'T3', 'T4', 'T5', 'T6'];
var CORR_TIERS = ['T1', 'T2', 'T3', 'T4', 'T5'];
var LANGS = ['go', 'rust', 'cpp'];
var CONDS = ['open_book', 'closed_book', 'delta'];
var LANG_LABEL = { go: 'Go', rust: 'Rust', cpp: 'C++' };
var COND_LABEL = { open_book: 'Open book', closed_book: 'Closed book', delta: 'Delta' };
var COND_SHORT = { open_book: 'open', closed_book: 'closed', delta: 'delta' };
var COND_COLOR = { open_book: 'var(--cat2)', closed_book: 'var(--cat3)', delta: 'var(--cat4)' };
var TIER_INFO = {
  T1: { name: 'Flat tensors', desc: 'Single flat f16/f32 state dicts — the on-ramp.' },
  T2: { name: 'Containers', desc: 'Nested dicts, ordered metadata, sequences of tensors.' },
  T3: { name: 'Views & aliasing', desc: 'Shared storage, offsets, transposes, zero-dim scalars.' },
  T4: { name: 'Formats & dtypes', desc: 'Wide dtypes, zip64, re-zipped archives, legacy protocols.' },
  T5: { name: 'Resource envelope', desc: 'Huge tensors under a memory ceiling — stream, don’t slurp.' },
  T6: { name: 'Adversarial', desc: 'Pickle bombs, exec opcodes, truncation, cycles. Survive without executing anything.' },
  RVC: { name: 'RVC fidelity', desc: 'Engine sidetrack: config extraction + f16→f32 against a real converter’s contract. Graded separately — not a difficulty tier.' }
};
var RAMP = {
  light: { fills: ['#cde2fb', '#9ec5f4', '#6da7ec', '#256abf', '#184f95', '#0d366b'],
           inks:  ['#0d366b', '#0d366b', '#0d366b', '#ffffff', '#ffffff', '#ffffff'] },
  dark:  { fills: ['#16273f', '#1c3a63', '#1c5cab', '#2470c9', '#6da7ec', '#9ec5f4'],
           inks:  ['#bcd4f2', '#cfe0f6', '#ffffff', '#ffffff', '#0d2440', '#0d2440'] }
};
var MAX_HL = 5;
var SVGNS = 'http://www.w3.org/2000/svg';

/* ================= tiny dom helpers ================= */
function $(id) { return document.getElementById(id); }
function el(tag, attrs) {
  var n = document.createElement(tag);
  applyAttrs(n, attrs);
  for (var i = 2; i < arguments.length; i++) appendKid(n, arguments[i]);
  return n;
}
function svg(tag, attrs) {
  var n = document.createElementNS(SVGNS, tag);
  applyAttrs(n, attrs);
  for (var i = 2; i < arguments.length; i++) appendKid(n, arguments[i]);
  return n;
}
function applyAttrs(n, attrs) {
  if (!attrs) return;
  Object.keys(attrs).forEach(function (k) {
    var v = attrs[k];
    if (v === null || v === undefined || v === false) return;
    if (k === 'class') { n.setAttribute('class', v); }
    else if (k === 'text') { n.textContent = v; }
    else if (k === 'style') { n.style.cssText = v; }
    else if (k.indexOf('on') === 0) { n.addEventListener(k.slice(2), v); }
    else { n.setAttribute(k, v === true ? '' : String(v)); }
  });
}
function appendKid(n, kid) {
  if (kid === null || kid === undefined || kid === false) return;
  if (Array.isArray(kid)) { kid.forEach(function (k) { appendKid(n, k); }); return; }
  n.appendChild(kid.nodeType ? kid : document.createTextNode(String(kid)));
}
function clearNode(n) { while (n.firstChild) n.removeChild(n.firstChild); }

/* ================= formatting ================= */
function fmtInt(n) { return (typeof n === 'number' && isFinite(n)) ? n.toLocaleString('en-US') : '—'; }
function fmtPct(r) { return r === null || r === undefined ? '—' : Math.round(r * 100) + '%'; }
function fmtPP(x) {
  if (x === null || x === undefined) return '—';
  var v = Math.abs(x * 100).toFixed(1);
  return (x < 0 ? '−' : '+') + v + ' pp';
}
function frac(p, t) { return fmtInt(p) + '/' + fmtInt(t); }
function fmtTok(n) {
  if (n === null || n === undefined || !isFinite(n)) return '—';
  if (n >= 1e6) return (n / 1e6).toFixed(1).replace(/\.0$/, '') + 'M';
  if (n >= 1e3) return (n / 1e3).toFixed(1).replace(/\.0$/, '') + 'k';
  return String(n);
}
function fmtDur(s) {
  if (s === null || s === undefined || !isFinite(s)) return '—';
  s = Math.round(s);
  if (s < 60) return s + 's';
  if (s < 3600) { var m = Math.floor(s / 60), r = s % 60; return r ? m + 'm ' + r + 's' : m + 'm'; }
  var h = Math.floor(s / 3600), mm = Math.round((s % 3600) / 60);
  return mm ? h + 'h ' + mm + 'm' : h + 'h';
}
function fmtDurTick(v, max) {
  if (max >= 5400) return (v / 3600).toFixed(v % 3600 === 0 ? 0 : 1) + 'h';
  if (max >= 120) return Math.round(v / 60) + 'm';
  return Math.round(v) + 's';
}
function fmtBytes(b) {
  if (b === null || b === undefined || !isFinite(b) || b <= 0) return '—';
  if (b >= 1073741824) return (b / 1073741824).toFixed(1) + ' GiB';
  if (b >= 1048576) return (b / 1048576).toFixed(0) + ' MiB';
  return Math.round(b / 1024) + ' KiB';
}

/* ================= state ================= */
var state = {
  raw: null, field: null, loadError: null,
  lang: null, cond: null,
  sort: { key: 'correctness', dir: 'desc' },
  hl: new Map(),
  cmpA: null, cmpB: null,
  costMetric: 'tokens',
  raceFrac: 1,
  drawerModel: null, ddSel: null,
  lastW: window.innerWidth
};

function theme() { return document.documentElement.getAttribute('data-theme') === 'light' ? 'light' : 'dark'; }

/* ================= data ================= */
function num(v) { return typeof v === 'number' && isFinite(v) ? v : 0; }
function rate(p, t) { return t > 0 ? p / t : null; }
function sumT(run, keys) {
  var p = 0, t = 0;
  keys.forEach(function (k) {
    var x = run.tiers && run.tiers[k];
    if (x) { p += num(x.pass); t += num(x.total); }
  });
  return { pass: p, total: t, r: rate(p, t) };
}
function rvcOf(run) {
  var x = run.tiers && run.tiers.RVC;
  if (!x) return null;
  return { pass: num(x.pass), total: num(x.total), r: rate(num(x.pass), num(x.total)) };
}
function deriveField(data) {
  var runs = (data && Array.isArray(data.runs) ? data.runs : []).filter(function (r) {
    return r && typeof r === 'object' && r.model_id && r.tiers;
  });
  var models = [];
  runs.forEach(function (r) { if (models.indexOf(r.model_id) < 0) models.push(r.model_id); });
  models.sort();
  var langs = LANGS.filter(function (l) { return runs.some(function (r) { return r.language === l; }); });
  var conds = CONDS.filter(function (c) { return runs.some(function (r) { return r.condition === c; }); });
  var map = new Map();
  runs.forEach(function (r) { map.set(r.model_id + '|' + r.language + '|' + r.condition, r); });
  var dq = new Map();
  models.forEach(function (m) {
    dq.set(m, runs.some(function (r) {
      return r.model_id === m && r.t6_incidents && num(r.t6_incidents.exec_attempts) > 0;
    }));
  });
  return {
    runs: runs, models: models, langs: langs, conds: conds, map: map, dq: dq,
    hasRVC: runs.some(function (r) { return !!(r.tiers && r.tiers.RVC); }),
    hasDelta: conds.indexOf('delta') >= 0,
    totalExec: runs.reduce(function (a, r) { return a + num(r.t6_incidents && r.t6_incidents.exec_attempts); }, 0),
    totalDet: runs.reduce(function (a, r) { return a + num(r.determinism_violations); }, 0),
    totalFixtures: runs.reduce(function (a, r) {
      var s = sumT(r, TIERS).total; var v = rvcOf(r); return a + s + (v ? v.total : 0);
    }, 0)
  };
}
function runAt(m, l, c) { return state.field.map.get(m + '|' + l + '|' + c) || null; }
function stockRun(m, l) { return runAt(m, l, 'open_book') || runAt(m, l, 'closed_book'); }
function stockCondOf(m, l) { return runAt(m, l, 'open_book') ? 'open_book' : (runAt(m, l, 'closed_book') ? 'closed_book' : null); }
function gapFor(m, l) {
  var s = stockRun(m, l), d = runAt(m, l, 'delta');
  if (!s || !d) return null;
  var sr = sumT(s, TIERS), dr = sumT(d, TIERS);
  if (sr.r === null || dr.r === null) return null;
  return { stock: sr, delta: dr, stockCond: stockCondOf(m, l), gap: sr.r - dr.r };
}

/* leaderboard row */
function buildRow(m, lang, cond) {
  var run = runAt(m, lang, cond);
  var isDq = !!state.field.dq.get(m);
  if (!run) return { model: m, run: null, dq: isDq, group: 2, corr: null, rob: null, rvc: null };
  return {
    model: m, run: run, dq: isDq, group: isDq ? 1 : 0,
    corr: sumT(run, CORR_TIERS), rob: sumT(run, ['T6']), rvc: rvcOf(run)
  };
}
function sortVal(row, key) {
  if (key === 'model') return row.model;
  if (!row.run) return null;
  if (key === 'correctness') return row.corr.r;
  if (key === 'robustness') return row.rob.r;
  if (key === 'RVC') return row.rvc ? row.rvc.r : null;
  var x = row.run.tiers && row.run.tiers[key];
  return x && num(x.total) > 0 ? num(x.pass) / num(x.total) : null;
}
function cmpRows(a, b) {
  if (a.group !== b.group) return a.group - b.group;
  var k = state.sort, va = sortVal(a, k.key), vb = sortVal(b, k.key);
  var d = 0;
  if (va === null && vb === null) d = 0;
  else if (va === null) return 1;
  else if (vb === null) return -1;
  else if (typeof va === 'string') d = va.localeCompare(vb) * (k.dir === 'desc' ? -1 : 1);
  else d = (va - vb) * (k.dir === 'desc' ? -1 : 1);
  if (d !== 0) return d;
  var ca = a.corr ? a.corr.r : null, cb = b.corr ? b.corr.r : null;
  if (ca !== cb) return (cb || 0) - (ca || 0);
  var ra = a.rob ? a.rob.r : null, rb = b.rob ? b.rob.r : null;
  if (ra !== rb) return (rb || 0) - (ra || 0);
  return a.model.localeCompare(b.model);
}
function sliceRows() {
  return state.field.models.map(function (m) { return buildRow(m, state.lang, state.cond); });
}
function defaultRank(rows) {
  return rows.slice().sort(function (a, b) {
    if (a.group !== b.group) return a.group - b.group;
    var ca = a.corr ? a.corr.r : -1, cb = b.corr ? b.corr.r : -1;
    if (ca !== cb) return cb - ca;
    var ra = a.rob ? a.rob.r : -1, rb = b.rob ? b.rob.r : -1;
    if (ra !== rb) return rb - ra;
    return a.model.localeCompare(b.model);
  });
}
function leadersOf(rows) {
  var ranked = defaultRank(rows);
  var q = ranked.filter(function (r) { return r.group === 0 && r.corr && r.corr.r !== null; });
  var allDq = !q.length;
  if (allDq) q = ranked.filter(function (r) { return r.group === 1 && r.corr && r.corr.r !== null; });
  if (!q.length) return { list: [], allDq: false };
  var top = q[0];
  return {
    allDq: allDq,
    list: q.filter(function (r) {
      return r.corr.r === top.corr.r && (r.rob ? r.rob.r : null) === (top.rob ? top.rob.r : null);
    })
  };
}

/* highlights */
function hlColor(m) {
  var s = state.hl.get(m);
  return s === undefined ? null : 'var(--cat' + (s + 1) + ')';
}
function toggleHl(m) {
  var fid = activeFid();
  if (state.hl.has(m)) { state.hl.delete(m); }
  else {
    var used = new Set(state.hl.values()), slot = -1;
    for (var i = 0; i < MAX_HL; i++) if (!used.has(i)) { slot = i; break; }
    if (slot < 0) { toast('Up to ' + MAX_HL + ' models can be highlighted at once'); return; }
    state.hl.set(m, slot);
  }
  renderAll();
  if (state.drawerModel) renderDrawerBody();
  focusFid(fid);
}
function effectiveHl() {
  if (state.hl.size) {
    var out = new Map();
    state.hl.forEach(function (slot, m) { out.set(m, 'var(--cat' + (slot + 1) + ')'); });
    return { map: out, auto: false };
  }
  var leaders = leadersOf(sliceRows());
  var out2 = new Map();
  if (leaders.list.length) out2.set(leaders.list[0].model, 'var(--cat1)');
  return { map: out2, auto: true };
}

/* ================= tooltip / toast / announce ================= */
var tipEl = null, toastTimer = null;
function tipShowAt(x, y, node) {
  clearNode(tipEl);
  tipEl.appendChild(node);
  tipEl.hidden = false;
  var r = tipEl.getBoundingClientRect();
  var px = Math.min(x + 14, window.innerWidth - r.width - 8);
  var py = Math.min(y + 14, window.innerHeight - r.height - 8);
  tipEl.style.left = Math.max(4, px) + 'px';
  tipEl.style.top = Math.max(4, py) + 'px';
}
function tipHide() { tipEl.hidden = true; }
function attachTip(target, build) {
  function onMove(e) { tipShowAt(e.clientX, e.clientY, build()); }
  target.addEventListener('pointerenter', onMove);
  target.addEventListener('pointermove', onMove);
  target.addEventListener('pointerleave', tipHide);
  target.addEventListener('focus', function () {
    var r = target.getBoundingClientRect();
    tipShowAt(r.left + r.width / 2, r.bottom + 2, build());
  });
  target.addEventListener('blur', tipHide);
}
function tipTitle(t) { return el('div', { class: 'tt-title', text: t }); }
function tipRow(color, value, name) {
  var key = el('span', { class: 'tt-key' });
  if (color) key.style.setProperty('--c', color);
  return el('div', { class: 'tt-row' }, key,
    el('span', { class: 'tt-val', text: value }), el('span', { class: 'tt-name', text: name }));
}
function tipDim(t) { return el('div', { class: 'tt-row tt-dim', text: t }); }
function toast(msg) {
  var t = $('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(function () { t.classList.remove('show'); }, 1700);
}
function announce(msg) { $('announcer').textContent = msg; }
/* re-renders replace DOM nodes; give repeat controls a stable id so focus survives */
function focusFid(fid) {
  if (!fid) return;
  var safe = fid.replace(/["\\]/g, '\\$&');
  var n = document.querySelector('[data-fid="' + safe + '"]');
  if (n) n.focus();
}
function activeFid() {
  var a = document.activeElement;
  return a && a.getAttribute ? a.getAttribute('data-fid') : null;
}

/* ================= chart scaffolding ================= */
function linScale(d0, d1, r0, r1) {
  if (d1 === d0) return function () { return (r0 + r1) / 2; };
  var k = (r1 - r0) / (d1 - d0);
  return function (v) { return r0 + (v - d0) * k; };
}
function niceTicks(min, max, n) {
  if (!(max > min)) return [min];
  var step0 = (max - min) / Math.max(1, n || 5);
  var mag = Math.pow(10, Math.floor(Math.log(step0) / Math.LN10));
  var norm = step0 / mag;
  var step = (norm >= 5 ? 5 : norm >= 2 ? 2 : 1) * mag;
  var out = [], v = Math.ceil(min / step) * step;
  for (; v <= max + 1e-9; v += step) out.push(+v.toFixed(10));
  return out;
}
function hostWidth(host, fallback) {
  var w = host.clientWidth || (host.parentNode && host.parentNode.clientWidth) || 0;
  return Math.max(300, w || fallback || 640);
}
function chartCard(title, note) {
  var card = el('figure', { class: 'card chart-card', style: 'margin:0' });
  var head = el('div', { class: 'chart-head' },
    el('figcaption', { class: 'chart-title', text: title }));
  if (note) head.appendChild(el('p', { class: 'chart-note', text: note }));
  card.appendChild(head);
  return card;
}
function tableTwin(summary, heads, rows) {
  var thead = el('thead', null, el('tr', null, heads.map(function (h) { return el('th', { scope: 'col', text: h }); })));
  var tbody = el('tbody', null, rows.map(function (r) {
    return el('tr', null, r.map(function (c) { return el('td', { text: c }); }));
  }));
  return el('details', { class: 'twin' },
    el('summary', { text: summary }),
    el('div', { class: 'scroll-x' }, el('table', null, thead, tbody)));
}
function legendKey(color, label, dim) {
  var k = el('span', { class: 'key' + (dim ? ' dim' : '') },
    el('span', { class: 'swatch-line' }), label);
  if (color) k.firstChild.style.setProperty('--c', color);
  return k;
}

/* ================= boot ================= */
function boot() {
  tipEl = $('tooltip');
  wireTheme();
  fetch('./results.json', { cache: 'no-store' })
    .then(function (res) {
      if (!res.ok) throw new Error('status ' + res.status);
      return res.json();
    })
    .then(function (data) {
      state.raw = data;
      state.field = deriveField(data);
      initDefaults();
      renderAll();
    })
    .catch(function (err) {
      state.loadError = String(err && err.message ? err.message : err);
      state.field = deriveField(null);
      renderAll();
    });
  window.addEventListener('resize', function () {
    if (Math.abs(window.innerWidth - state.lastW) < 4) return;
    state.lastW = window.innerWidth;
    if (state.field) { renderAll(); if (state.drawerModel) renderDrawerBody(); }
  });
}
function initDefaults() {
  var f = state.field;
  state.lang = f.langs[0] || null;
  state.cond = f.conds.indexOf('open_book') >= 0 ? 'open_book' : (f.conds[0] || null);
  var ranked = defaultRank(sliceRows()).filter(function (r) { return r.run; });
  state.cmpA = ranked[0] ? ranked[0].model : null;
  state.cmpB = ranked[1] ? ranked[1].model : null;
  if (state.raw && state.raw.benchmark_version) {
    var bv = $('brandVer');
    bv.textContent = 'v' + state.raw.benchmark_version;
    bv.hidden = false;
  }
}

/* ================= theme ================= */
function wireTheme() {
  var btn = $('themeToggle');
  function sync() {
    var light = theme() === 'light';
    btn.setAttribute('aria-pressed', String(light));
    btn.setAttribute('aria-label', light ? 'Switch to dark theme' : 'Switch to light theme');
    btn.querySelector('.tt-label').textContent = light ? 'Light' : 'Dark';
  }
  btn.addEventListener('click', function () {
    var next = theme() === 'light' ? 'dark' : 'light';
    document.documentElement.setAttribute('data-theme', next);
    try { localStorage.setItem('nulltorch-theme', next); } catch (e) { /* fine */ }
    sync();
    if (state.field) { renderAll(); if (state.drawerModel) renderDrawerBody(); }
  });
  sync();
}

/* ================= render all ================= */
function renderAll() {
  renderHero();
  renderFilters();
  renderLeaderboard();
  renderGauntlet();
  renderRace();
  renderGap();
  renderCost();
  renderCompare();
  renderRepro();
}
function emptyNote(msg, strong) {
  return el('div', { class: 'empty-note' },
    strong ? el('strong', { text: strong }) : null,
    strong ? el('br') : null, msg);
}

/* ================= hero ================= */
function renderHero() {
  var f = state.field, h1 = $('heroTitle'), sub = $('heroSub'), kpis = $('heroKpis');
  clearNode(kpis);
  if (state.loadError) {
    h1.textContent = 'Couldn’t load results.json';
    sub.textContent = 'This explorer reads results.json from its own folder. Serve the folder over http(s) — for example any local static file server — and reload. No network beyond that is ever used.';
    return;
  }
  var n = f.models.length;
  var langNames = f.langs.map(function (l) { return LANG_LABEL[l]; }).join(', ') || 'Go, Rust and C++';
  if (n === 0) {
    h1.textContent = 'The gauntlet awaits its first challenger.';
    sub.textContent = 'No runs are recorded in this release yet. When they land, this page becomes a full tour: tier-by-tier profiles, self-grade race curves, memorization gaps and hostile-input forensics.';
  } else if (n === 1) {
    h1.textContent = f.models[0] + ' vs. the checkpoint gauntlet.';
  } else {
    h1.textContent = n + ' coding models. One booby-trapped file format.';
  }
  if (n > 0) {
    sub.textContent = 'Each model wrote a PyTorch .pth checkpoint reader from scratch — ' + langNames +
      ', no Python, no torch — then was graded on hidden fixtures across six tiers, from flat tensors to hostile pickles. ' +
      'Correctness is T1–T5. Robustness is surviving T6. Executing a hostile pickle disqualifies.';
  }
  var dqCount = 0;
  f.dq.forEach(function (v) { if (v) dqCount++; });
  var tiles = [
    { label: 'Models', value: n, note: n === 1 ? 'flying solo' : 'in the arena' },
    { label: 'Runs', value: f.runs.length, note: 'model × language × condition' },
    { label: 'Fixture verdicts', value: f.totalFixtures, note: 'hidden-set gradings' },
    { label: 'Hostile-code executions', value: f.totalExec, note: dqCount ? dqCount + ' model' + (dqCount > 1 ? 's' : '') + ' disqualified' : (n ? 'nobody took the bait' : 'no runs yet'), cls: f.totalExec > 0 ? 'crit' : 'good' }
  ];
  if (f.totalDet > 0) tiles.push({ label: 'Determinism violations', value: f.totalDet, note: 'non-identical double conversions', cls: 'crit' });
  tiles.forEach(function (t) {
    var vEl = el('div', { class: 'kpi-value num', text: '0' });
    kpis.appendChild(el('div', { class: 'kpi' + (t.cls ? ' ' + t.cls : '') },
      el('div', { class: 'kpi-label', text: t.label }), vEl,
      el('div', { class: 'kpi-note', text: t.note })));
    countUp(vEl, t.value);
  });
}
function countUp(node, target) {
  var reduced = window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (reduced || !target) { node.textContent = fmtInt(target); return; }
  var t0 = null, dur = 700;
  function step(ts) {
    if (t0 === null) t0 = ts;
    var p = Math.min(1, (ts - t0) / dur);
    node.textContent = fmtInt(Math.round(target * (1 - Math.pow(1 - p, 3))));
    if (p < 1) requestAnimationFrame(step);
  }
  requestAnimationFrame(step);
}

/* ================= filters ================= */
function renderFilters() {
  var f = state.field, bar = $('filterbar');
  if (!f.runs.length) { bar.hidden = true; return; }
  bar.hidden = false;
  var lc = $('langChips'), cc = $('condChips');
  clearNode(lc); clearNode(cc);
  f.langs.forEach(function (l) {
    lc.appendChild(el('button', {
      class: 'chip', type: 'button', 'aria-pressed': String(state.lang === l),
      'data-fid': 'lang:' + l,
      onclick: function () {
        if (state.lang === l) return;
        state.lang = l; renderAll(); focusFid('lang:' + l);
        announce('Language filter: ' + LANG_LABEL[l]);
      }
    }, LANG_LABEL[l]));
  });
  f.conds.forEach(function (c) {
    cc.appendChild(el('button', {
      class: 'chip', type: 'button', 'aria-pressed': String(state.cond === c),
      'data-fid': 'cond:' + c,
      onclick: function () {
        if (state.cond === c) return;
        state.cond = c; renderAll(); focusFid('cond:' + c);
        announce('Condition filter: ' + COND_LABEL[c]);
      }
    }, COND_LABEL[c]));
  });
  var inSlice = sliceRows().filter(function (r) { return r.run; }).length;
  $('sliceNote').textContent = inSlice + ' of ' + f.models.length + ' models ran ' +
    LANG_LABEL[state.lang] + ' · ' + COND_LABEL[state.cond].toLowerCase();
}

/* ================= leaderboard ================= */
var starSeq = 0;
function starBtn(m, ctx) {
  var pressed = state.hl.has(m);
  var b = el('button', {
    class: 'star', type: 'button', 'aria-pressed': String(pressed),
    'data-fid': 'star:' + (ctx || 'lb') + ':' + m,
    'aria-label': (pressed ? 'Remove ' : 'Add ') + m + ' highlight in charts',
    title: 'Highlight in charts',
    onclick: function (e) { e.stopPropagation(); toggleHl(m); }
  }, pressed ? '★' : '☆');
  var c = hlColor(m);
  if (c) b.style.setProperty('--hl', c);
  return b;
}
function badgeSet(row) {
  var out = [], run = row.run, inc = run && run.t6_incidents;
  if (row.dq) out.push(el('span', { class: 'badge dq', title: 'Disqualified: attempted to execute hostile pickle code in at least one run', text: 'DQ' }));
  if (inc && num(inc.crashes) > 0) out.push(el('span', { class: 'badge warn-b', title: 'T6 crashes in this run', text: num(inc.crashes) + '× crash' }));
  if (inc && num(inc.hangs) > 0) out.push(el('span', { class: 'badge warn-b', title: 'T6 watchdog hangs in this run', text: num(inc.hangs) + '× hang' }));
  if (run && num(run.determinism_violations) > 0) out.push(el('span', { class: 'badge det', title: 'Fixtures whose two conversion passes differed', text: 'det ' + num(run.determinism_violations) }));
  return out.length ? el('span', { class: 'badges' }, out) : null;
}
function tierCellContent(row, key) {
  if (!row.run) return el('span', { class: 'cellnum na', title: 'Not run in this slice', text: '—' });
  var isRVC = key === 'RVC';
  var x = isRVC ? (row.run.tiers && row.run.tiers.RVC) : (row.run.tiers && row.run.tiers[key]);
  if (isRVC && !x) return el('span', { class: 'cellnum na', title: 'RVC track not run in this condition', text: 'n/a' });
  if (!x || num(x.total) === 0) return el('span', { class: 'cellnum na', title: 'No ' + key + ' fixtures in this condition', text: '—' });
  var r = num(x.pass) / num(x.total);
  var wrap = el('span', null,
    el('span', { class: 'cellnum', text: frac(num(x.pass), num(x.total)) }),
    el('span', { class: 'microbar', 'aria-hidden': 'true' },
      el('i', { style: 'width:' + (r * 100).toFixed(1) + '%' })));
  wrap.setAttribute('title', key + ': ' + fmtPct(r) + ' — ' + frac(num(x.pass), num(x.total)) + ' hidden fixtures');
  return wrap;
}
function scoreCell(agg) {
  if (!agg || agg.r === null) return el('span', { class: 'cellnum na', text: '—' });
  return el('span', { class: 'score' }, fmtPct(agg.r), ' ',
    el('small', { text: frac(agg.pass, agg.total) }));
}
var SORT_COLS = [
  { key: 'model', label: 'Model' },
  { key: 'T1', label: 'T1' }, { key: 'T2', label: 'T2' }, { key: 'T3', label: 'T3' },
  { key: 'T4', label: 'T4' }, { key: 'T5', label: 'T5' }, { key: 'T6', label: 'T6' },
  { key: 'RVC', label: 'RVC' },
  { key: 'correctness', label: 'Correctness' }, { key: 'robustness', label: 'Robustness' }
];
function setSort(key) {
  var fid = activeFid();
  if (state.sort.key === key) state.sort.dir = state.sort.dir === 'desc' ? 'asc' : 'desc';
  else state.sort = { key: key, dir: key === 'model' ? 'asc' : 'desc' };
  renderLeaderboard();
  focusFid(fid);
  var col = SORT_COLS.filter(function (c) { return c.key === key; })[0];
  announce('Leaderboard sorted by ' + (col ? col.label : key) + ', ' + (state.sort.dir === 'desc' ? 'descending' : 'ascending'));
}
function renderLeaderboard() {
  var body = $('lbBody'), f = state.field;
  clearNode(body);
  $('lbSub').textContent = f.runs.length
    ? 'Hidden-set tier profile at ' + LANG_LABEL[state.lang] + ' · ' + COND_LABEL[state.cond].toLowerCase() +
      '. Every cell is fixtures passed / graded — sort any column, star models to follow them through the charts below.'
    : '';
  if (state.loadError) { body.appendChild(emptyNote('No data loaded.')); return; }
  if (!f.runs.length) { body.appendChild(emptyNote('When runs land in results.json they line up here, tier by tier.', 'No runs in this release yet.')); return; }

  if (!f.hasRVC && state.sort.key === 'RVC') state.sort = { key: 'correctness', dir: 'desc' };
  var rows = sliceRows().sort(cmpRows);
  var leaders = leadersOf(rows);
  if (leaders.list.length) {
    var names = leaders.list.map(function (r) { return r.model; });
    var lead = leaders.list[0];
    var crown = leaders.allDq ? '⚠ No qualified leader — top score: '
      : (leaders.list.length > 1 ? '🔥 ' + leaders.list.length + '-way tie at the top: ' : '🔥 Leader: ');
    body.appendChild(el('p', { class: 'leader-line' },
      el('span', { class: leaders.allDq ? 'crown dq-crown' : 'crown' }, crown),
      el('span', { class: 'mono', text: names.join(', ') }),
      el('span', { text: ' — ' + fmtPct(lead.corr.r) + ' correctness (' + frac(lead.corr.pass, lead.corr.total) + ')' +
        (lead.rob.r !== null ? ', ' + fmtPct(lead.rob.r) + ' robustness (' + frac(lead.rob.pass, lead.rob.total) + ')' : '') +
        (leaders.allDq ? ' — every model in this slice executed hostile code (DQ)' : '') })));
  }

  var cols = SORT_COLS.filter(function (c) { return c.key !== 'RVC' || f.hasRVC; });

  /* mobile sort row */
  var sel = el('select', { id: 'lbSortSel', 'data-fid': 'lbsortsel', onchange: function () { setSort(sel.value); } },
    cols.map(function (c) { return el('option', { value: c.key, text: c.label }); }));
  sel.value = state.sort.key;
  var sortRow = el('div', { class: 'lb-sortrow' },
    el('label', { for: 'lbSortSel', text: 'Sort by' }), sel,
    el('button', {
      class: 'dirbtn', type: 'button', 'data-fid': 'lbdir',
      'aria-label': 'Toggle sort direction, currently ' + (state.sort.dir === 'desc' ? 'descending' : 'ascending'),
      onclick: function () { setSort(state.sort.key); }
    }, state.sort.dir === 'desc' ? '↓ high first' : '↑ low first'));
  body.appendChild(sortRow);

  /* desktop table */
  var thead = el('thead', null, el('tr', null,
    el('th', { class: 'col-rank', scope: 'col' }, el('span', { class: 'visually-hidden', text: 'Rank' }), '#'),
    cols.map(function (c) {
      var isSorted = state.sort.key === c.key;
      var th = el('th', {
        scope: 'col',
        class: (c.key === 'model' ? 'col-model' : '') + (c.key === 'RVC' ? ' rvc-col' : ''),
        'aria-sort': isSorted ? (state.sort.dir === 'desc' ? 'descending' : 'ascending') : null
      }, el('button', {
        class: 'sort-btn', type: 'button', 'data-fid': 'sort:' + c.key, onclick: function () { setSort(c.key); },
        title: c.key === 'RVC' ? 'RVC engine-fidelity track — graded separately from T1–T6' :
          (TIER_INFO[c.key] ? c.key + ' · ' + TIER_INFO[c.key].name : 'Sort by ' + c.label)
      }, c.label, el('span', { class: 'arrow', 'aria-hidden': 'true', text: isSorted && state.sort.dir === 'asc' ? '▲' : '▼' })));
      return th;
    })));
  var tbody = el('tbody');
  rows.forEach(function (row, i) {
    var rankTxt = row.group === 0 ? String(i + 1) : (row.group === 1 ? 'DQ' : '—');
    var tr = el('tr', null,
      el('td', { class: 'col-rank num', text: rankTxt }),
      el('td', { class: 'col-model' }, el('span', { class: 'model-cell' },
        starBtn(row.model),
        el('button', {
          class: 'model-btn', type: 'button', text: row.model,
          title: 'Open ' + row.model + ' details',
          onclick: function (e) { openDrawer(row.model, e.currentTarget); }
        }), badgeSet(row))),
      TIERS.map(function (t) { return el('td', null, tierCellContent(row, t)); }),
      f.hasRVC ? el('td', { class: 'rvc-col' }, tierCellContent(row, 'RVC')) : null,
      el('td', null, scoreCell(row.corr)),
      el('td', null, scoreCell(row.rob)));
    tbody.appendChild(tr);
  });
  body.appendChild(el('div', { class: 'lb-tablewrap scroll-x' },
    el('table', { class: 'lb' },
      el('caption', { class: 'visually-hidden', text: 'Leaderboard: hidden-set pass counts per tier for each model at the selected language and condition' }),
      thead, tbody)));

  /* mobile cards */
  var cards = el('div', { class: 'lb-cards' });
  rows.forEach(function (row, i) {
    var rankTxt = row.group === 0 ? '#' + (i + 1) : (row.group === 1 ? 'DQ' : '—');
    var strip = el('div', { class: 'tier-strip' });
    TIERS.concat(f.hasRVC ? ['RVC'] : []).forEach(function (t) {
      var x = row.run && row.run.tiers && row.run.tiers[t];
      var hasFx = x && num(x.total) > 0;
      var r = hasFx ? num(x.pass) / num(x.total) : null;
      strip.appendChild(el('div', { class: 'ts-cell' },
        el('span', { class: 'ts-lab' + (t === 'RVC' ? ' rvc' : ''), text: t }),
        el('span', { class: 'microbar', 'aria-hidden': 'true' },
          hasFx ? el('i', { style: 'width:' + (r * 100).toFixed(1) + '%' }) : null),
        el('span', { class: 'ts-num', text: !row.run ? '—' : (t === 'RVC' && !x) ? 'n/a' : hasFx ? frac(num(x.pass), num(x.total)) : '—' })));
    });
    cards.appendChild(el('article', { class: 'card lb-card' },
      el('div', { class: 'top' },
        el('span', { class: 'rank num', text: rankTxt }),
        starBtn(row.model, 'card'),
        el('button', {
          class: 'model-btn', type: 'button', text: row.model,
          onclick: function (e) { openDrawer(row.model, e.currentTarget); }
        }), badgeSet(row)),
      el('div', { class: 'scores' },
        el('span', null, el('span', { class: 'lab', text: 'Correctness T1–T5' }),
          el('span', { class: 'val num', text: row.corr ? fmtPct(row.corr.r) : '—' }), ' ',
          el('span', { class: 'n', text: row.corr && row.corr.r !== null ? frac(row.corr.pass, row.corr.total) : '' })),
        el('span', null, el('span', { class: 'lab', text: 'Robustness T6' }),
          el('span', { class: 'val num', text: row.rob ? fmtPct(row.rob.r) : '—' }), ' ',
          el('span', { class: 'n', text: row.rob && row.rob.r !== null ? frac(row.rob.pass, row.rob.total) : '' }))),
      strip));
  });
  body.appendChild(cards);

  if (f.hasRVC) {
    body.appendChild(el('p', { class: 'chart-note', style: 'margin:10px 2px 0' },
      'RVC is the engine-fidelity sidetrack (config extraction + f16→f32) — graded separately, never mixed into correctness or robustness. Delta runs don’t carry it, so those cells read n/a.'));
  }
}

/* ================= gauntlet (slope chart) ================= */
function renderGauntlet() {
  var body = $('gauntletBody'), chips = $('tierChips'), f = state.field;
  clearNode(body); clearNode(chips);
  TIERS.concat(f.hasRVC ? ['RVC'] : []).forEach(function (t) {
    chips.appendChild(el('div', { class: 'tier-chip' + (t === 'RVC' ? ' rvc' : '') },
      el('b', { text: t + ' · ' + TIER_INFO[t].name }),
      el('span', { text: TIER_INFO[t].desc })));
  });
  if (state.loadError || !f.runs.length) {
    body.appendChild(emptyNote(state.loadError ? 'No data loaded.' : 'The tier-by-tier cliff chart appears once runs are recorded.'));
    return;
  }
  var rows = sliceRows().filter(function (r) { return r.run; });
  if (!rows.length) {
    body.appendChild(emptyNote('No model ran ' + LANG_LABEL[state.lang] + ' · ' + COND_LABEL[state.cond].toLowerCase() + '. Pick another slice above.'));
    return;
  }
  var hl = effectiveHl();
  var series = rows.map(function (row) {
    return {
      model: row.model, dq: row.dq, color: hl.map.get(row.model) || null,
      vals: TIERS.map(function (t) {
        var x = row.run.tiers && row.run.tiers[t];
        return x && num(x.total) > 0 ? { r: num(x.pass) / num(x.total), pass: num(x.pass), total: num(x.total) } : null;
      })
    };
  });
  var deadTiers = TIERS.filter(function (t, i) {
    return series.every(function (s) { return s.vals[i] === null; });
  });

  var card = chartCard('Pass rate per tier — ' + LANG_LABEL[state.lang] + ' · ' + COND_LABEL[state.cond].toLowerCase(),
    hl.auto ? 'Top scorer auto-highlighted — ☆ models in the leaderboard to follow more.' : null);
  var host = el('div', { class: 'chart-host' });
  card.appendChild(host);
  body.appendChild(card);

  var w = hostWidth(host, 900), hgt = 300;
  var labelRoom = w >= 620 ? 128 : 12;
  var pad = { l: 42, r: labelRoom, t: 14, b: 30 };
  var x = function (i) { return pad.l + (w - pad.l - pad.r) * (TIERS.length === 1 ? 0.5 : i / (TIERS.length - 1)); };
  var y = linScale(0, 1, hgt - pad.b, pad.t);
  var s = svg('svg', { viewBox: '0 0 ' + w + ' ' + hgt, width: w, height: hgt, role: 'img',
    'aria-label': 'Line chart of hidden-set pass rate across tiers T1 to T6 for each model. Data table below.' });

  [0, 0.25, 0.5, 0.75, 1].forEach(function (v) {
    s.appendChild(svg('line', { class: v === 0 ? 'axline' : 'gridline', x1: pad.l, x2: w - pad.r, y1: y(v), y2: y(v) }));
    s.appendChild(svg('text', { class: 'ax-tick', x: pad.l - 7, y: y(v) + 4, 'text-anchor': 'end', text: Math.round(v * 100) + '%' }));
  });
  TIERS.forEach(function (t, i) {
    s.appendChild(svg('text', { class: 'ax-lab', x: x(i), y: hgt - 8, 'text-anchor': 'middle', text: t }));
  });

  function pathFor(vals) {
    var d = '', pen = false;
    vals.forEach(function (v, i) {
      if (v === null) { pen = false; return; }
      d += (pen ? 'L' : 'M') + x(i).toFixed(1) + ' ' + y(v.r).toFixed(1);
      pen = true;
    });
    return d;
  }
  var ordered = series.filter(function (sr) { return !sr.color; }).concat(series.filter(function (sr) { return sr.color; }));
  ordered.forEach(function (sr) {
    var d = pathFor(sr.vals);
    if (d) {
      var p = svg('path', { d: d, class: sr.color ? 'hl-line' : 'ctx-line' });
      if (sr.color) p.style.stroke = sr.color;
      s.appendChild(p);
    }
    sr.vals.forEach(function (v, i) {
      if (v === null) return;
      var c = svg('circle', { class: 'mark-dot', cx: x(i), cy: y(v.r), r: sr.color ? 4.5 : 3 });
      c.style.fill = sr.color || 'var(--ctx)';
      s.appendChild(c);
    });
  });

  /* end labels for highlighted series */
  if (labelRoom > 40) {
    var labs = ordered.filter(function (sr) { return sr.color; }).map(function (sr) {
      var last = -1;
      sr.vals.forEach(function (v, i) { if (v !== null) last = i; });
      return last < 0 ? null : { sr: sr, x: x(last) + 9, y: y(sr.vals[last].r) };
    }).filter(Boolean).sort(function (a, b) { return a.y - b.y; });
    for (var i = 1; i < labs.length; i++) if (labs[i].y - labs[i - 1].y < 14) labs[i].y = labs[i - 1].y + 14;
    labs.forEach(function (L) {
      var t = svg('text', { class: 'end-lab', x: L.x, y: L.y + 4, text: L.sr.model });
      t.style.fill = 'var(--ink)';
      s.appendChild(t);
    });
  }

  /* hover: one tooltip per tier column, every series listed */
  TIERS.forEach(function (t, i) {
    var slot = (w - pad.l - pad.r) / Math.max(1, TIERS.length - 1);
    var hit = svg('rect', {
      class: 'hit', x: x(i) - slot / 2, y: pad.t, width: slot, height: hgt - pad.t - pad.b,
      tabindex: '0', role: 'img',
      'aria-label': t + ' ' + TIER_INFO[t].name + ': ' + series.map(function (sr) {
        var v = sr.vals[i];
        return sr.model + ' ' + (v ? fmtPct(v.r) : 'no fixtures');
      }).join(', ')
    });
    attachTip(hit, function () {
      var box = el('div');
      box.appendChild(tipTitle(t + ' · ' + TIER_INFO[t].name));
      var vals = series.map(function (sr) { return { sr: sr, v: sr.vals[i] }; })
        .filter(function (o) { return o.v !== null; })
        .sort(function (a, b) { return b.v.r - a.v.r; });
      vals.slice(0, 8).forEach(function (o) {
        box.appendChild(tipRow(o.sr.color || 'var(--ctx)', fmtPct(o.v.r), o.sr.model + ' · ' + frac(o.v.pass, o.v.total)));
      });
      if (vals.length > 8) box.appendChild(tipDim('…and ' + (vals.length - 8) + ' more — table below'));
      if (!vals.length) box.appendChild(tipDim('No fixtures in this tier for this condition'));
      return box;
    });
    s.appendChild(hit);
  });
  host.appendChild(s);

  var legend = el('div', { class: 'legend' });
  hl.map.forEach(function (color, m) { legend.appendChild(legendKey(color, m + (hl.auto ? ' (top score)' : ''))); });
  if (series.length > hl.map.size) legend.appendChild(legendKey(null, 'other models', true));
  card.appendChild(legend);
  if (deadTiers.length) {
    card.appendChild(el('p', { class: 'chart-note', text: 'No ' + deadTiers.join(', ') + ' fixtures exist in the ' + COND_LABEL[state.cond].toLowerCase() + ' set — shown as gaps, not zeros.' }));
  }
  card.appendChild(tableTwin('View as table', ['Model'].concat(TIERS.map(function (t) { return t; })),
    series.map(function (sr) {
      return [sr.model].concat(sr.vals.map(function (v) { return v ? fmtPct(v.r) + ' (' + frac(v.pass, v.total) + ')' : '—'; }));
    })));
}

/* ================= race to green ================= */
var raceAnim = null;
function renderRace() {
  var body = $('raceBody'), f = state.field;
  clearNode(body);
  if (raceAnim) { cancelAnimationFrame(raceAnim); raceAnim = null; }
  if (state.loadError || !f.runs.length) {
    body.appendChild(emptyNote(state.loadError ? 'No data loaded.' : 'Self-grade curves appear once runs are recorded.'));
    return;
  }
  var rows = sliceRows().filter(function (r) { return r.run; });
  if (!rows.length) { body.appendChild(emptyNote('No runs in this slice.')); return; }

  var hl = effectiveHl();
  var series = rows.map(function (row) {
    var iters = (Array.isArray(row.run.iterations) ? row.run.iterations : [])
      .filter(function (it) { return it && it.tier_passes; })
      .map(function (it) {
        var passes = TIERS.reduce(function (a, t) { return a + num(it.tier_passes[t]); }, 0);
        return { t: Math.max(0, num(it.t_seconds)), passes: passes, tp: it.tier_passes };
      }).sort(function (a, b) { return a.t - b.t; });
    var endT = Math.max(num(row.run.spent && row.run.spent.wall_seconds), iters.length ? iters[iters.length - 1].t : 0);
    return { model: row.model, color: hl.map.get(row.model) || null, iters: iters, endT: endT };
  }).filter(function (sr) { return sr.iters.length; });

  if (!series.length) { body.appendChild(emptyNote('No self-grade snapshots were logged for this slice.')); return; }

  var xmax = Math.max(1, series.reduce(function (a, sr) { return Math.max(a, sr.endT); }, 0));
  var ymax = Math.max(1, series.reduce(function (a, sr) {
    return Math.max(a, sr.iters.reduce(function (b, it) { return Math.max(b, it.passes); }, 0));
  }, 0));
  var sparse = series.every(function (sr) { return sr.iters.length <= 1; });

  var card = chartCard('Public-fixture passes over wall clock — ' + LANG_LABEL[state.lang] + ' · ' + COND_LABEL[state.cond].toLowerCase(),
    sparse ? 'This release logged a single self-grade snapshot per run, so curves collapse to their final state.' : 'Steps mark self-grade snapshots. Drag the scrubber to replay.');
  var host = el('div', { class: 'chart-host' });
  card.appendChild(host);
  body.appendChild(card);

  var w = hostWidth(host, 900), hgt = 280;
  var pad = { l: 42, r: 18, t: 14, b: 30 };
  var x = linScale(0, xmax, pad.l, w - pad.r);
  var y = linScale(0, ymax, hgt - pad.b, pad.t);
  var clipId = 'raceclip' + Math.floor(x(xmax));
  var s = svg('svg', { viewBox: '0 0 ' + w + ' ' + hgt, width: w, height: hgt, role: 'img',
    'aria-label': 'Step chart of public-fixture passes over time per model. Data table below.' });
  var clipRect = svg('rect', { x: 0, y: 0, width: w, height: hgt });
  s.appendChild(svg('defs', null, svg('clipPath', { id: clipId }, clipRect)));

  niceTicks(0, ymax, 4).forEach(function (v) {
    s.appendChild(svg('line', { class: v === 0 ? 'axline' : 'gridline', x1: pad.l, x2: w - pad.r, y1: y(v), y2: y(v) }));
    s.appendChild(svg('text', { class: 'ax-tick', x: pad.l - 7, y: y(v) + 4, 'text-anchor': 'end', text: fmtInt(v) }));
  });
  var seenTick = {};
  niceTicks(0, xmax, w > 700 ? 7 : 4).forEach(function (v) {
    var lab = fmtDurTick(v, xmax);
    if (seenTick[lab] || x(v) > w - pad.r - 74) return;
    seenTick[lab] = 1;
    s.appendChild(svg('text', { class: 'ax-tick', x: x(v), y: hgt - 9, 'text-anchor': 'middle', text: lab }));
  });
  s.appendChild(svg('text', { class: 'ax-lab', x: w - pad.r, y: hgt - 9, 'text-anchor': 'end', text: 'wall clock' }));

  var plot = svg('g', { 'clip-path': 'url(#' + clipId + ')' });
  var ordered = series.filter(function (sr) { return !sr.color; }).concat(series.filter(function (sr) { return sr.color; }));
  ordered.forEach(function (sr) {
    /* step-after path */
    var d = 'M' + x(0).toFixed(1) + ' ' + y(0).toFixed(1);
    var prevY = y(0);
    sr.iters.forEach(function (it) {
      d += ' L' + x(it.t).toFixed(1) + ' ' + prevY.toFixed(1);
      prevY = y(it.passes);
      d += ' L' + x(it.t).toFixed(1) + ' ' + prevY.toFixed(1);
    });
    d += ' L' + x(sr.endT).toFixed(1) + ' ' + prevY.toFixed(1);
    var p = svg('path', { d: d, class: sr.color ? 'hl-line' : 'ctx-line' });
    if (sr.color) p.style.stroke = sr.color;
    plot.appendChild(p);
    sr.iters.forEach(function (it) {
      var c = svg('circle', { class: 'mark-dot', cx: x(it.t), cy: y(it.passes), r: sr.color ? 4.5 : 3.2 });
      c.style.fill = sr.color || 'var(--ctx)';
      plot.appendChild(c);
    });
  });
  s.appendChild(plot);

  var playX = x(0 + xmax * state.raceFrac);
  var playhead = svg('line', { class: 'playhead', x1: playX, x2: playX, y1: pad.t, y2: hgt - pad.b, opacity: state.raceFrac >= 1 ? 0 : 1 });
  s.appendChild(playhead);

  /* marker hit targets (on top, unclipped so they stay reachable) */
  ordered.forEach(function (sr) {
    sr.iters.forEach(function (it, idx) {
      var hit = svg('circle', {
        class: 'hit', cx: x(it.t), cy: y(it.passes), r: 12, tabindex: '0', role: 'img',
        'aria-label': sr.model + ' at ' + fmtDur(it.t) + ': ' + it.passes + ' public fixtures passing'
      });
      attachTip(hit, function () {
        var box = el('div');
        box.appendChild(tipTitle(sr.model + ' · ' + fmtDur(it.t)));
        box.appendChild(tipRow(sr.color || 'var(--ctx)', String(it.passes), 'public fixtures passing'));
        box.appendChild(tipDim(TIERS.map(function (t) { return t + ' ' + num(it.tp[t]); }).join(' · ')));
        if (it.tp.RVC !== undefined) box.appendChild(tipDim('RVC ' + num(it.tp.RVC) + ' (side track)'));
        box.appendChild(tipDim('snapshot ' + (idx + 1) + ' of ' + sr.iters.length));
        return box;
      });
      s.appendChild(hit);
    });
  });
  host.appendChild(s);

  function setFrac(fr) {
    state.raceFrac = Math.max(0, Math.min(1, fr));
    var cx = x(xmax * state.raceFrac);
    clipRect.setAttribute('width', String(Math.max(0, cx)));
    playhead.setAttribute('x1', cx); playhead.setAttribute('x2', cx);
    playhead.setAttribute('opacity', state.raceFrac >= 1 ? '0' : '1');
    out.value = fmtDur(xmax * state.raceFrac);
    range.value = String(Math.round(state.raceFrac * 1000));
  }
  var range = el('input', {
    type: 'range', min: '0', max: '1000', step: '1', value: String(Math.round(state.raceFrac * 1000)),
    'aria-label': 'Replay position in wall-clock time',
    oninput: function () { stopPlay(); setFrac(Number(range.value) / 1000); }
  });
  var out = el('output', { text: fmtDur(xmax * state.raceFrac) });
  var playBtn = el('button', { class: 'playbtn', type: 'button', onclick: togglePlay }, 'Replay ▶');
  function stopPlay() {
    if (raceAnim) { cancelAnimationFrame(raceAnim); raceAnim = null; playBtn.textContent = 'Replay ▶'; }
  }
  function togglePlay() {
    if (raceAnim) { stopPlay(); return; }
    var reduced = window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches;
    if (reduced || sparse) { setFrac(1); return; }
    playBtn.textContent = 'Pause ⏸';
    var from = state.raceFrac >= 1 ? 0 : state.raceFrac;
    var dur = 4500 * (1 - from), t0 = null;
    function step(ts) {
      if (t0 === null) t0 = ts;
      var p = dur <= 0 ? 1 : Math.min(1, (ts - t0) / dur);
      setFrac(from + (1 - from) * p);
      if (p < 1) raceAnim = requestAnimationFrame(step);
      else stopPlay();
    }
    raceAnim = requestAnimationFrame(step);
  }
  if (!sparse) card.appendChild(el('div', { class: 'race-controls' }, playBtn, range, out));
  else state.raceFrac = 1;
  setFrac(state.raceFrac);

  var legend = el('div', { class: 'legend' });
  hl.map.forEach(function (color, m) { legend.appendChild(legendKey(color, m + (hl.auto ? ' (top score)' : ''))); });
  if (series.length > hl.map.size) legend.appendChild(legendKey(null, 'other models', true));
  card.appendChild(legend);
  card.appendChild(tableTwin('View as table',
    ['Model', 'Snapshots', 'First snapshot', 'Final snapshot', 'Final passes'],
    series.map(function (sr) {
      var last = sr.iters[sr.iters.length - 1];
      return [sr.model, String(sr.iters.length), fmtDur(sr.iters[0].t), fmtDur(last.t), String(last.passes)];
    })));
}

/* ================= memorization gap ================= */
function renderGap() {
  var body = $('gapBody'), f = state.field;
  clearNode(body);
  $('gapSub').textContent = f.runs.length
    ? 'Same tiers, but delta swaps in mutated fixtures that punish memorized outputs. Gap = stock − delta pass rate over the T1–T6 reading tiers (RVC excluded), at ' + LANG_LABEL[state.lang] + '.'
    : '';
  if (state.loadError || !f.runs.length) {
    body.appendChild(emptyNote(state.loadError ? 'No data loaded.' : 'Stock-vs-delta gaps appear once runs are recorded.'));
    return;
  }
  if (!f.hasDelta) {
    body.appendChild(emptyNote('This release has no delta-condition runs, so the memorization gap can’t be derived — shown as missing, not zero.', 'No delta runs.'));
    return;
  }
  var entries = [], missing = [];
  f.models.forEach(function (m) {
    var g = gapFor(m, state.lang);
    if (g) entries.push({ model: m, g: g, dq: f.dq.get(m) });
    else {
      var hasAny = LANGS.some(function () { return true; }) &&
        (stockRun(m, state.lang) || runAt(m, state.lang, 'delta'));
      missing.push({ model: m, why: hasAny ? (stockRun(m, state.lang) ? 'no delta run' : 'no stock run') : 'not run at ' + LANG_LABEL[state.lang] });
    }
  });
  entries.sort(function (a, b) { return b.g.gap - a.g.gap; });
  if (!entries.length) {
    body.appendChild(emptyNote('No model has both a stock and a delta run at ' + LANG_LABEL[state.lang] + '.'));
    return;
  }
  var hl = effectiveHl();
  var card = chartCard('Stock vs delta pass rate — ' + LANG_LABEL[state.lang],
    'Big gap ⇒ the reader leaned on memorized fixtures. Both dots share one 0–100% scale.');
  var host = el('div', { class: 'chart-host' });
  card.appendChild(el('div', { class: 'legend gap-legend' },
    (function () { var k = el('span', { class: 'key' }, el('span', { class: 'swatch-dot' }), 'stock (open/closed book)'); k.firstChild.style.setProperty('--c', 'var(--blue)'); return k; })(),
    (function () { var k = el('span', { class: 'key' }, el('span', { class: 'swatch-dot' }), 'delta (mutated fixtures)'); k.firstChild.style.setProperty('--c', theme() === 'light' ? '#86b6ef' : '#86b6ef'); return k; })()));
  card.appendChild(host);
  body.appendChild(card);

  var w = hostWidth(host, 900);
  var nameW = w < 520 ? 92 : 150, valW = 76, padR = 14;
  var rowH = 30, topH = 22;
  var hgt = topH + entries.length * rowH + 8;
  var x = linScale(0, 1, nameW + 8, w - valW - padR);
  var s = svg('svg', { viewBox: '0 0 ' + w + ' ' + hgt, width: w, height: hgt, role: 'img',
    'aria-label': 'Dumbbell chart of stock versus delta pass rates per model. Data table below.' });
  [0, 0.25, 0.5, 0.75, 1].forEach(function (v) {
    s.appendChild(svg('line', { class: 'gridline', x1: x(v), x2: x(v), y1: topH - 4, y2: hgt - 6 }));
    s.appendChild(svg('text', { class: 'ax-tick', x: x(v), y: 12, 'text-anchor': 'middle', text: Math.round(v * 100) + '%' }));
  });
  var deltaDot = '#86b6ef';
  entries.forEach(function (e, i) {
    var cy = topH + i * rowH + rowH / 2;
    var color = hl.map.get(e.model);
    var nm = svg('text', { class: 'ax-lab', x: nameW, y: cy + 4, 'text-anchor': 'end', text: e.model.length > 14 && w < 520 ? e.model.slice(0, 13) + '…' : e.model });
    nm.style.fill = 'var(--ink)'; nm.style.fontWeight = '600';
    s.appendChild(nm);
    var x1 = x(e.g.delta.r), x2 = x(e.g.stock.r);
    s.appendChild(svg('line', { x1: x1, x2: x2, y1: cy, y2: cy, class: 'axline', 'stroke-width': 2 }));
    var dDot = svg('circle', { class: 'mark-dot', cx: x1, cy: cy, r: 5.5 }); dDot.style.fill = deltaDot; s.appendChild(dDot);
    var sDot = svg('circle', { class: 'mark-dot', cx: x2, cy: cy, r: 5.5 }); sDot.style.fill = color || 'var(--blue)'; s.appendChild(sDot);
    var gapT = svg('text', { class: 'ax-tick num', x: w - padR, y: cy + 4, 'text-anchor': 'end', text: fmtPP(e.g.gap) });
    gapT.style.fill = 'var(--ink)'; gapT.style.fontWeight = '700';
    s.appendChild(gapT);
    var hit = svg('rect', {
      class: 'hit', x: 0, y: topH + i * rowH, width: w, height: rowH, tabindex: '0', role: 'img',
      'aria-label': e.model + ': stock ' + fmtPct(e.g.stock.r) + ' (' + frac(e.g.stock.pass, e.g.stock.total) + '), delta ' +
        fmtPct(e.g.delta.r) + ' (' + frac(e.g.delta.pass, e.g.delta.total) + '), gap ' + fmtPP(e.g.gap)
    });
    attachTip(hit, function () {
      var box = el('div');
      box.appendChild(tipTitle(e.model + (e.dq ? ' · DQ' : '')));
      box.appendChild(tipRow('var(--blue)', fmtPct(e.g.stock.r), 'stock (' + COND_LABEL[e.g.stockCond].toLowerCase() + ') · ' + frac(e.g.stock.pass, e.g.stock.total)));
      box.appendChild(tipRow(deltaDot, fmtPct(e.g.delta.r), 'delta · ' + frac(e.g.delta.pass, e.g.delta.total)));
      box.appendChild(tipDim('gap ' + fmtPP(e.g.gap) + ' over T1–T6'));
      return box;
    });
    s.appendChild(hit);
  });
  host.appendChild(s);
  if (missing.length) {
    card.appendChild(el('p', { class: 'gap-missing', text: 'No derivable gap (missing, not zero): ' + missing.map(function (m) { return m.model + ' (' + m.why + ')'; }).join(' · ') }));
  }
  card.appendChild(tableTwin('View as table',
    ['Model', 'Stock', 'Stock n', 'Delta', 'Delta n', 'Gap'],
    entries.map(function (e) {
      return [e.model, fmtPct(e.g.stock.r), frac(e.g.stock.pass, e.g.stock.total),
        fmtPct(e.g.delta.r), frac(e.g.delta.pass, e.g.delta.total), fmtPP(e.g.gap)];
    })));
}

/* ================= cost vs score ================= */
function renderCost() {
  var body = $('costBody'), f = state.field;
  clearNode(body);
  if (state.loadError || !f.runs.length) {
    body.appendChild(emptyNote(state.loadError ? 'No data loaded.' : 'Cost-vs-score lands here once runs are recorded.'));
    return;
  }
  var rows = sliceRows().filter(function (r) { return r.run && r.corr && r.corr.r !== null; });
  if (!rows.length) { body.appendChild(emptyNote('No runs in this slice.')); return; }

  var metrics = {
    tokens: { label: 'Tokens spent', get: function (r) { return num(r.run.spent && r.run.spent.tokens); }, fmt: fmtTok },
    wall: { label: 'Wall clock', get: function (r) { return num(r.run.spent && r.run.spent.wall_seconds); }, fmt: function (v) { return fmtDurTick(v, xmax); } }
  };
  var tokensLive = rows.some(function (r) { return metrics.tokens.get(r) > 0; });
  var wallLive = rows.some(function (r) { return metrics.wall.get(r) > 0; });
  if (!tokensLive && state.costMetric === 'tokens' && wallLive) state.costMetric = 'wall';
  var mk = state.costMetric === 'wall' ? 'wall' : 'tokens';
  var metric = metrics[mk];

  var seg = el('div', { class: 'seg', role: 'group', 'aria-label': 'Cost metric' },
    el('button', { type: 'button', 'aria-pressed': String(mk === 'tokens'), 'data-fid': 'seg:tokens', onclick: function () { state.costMetric = 'tokens'; renderCost(); focusFid('seg:tokens'); } }, 'Tokens'),
    el('button', { type: 'button', 'aria-pressed': String(mk === 'wall'), 'data-fid': 'seg:wall', onclick: function () { state.costMetric = 'wall'; renderCost(); focusFid('seg:wall'); } }, 'Wall clock'));

  if (!tokensLive && !wallLive) {
    body.appendChild(emptyNote('These runs recorded no token or wall-clock spend (all zero), so there is no cost axis to draw — an honest blank beats a fake chart. Scores live in the leaderboard above.', 'No cost telemetry in this slice.'));
    return;
  }

  var hl = effectiveHl();
  var card = chartCard('Correctness vs ' + metric.label.toLowerCase() + ' — ' + LANG_LABEL[state.lang] + ' · ' + COND_LABEL[state.cond].toLowerCase(),
    'Up and to the left wins: more correct for less spend.');
  card.appendChild(el('div', { class: 'chart-head' }, seg));
  var host = el('div', { class: 'chart-host' });
  card.appendChild(host);
  body.appendChild(card);

  var pts = rows.map(function (r) { return { row: r, xv: metric.get(r), yv: r.corr.r, color: hl.map.get(r.model) }; });
  var xmax = Math.max(1, pts.reduce(function (a, p) { return Math.max(a, p.xv); }, 0)) * 1.08;
  var w = hostWidth(host, 900), hgt = 300;
  var pad = { l: 46, r: 20, t: 16, b: 34 };
  var x = linScale(0, xmax, pad.l, w - pad.r);
  var y = linScale(0, 1, hgt - pad.b, pad.t);
  var s = svg('svg', { viewBox: '0 0 ' + w + ' ' + hgt, width: w, height: hgt, role: 'img',
    'aria-label': 'Scatter of correctness against ' + metric.label.toLowerCase() + ' per model. Data table below.' });
  [0, 0.25, 0.5, 0.75, 1].forEach(function (v) {
    s.appendChild(svg('line', { class: v === 0 ? 'axline' : 'gridline', x1: pad.l, x2: w - pad.r, y1: y(v), y2: y(v) }));
    s.appendChild(svg('text', { class: 'ax-tick', x: pad.l - 7, y: y(v) + 4, 'text-anchor': 'end', text: Math.round(v * 100) + '%' }));
  });
  var seenTick = {};
  niceTicks(0, xmax, w > 700 ? 7 : 4).forEach(function (v) {
    var lab = mk === 'tokens' ? fmtTok(v) : fmtDurTick(v, xmax);
    if (seenTick[lab] || x(v) > w - pad.r - 92) return;
    seenTick[lab] = 1;
    s.appendChild(svg('text', { class: 'ax-tick', x: x(v), y: hgt - 12, 'text-anchor': 'middle', text: lab }));
  });
  s.appendChild(svg('text', { class: 'ax-lab', x: w - pad.r, y: hgt - 12, 'text-anchor': 'end', text: metric.label.toLowerCase() }));

  var best = pts.slice().sort(function (a, b) { return b.yv - a.yv || a.xv - b.xv; })[0];
  pts.forEach(function (p) {
    var cx = x(p.xv), cy = y(p.yv);
    if (p.row.dq) {
      var ring = svg('circle', { cx: cx, cy: cy, r: 9, fill: 'none', 'stroke-width': 2 });
      ring.style.stroke = 'var(--crit)';
      s.appendChild(ring);
    }
    var dot = svg('circle', { class: 'mark-dot', cx: cx, cy: cy, r: 5.5 });
    dot.style.fill = p.color || 'var(--blue)';
    s.appendChild(dot);
    if (p.color || p === best) {
      var anchor = cx > w - pad.r - 90 ? 'end' : 'start';
      var lx = anchor === 'end' ? cx - 10 : cx + 10;
      var t = svg('text', { class: 'end-lab', x: lx, y: cy - 9, 'text-anchor': anchor, text: p.row.model });
      t.style.fill = 'var(--ink)';
      s.appendChild(t);
    }
    var hit = svg('circle', {
      class: 'hit', cx: cx, cy: cy, r: 14, tabindex: '0', role: 'img',
      'aria-label': p.row.model + ': ' + fmtPct(p.yv) + ' correctness (' + frac(p.row.corr.pass, p.row.corr.total) + ') for ' +
        (mk === 'tokens' ? fmtTok(p.xv) + ' tokens' : fmtDur(p.xv)) + (p.row.dq ? '. Disqualified.' : '')
    });
    attachTip(hit, function () {
      var box = el('div');
      box.appendChild(tipTitle(p.row.model + (p.row.dq ? ' · DQ' : '')));
      box.appendChild(tipRow(p.color || 'var(--blue)', fmtPct(p.yv), 'correctness · ' + frac(p.row.corr.pass, p.row.corr.total)));
      box.appendChild(tipDim((mk === 'tokens' ? fmtTok(p.xv) + ' tokens spent' : fmtDur(p.xv) + ' wall clock') +
        (p.row.run.budget ? ' · budget ' + (mk === 'tokens' ? fmtTok(num(p.row.run.budget.tokens)) : fmtDur(num(p.row.run.budget.wall_seconds))) : '')));
      return box;
    });
    s.appendChild(hit);
  });
  host.appendChild(s);
  var legend = el('div', { class: 'legend' });
  hl.map.forEach(function (color, m) { legend.appendChild(legendKey(color, m + (hl.auto ? ' (top score)' : '')))} );
  legend.appendChild(legendKey('var(--blue)', 'other models', false));
  if (pts.some(function (p) { return p.row.dq; })) {
    var k = el('span', { class: 'key' }, el('span', { class: 'swatch-dot', style: 'background:none;border:2px solid var(--crit)' }), 'disqualified (executed hostile code)');
    legend.appendChild(k);
  }
  card.appendChild(legend);
  card.appendChild(tableTwin('View as table',
    ['Model', 'Correctness', 'n', 'Tokens spent', 'Wall clock'],
    pts.map(function (p) {
      return [p.row.model, fmtPct(p.yv), frac(p.row.corr.pass, p.row.corr.total),
        fmtTok(num(p.row.run.spent && p.row.run.spent.tokens)), fmtDur(num(p.row.run.spent && p.row.run.spent.wall_seconds))];
    })));
}

/* ================= compare ================= */
function renderCompare() {
  var body = $('cmpBody'), f = state.field;
  clearNode(body);
  if (state.loadError || !f.runs.length) {
    body.appendChild(emptyNote(state.loadError ? 'No data loaded.' : 'Two-model comparison unlocks once runs are recorded.'));
    return;
  }
  if (f.models.length < 2) {
    body.appendChild(emptyNote('Only ' + (f.models.length === 1 ? f.models[0] + ' is' : 'no models are') + ' in this release — a side-by-side needs two. Its full profile lives in the leaderboard row and model panel.', 'Nothing to compare yet.'));
    return;
  }
  if (!state.cmpA || f.models.indexOf(state.cmpA) < 0) state.cmpA = f.models[0];
  if (!state.cmpB || f.models.indexOf(state.cmpB) < 0 || state.cmpB === state.cmpA) {
    state.cmpB = f.models.filter(function (m) { return m !== state.cmpA; })[0];
  }
  function picker(label, val, dotVar, onchange) {
    var fid = 'cmp:' + label;
    var sel = el('select', { 'aria-label': 'Model ' + label, 'data-fid': fid,
      onchange: function () { onchange(sel.value); focusFid(fid); } },
      f.models.map(function (m) { return el('option', { value: m, text: m }); }));
    sel.value = val;
    return el('span', { class: 'pick' },
      el('span', { class: 'pick-dot', style: 'background:' + dotVar }),
      el('label', { text: label }), sel);
  }
  body.appendChild(el('div', { class: 'cmp-pickers' },
    picker('A', state.cmpA, 'var(--cat1)', function (v) { state.cmpA = v; renderCompare(); }),
    picker('B', state.cmpB, 'var(--cat2)', function (v) { state.cmpB = v; renderCompare(); }),
    el('span', { class: 'chart-note', style: 'align-self:center', text: LANG_LABEL[state.lang] + ' · ' + COND_LABEL[state.cond].toLowerCase() + ' (follows the filters above)' })));

  var A = buildRow(state.cmpA, state.lang, state.cond);
  var B = buildRow(state.cmpB, state.lang, state.cond);
  var card = el('div', { class: 'card butterfly' });
  card.appendChild(el('div', { class: 'bf-head' },
    el('span', { class: 'side' }, el('span', { class: 'pick-dot', style: 'background:var(--cat1)' }), el('span', { class: 'name', text: A.model }), badgeSet(A)),
    el('span', { class: 'bf-mid', text: 'vs' }),
    el('span', { class: 'side b' }, badgeSet(B), el('span', { class: 'name', text: B.model }), el('span', { class: 'pick-dot', style: 'background:var(--cat2)' }))));
  if (!A.run || !B.run) {
    card.appendChild(el('p', { class: 'chart-note', text: (!A.run ? A.model : B.model) + ' has no ' + LANG_LABEL[state.lang] + ' · ' + COND_LABEL[state.cond].toLowerCase() + ' run — its side reads as missing below, not zero.' }));
  }
  var tiersToShow = TIERS.concat(f.hasRVC ? ['RVC'] : []);
  tiersToShow.forEach(function (t) {
    function side(row) {
      if (!row.run) return null;
      var x = row.run.tiers && row.run.tiers[t];
      if (t === 'RVC' && !x) return { na: true };
      if (!x || num(x.total) === 0) return { none: true };
      return { r: num(x.pass) / num(x.total), pass: num(x.pass), total: num(x.total) };
    }
    var a = side(A), b = side(B);
    function barCell(v, cls) {
      var cell = el('div', { class: 'bf-bar ' + cls });
      if (v && v.r !== undefined) cell.appendChild(el('i', { style: 'width:' + (v.r * 100).toFixed(1) + '%' }));
      var lab = !v ? 'not run' : v.na ? 'n/a' : v.none ? 'no fixtures' : fmtPct(v.r) + ' · ' + frac(v.pass, v.total);
      cell.setAttribute('title', t + ' — ' + lab);
      return cell;
    }
    var dTxt = (a && b && a.r !== undefined && b.r !== undefined) ? fmtPP(a.r - b.r) + ' A−B' : '';
    var mid = el('span', { class: 'bf-mid' + (t === 'RVC' ? ' rvc' : ''), text: t });
    if (dTxt) mid.appendChild(el('span', { class: 'd', text: dTxt }));
    var row = el('div', { class: 'bf-row', role: 'img',
      'aria-label': t + ': ' + A.model + ' ' + (a && a.r !== undefined ? fmtPct(a.r) + ' (' + frac(a.pass, a.total) + ')' : a && a.na ? 'n/a' : a && a.none ? 'no fixtures' : 'not run') +
        ', ' + B.model + ' ' + (b && b.r !== undefined ? fmtPct(b.r) + ' (' + frac(b.pass, b.total) + ')' : b && b.na ? 'n/a' : b && b.none ? 'no fixtures' : 'not run') },
      barCell(a, 'left'), mid, barCell(b, 'right'));
    attachTip(row, function () {
      var box = el('div');
      box.appendChild(tipTitle(t + (TIER_INFO[t] ? ' · ' + TIER_INFO[t].name : '')));
      box.appendChild(tipRow('var(--cat1)', a && a.r !== undefined ? fmtPct(a.r) : (a && a.na ? 'n/a' : a && a.none ? 'no fx' : 'not run'), A.model + (a && a.r !== undefined ? ' · ' + frac(a.pass, a.total) : '')));
      box.appendChild(tipRow('var(--cat2)', b && b.r !== undefined ? fmtPct(b.r) : (b && b.na ? 'n/a' : b && b.none ? 'no fx' : 'not run'), B.model + (b && b.r !== undefined ? ' · ' + frac(b.pass, b.total) : '')));
      return box;
    });
    card.appendChild(row);
  });

  function statRows(row) {
    var r = row.run;
    return {
      corr: row.corr && row.corr.r !== null ? fmtPct(row.corr.r) + ' (' + frac(row.corr.pass, row.corr.total) + ')' : '—',
      rob: row.rob && row.rob.r !== null ? fmtPct(row.rob.r) + ' (' + frac(row.rob.pass, row.rob.total) + ')' : '—',
      rvc: row.rvc ? fmtPct(row.rvc.r) + ' (' + frac(row.rvc.pass, row.rvc.total) + ')' : 'n/a',
      tok: r ? fmtTok(num(r.spent && r.spent.tokens)) : '—',
      wall: r ? fmtDur(num(r.spent && r.spent.wall_seconds)) : '—',
      rss: r ? fmtBytes(num(r.t5_peak_rss_bytes)) : '—',
      crash: r ? fmtInt(num(r.t6_incidents && r.t6_incidents.crashes)) : '—',
      hang: r ? fmtInt(num(r.t6_incidents && r.t6_incidents.hangs)) : '—',
      exec: r ? fmtInt(num(r.t6_incidents && r.t6_incidents.exec_attempts)) : '—',
      det: r ? fmtInt(num(r.determinism_violations)) : '—'
    };
  }
  var sa = statRows(A), sb = statRows(B);
  var metricRows = [
    ['Correctness (T1–T5)', sa.corr, sb.corr], ['Robustness (T6)', sa.rob, sb.rob],
    ['RVC fidelity', sa.rvc, sb.rvc], ['Tokens spent', sa.tok, sb.tok], ['Wall clock', sa.wall, sb.wall],
    ['T5 peak RSS', sa.rss, sb.rss], ['T6 crashes', sa.crash, sb.crash], ['T6 hangs', sa.hang, sb.hang],
    ['T6 exec attempts', sa.exec, sb.exec], ['Determinism violations', sa.det, sb.det]
  ];
  card.appendChild(el('div', { class: 'cmp-stats scroll-x' },
    el('table', null,
      el('thead', null, el('tr', null, el('th', { text: 'Metric' }), el('th', { text: A.model }), el('th', { text: B.model }))),
      el('tbody', null, metricRows.map(function (r) {
        return el('tr', null, el('td', { class: 'metric', text: r[0] }), el('td', { text: r[1] }), el('td', { text: r[2] }));
      })))));
  body.appendChild(card);
}

/* ================= provenance ================= */
function copyBtn(text, what) {
  return el('button', {
    class: 'copybtn', type: 'button', 'aria-label': 'Copy ' + what,
    onclick: function () {
      try {
        if (navigator.clipboard && navigator.clipboard.writeText) {
          navigator.clipboard.writeText(text).then(function () { toast('Copied ' + what); }, function () { toast('Copy unavailable here'); });
        } else { toast('Copy unavailable here'); }
      } catch (e) { toast('Copy unavailable here'); }
    }
  }, 'copy');
}
function shortHash(h) {
  if (typeof h !== 'string') return '—';
  var v = h.replace(/^sha256:/, '');
  return v.length > 18 ? 'sha256:' + v.slice(0, 10) + '…' + v.slice(-6) : h;
}
function reproItem(label, value, full) {
  return el('div', { class: 'repro-item' },
    el('div', { class: 'rlab', text: label }),
    el('div', { class: 'rval' },
      el('code', { title: full || value, text: value }),
      full ? copyBtn(full, label) : null));
}
function renderRepro() {
  var body = $('reproBody'), raw = state.raw;
  clearNode(body);
  if (state.loadError || !raw) {
    body.appendChild(emptyNote('Provenance appears once results.json loads.'));
    return;
  }
  var grid = el('div', { class: 'repro-grid' });
  grid.appendChild(reproItem('Benchmark version', raw.benchmark_version || '—'));
  grid.appendChild(reproItem('Harness version', raw.harness_version || '—'));
  grid.appendChild(reproItem('Fixture-set hash', shortHash(raw.fixture_set_hash), typeof raw.fixture_set_hash === 'string' ? raw.fixture_set_hash : null));
  grid.appendChild(reproItem('Recorded runs', fmtInt(state.field.runs.length) + ' runs · ' + fmtInt(state.field.models.length) + ' models'));
  body.appendChild(grid);

  var dig = raw.container_digests && typeof raw.container_digests === 'object' ? Object.keys(raw.container_digests).sort() : [];
  if (dig.length) {
    var dgrid = el('div', { class: 'repro-grid', style: 'margin-top:10px' });
    dig.forEach(function (k) {
      dgrid.appendChild(reproItem('container · ' + k, shortHash(raw.container_digests[k]), raw.container_digests[k]));
    });
    body.appendChild(el('details', { class: 'digests' },
      el('summary', { text: 'Container digests (' + dig.length + ') — one per language × condition cell' }), dgrid));
  } else {
    body.appendChild(el('p', { class: 'chart-note', style: 'margin-top:10px', text: 'No container digests recorded in this release.' }));
  }
}

/* ================= model drawer ================= */
var drawerPrevFocus = null;
function openDrawer(model, opener) {
  drawerPrevFocus = opener || document.activeElement;
  state.drawerModel = model;
  state.ddSel = null;
  renderDrawerBody();
  var closeBtn = document.querySelector('.drawer-close');
  if (closeBtn) closeBtn.focus();
}
function closeDrawer() {
  state.drawerModel = null;
  clearNode($('drawerRoot'));
  document.body.style.overflow = '';
  if (drawerPrevFocus && drawerPrevFocus.isConnected) drawerPrevFocus.focus();
  drawerPrevFocus = null;
}
function drawerKeydown(e, drawer) {
  if (e.key === 'Escape') { e.preventDefault(); closeDrawer(); return; }
  if (e.key !== 'Tab') return;
  var focusables = Array.prototype.slice.call(
    drawer.querySelectorAll('button, select, a[href], input, [tabindex="0"], summary')
  ).filter(function (n) { return n.getClientRects().length > 0; });
  if (!focusables.length) return;
  var first = focusables[0], last = focusables[focusables.length - 1];
  if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus(); }
  else if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus(); }
}
function drawerCells(model) {
  var out = [];
  state.field.langs.forEach(function (l) {
    state.field.conds.forEach(function (c) {
      var run = runAt(model, l, c);
      out.push({ lang: l, cond: c, run: run });
    });
  });
  return out;
}
function renderDrawerBody() {
  var root = $('drawerRoot');
  clearNode(root);
  var model = state.drawerModel;
  if (!model) return;
  var f = state.field;
  document.body.style.overflow = 'hidden';

  var cells = drawerCells(model).filter(function (c) { return c.run; });
  if (!state.ddSel || !runAt(model, state.ddSel.lang, state.ddSel.cond)) {
    var pref = cells.filter(function (c) { return c.lang === state.lang && c.cond === state.cond; })[0] || cells[0];
    state.ddSel = pref ? { lang: pref.lang, cond: pref.cond } : null;
  }

  var scrim = el('div', { class: 'drawer-scrim', onclick: closeDrawer });
  var drawer = el('aside', {
    class: 'drawer', role: 'dialog', 'aria-modal': 'true', 'aria-label': model + ' details',
    onkeydown: function (e) { drawerKeydown(e, drawer); }
  });
  var lbRow = buildRow(model, state.lang, state.cond);
  drawer.appendChild(el('div', { class: 'drawer-head' },
    starBtn(model, 'drawer'),
    el('h3', { text: model }),
    badgeSet(lbRow),
    el('button', { class: 'drawer-close', type: 'button', 'aria-label': 'Close ' + model + ' details', onclick: closeDrawer }, '✕')));
  var bodyEl = el('div', { class: 'drawer-body' });
  drawer.appendChild(bodyEl);

  /* --- heatmap --- */
  var hmSec = el('section', { class: 'drawer-sec' },
    el('h4', { text: 'Hidden-set heatmap' }),
    el('p', { class: 'sub', text: 'Tier × language × condition. Cells shaded by pass rate — select one to inspect its fixture categories below.' }));
  if (!cells.length) {
    hmSec.appendChild(emptyNote('No runs recorded for this model.'));
  } else {
    var allCells = drawerCells(model);
    var th = theme();
    var showRVC = f.hasRVC;
    var head1 = el('tr', null, el('th'));
    var head2 = el('tr', null, el('th', { class: 'rowlab', scope: 'col', text: 'tier' }));
    f.langs.forEach(function (l) {
      head1.appendChild(el('th', { colspan: String(f.conds.length), scope: 'colgroup', text: LANG_LABEL[l] }));
      f.conds.forEach(function (c) { head2.appendChild(el('th', { scope: 'col', text: COND_SHORT[c] })); });
    });
    var tbl = el('table', { class: 'hm' }, el('thead', null, head1, head2));
    var tb = el('tbody');
    TIERS.concat(showRVC ? ['RVC'] : []).forEach(function (t) {
      var tr = el('tr', { class: t === 'RVC' ? 'rvc-row' : null },
        el('th', { class: 'rowlab' + (t === 'RVC' ? ' rvc' : ''), scope: 'row', text: t }));
      allCells.forEach(function (cell) {
        var td = el('td');
        if (!cell.run) {
          td.appendChild(el('span', { class: 'cell missing', title: model + ' did not run ' + LANG_LABEL[cell.lang] + ' ' + COND_LABEL[cell.cond].toLowerCase(), text: '—' }));
        } else {
          var x = cell.run.tiers && cell.run.tiers[t];
          if (t === 'RVC' && !x) {
            td.appendChild(el('span', { class: 'cell missing', title: 'RVC track not run in ' + COND_LABEL[cell.cond].toLowerCase(), text: 'n/a' }));
          } else if (!x || num(x.total) === 0) {
            td.appendChild(el('span', { class: 'cell missing', title: 'No ' + t + ' fixtures in ' + COND_LABEL[cell.cond].toLowerCase(), text: '—' }));
          } else {
            var r = num(x.pass) / num(x.total);
            var ramp = RAMP[th];
            var idx = Math.max(0, Math.min(5, Math.floor(r * 6)));
            var sel = state.ddSel && state.ddSel.lang === cell.lang && state.ddSel.cond === cell.cond;
            var fid = 'hm:' + t + ':' + cell.lang + ':' + cell.cond;
            var btn = el('button', {
              class: 'cell' + (sel ? ' is-sel' : ''), type: 'button',
              style: 'background:' + ramp.fills[idx] + ';color:' + ramp.inks[idx],
              'aria-pressed': String(!!sel), 'data-fid': fid,
              'aria-label': t + ' at ' + LANG_LABEL[cell.lang] + ' ' + COND_LABEL[cell.cond].toLowerCase() + ': ' + fmtPct(r) + ', ' + frac(num(x.pass), num(x.total)) + '. Inspect categories.',
              onclick: function () { state.ddSel = { lang: cell.lang, cond: cell.cond }; renderDrawerBody(); focusFid(fid); }
            }, el('span', { class: 'pct', text: fmtPct(r) }), el('span', { text: frac(num(x.pass), num(x.total)) }));
            td.appendChild(btn);
          }
        }
        tr.appendChild(td);
      });
      tb.appendChild(tr);
    });
    tbl.appendChild(tb);
    hmSec.appendChild(el('div', { class: 'scroll-x' }, tbl));
    hmSec.appendChild(el('p', { class: 'hm-note', text: 'Dashed cells are missing data (cell not run / track absent) — never zero. ' + (showRVC ? 'The RVC row is the separate engine-fidelity track.' : '') }));
  }
  bodyEl.appendChild(hmSec);

  /* --- fixture drill-down --- */
  var ddSec = el('section', { class: 'drawer-sec' });
  if (state.ddSel) {
    var run = runAt(model, state.ddSel.lang, state.ddSel.cond);
    ddSec.appendChild(el('h4', { text: 'Fixture categories — ' + LANG_LABEL[state.ddSel.lang] + ' · ' + COND_LABEL[state.ddSel.cond].toLowerCase() }));
    ddSec.appendChild(el('p', { class: 'sub', text: 'Every category in this run’s tier composition; failing categories flagged. T6 carries its incident stream.' }));
    TIERS.concat(run.tiers && run.tiers.RVC ? ['RVC'] : []).forEach(function (t) {
      var x = run.tiers && run.tiers[t];
      var group = el('div', { class: 'dd-group' });
      var titleBits = [el('span', { class: t === 'RVC' ? 'rvc-tag' : null, text: t + ' · ' + TIER_INFO[t].name })];
      if (!x || num(x.total) === 0) {
        group.appendChild(el('h5', null, titleBits, el('span', { class: 'tsub', text: 'no fixtures in this condition' })));
        ddSec.appendChild(group);
        return;
      }
      var failing = Object.keys(x.categories || {}).filter(function (c) {
        return num(x.categories[c].pass) < num(x.categories[c].total);
      });
      var h5 = el('h5', null, titleBits,
        el('span', { class: 'tsub', text: frac(num(x.pass), num(x.total)) + ' passed' + (failing.length ? ' · ' + failing.length + ' categor' + (failing.length > 1 ? 'ies' : 'y') + ' failing' : ' · clean sweep') }));
      if (t === 'T6') {
        var inc = run.t6_incidents || {};
        var chips = el('span', { class: 'incident-chips' });
        if (num(inc.crashes) || num(inc.hangs) || num(inc.exec_attempts)) {
          if (num(inc.crashes)) chips.appendChild(el('span', { class: 'ichip bad', text: 'crashes ' + num(inc.crashes) }));
          if (num(inc.hangs)) chips.appendChild(el('span', { class: 'ichip bad', text: 'hangs ' + num(inc.hangs) }));
          if (num(inc.exec_attempts)) chips.appendChild(el('span', { class: 'ichip bad', text: '⚠ exec attempts ' + num(inc.exec_attempts) + ' — disqualifying' }));
        } else {
          chips.appendChild(el('span', { class: 'ichip ok', text: 'no crashes · no hangs · no exec attempts' }));
        }
        h5.appendChild(chips);
      }
      group.appendChild(h5);
      var cats = Object.keys(x.categories || {}).sort();
      cats.forEach(function (cname) {
        var c = x.categories[cname];
        var p = num(c.pass), tt = num(c.total);
        var r = rate(p, tt);
        var fail = p < tt;
        var row = el('div', { class: 'dd-cat' + (fail ? ' fail' : '') },
          el('span', { class: 'cname', title: cname, text: cname }),
          el('span', { class: 'meter', 'aria-hidden': 'true' }, el('i', { style: 'width:' + (r === null ? 0 : r * 100).toFixed(1) + '%' })),
          el('span', { class: 'cnum', text: frac(p, tt) + (fail ? ' ✗' : ' ✓') }));
        row.setAttribute('role', 'img');
        row.setAttribute('aria-label', cname + ': ' + frac(p, tt) + ' fixtures ' + (fail ? '— failing' : '— passing'));
        group.appendChild(row);
      });
      if (!cats.length) group.appendChild(el('p', { class: 'sub', text: 'No category breakdown recorded.' }));
      ddSec.appendChild(group);
    });
  } else {
    ddSec.appendChild(el('h4', { text: 'Fixture categories' }));
    ddSec.appendChild(emptyNote('No run to drill into.'));
  }
  bodyEl.appendChild(ddSec);

  /* --- per-model race (small multiples per language) --- */
  var raceSec = el('section', { class: 'drawer-sec' },
    el('h4', { text: 'Self-grade race' }),
    el('p', { class: 'sub', text: 'Public-fixture passes (T1–T6) over each run’s wall clock, one panel per language, colored by condition.' }));
  var anyRace = false;
  f.langs.forEach(function (l) {
    var runsHere = f.conds.map(function (c) { return { cond: c, run: runAt(model, l, c) }; })
      .filter(function (o) {
        return o.run && Array.isArray(o.run.iterations) && o.run.iterations.length;
      });
    if (!runsHere.length) return;
    anyRace = true;
    var host = el('div', { class: 'chart-host' });
    raceSec.appendChild(el('p', { class: 'sub', style: 'margin:10px 0 2px;font-weight:700;color:var(--ink)', text: LANG_LABEL[l] }));
    raceSec.appendChild(host);
    var series = runsHere.map(function (o) {
      var iters = o.run.iterations.filter(function (it) { return it && it.tier_passes; })
        .map(function (it) {
          return { t: Math.max(0, num(it.t_seconds)), passes: TIERS.reduce(function (a, t2) { return a + num(it.tier_passes[t2]); }, 0), tp: it.tier_passes };
        }).sort(function (a, b) { return a.t - b.t; });
      return { cond: o.cond, iters: iters, endT: Math.max(num(o.run.spent && o.run.spent.wall_seconds), iters.length ? iters[iters.length - 1].t : 0) };
    }).filter(function (sr) { return sr.iters.length; });
    var xmax = Math.max(1, series.reduce(function (a, sr) { return Math.max(a, sr.endT); }, 0));
    var ymax = Math.max(1, series.reduce(function (a, sr) { return Math.max(a, sr.iters[sr.iters.length - 1].passes); }, 0));
    var w = Math.max(280, hostWidth(host, 560)), hgt = 130;
    var pad = { l: 34, r: 12, t: 8, b: 20 };
    var x = linScale(0, xmax, pad.l, w - pad.r);
    var y = linScale(0, ymax, hgt - pad.b, pad.t);
    var s = svg('svg', { viewBox: '0 0 ' + w + ' ' + hgt, width: w, height: hgt, role: 'img',
      'aria-label': LANG_LABEL[l] + ' self-grade curves: ' + series.map(function (sr) {
        return COND_LABEL[sr.cond] + ' finishes at ' + sr.iters[sr.iters.length - 1].passes + ' passes';
      }).join('; ') });
    [0, ymax].forEach(function (v) {
      s.appendChild(svg('line', { class: v === 0 ? 'axline' : 'gridline', x1: pad.l, x2: w - pad.r, y1: y(v), y2: y(v) }));
      s.appendChild(svg('text', { class: 'ax-tick', x: pad.l - 5, y: y(v) + 4, 'text-anchor': 'end', text: fmtInt(v) }));
    });
    s.appendChild(svg('text', { class: 'ax-tick', x: w - pad.r, y: hgt - 6, 'text-anchor': 'end', text: fmtDurTick(xmax, xmax) }));
    series.forEach(function (sr) {
      var d = 'M' + x(0).toFixed(1) + ' ' + y(0).toFixed(1), prevY = y(0);
      sr.iters.forEach(function (it) {
        d += ' L' + x(it.t).toFixed(1) + ' ' + prevY.toFixed(1);
        prevY = y(it.passes);
        d += ' L' + x(it.t).toFixed(1) + ' ' + prevY.toFixed(1);
      });
      d += ' L' + x(sr.endT).toFixed(1) + ' ' + prevY.toFixed(1);
      var p = svg('path', { d: d, class: 'hl-line', 'stroke-width': 2 });
      p.style.stroke = COND_COLOR[sr.cond];
      s.appendChild(p);
      sr.iters.forEach(function (it) {
        var c = svg('circle', { class: 'mark-dot', cx: x(it.t), cy: y(it.passes), r: 3.5 });
        c.style.fill = COND_COLOR[sr.cond];
        s.appendChild(c);
        var hit = svg('circle', { class: 'hit', cx: x(it.t), cy: y(it.passes), r: 11, tabindex: '0', role: 'img',
          'aria-label': COND_LABEL[sr.cond] + ' at ' + fmtDur(it.t) + ': ' + it.passes + ' passes' });
        attachTip(hit, function () {
          var box = el('div');
          box.appendChild(tipTitle(COND_LABEL[sr.cond] + ' · ' + fmtDur(it.t)));
          box.appendChild(tipRow(COND_COLOR[sr.cond], String(it.passes), 'public fixtures passing'));
          box.appendChild(tipDim(TIERS.map(function (t2) { return t2 + ' ' + num(it.tp[t2]); }).join(' · ')));
          return box;
        });
        s.appendChild(hit);
      });
    });
    host.appendChild(s);
    var lg = el('div', { class: 'legend' });
    series.forEach(function (sr) { lg.appendChild(legendKey(COND_COLOR[sr.cond], COND_LABEL[sr.cond].toLowerCase())); });
    raceSec.appendChild(lg);
  });
  if (!anyRace) raceSec.appendChild(el('p', { class: 'sub', text: 'No self-grade snapshots logged for this model.' }));
  bodyEl.appendChild(raceSec);

  /* --- per-model memorization gap --- */
  var gapSec = el('section', { class: 'drawer-sec' },
    el('h4', { text: 'Memorization gap' }),
    el('p', { class: 'sub', text: 'Stock − delta pass rate over T1–T6, per language.' }));
  var gapAny = false;
  f.langs.forEach(function (l) {
    var g = gapFor(model, l);
    if (!g) {
      gapSec.appendChild(el('p', { class: 'sub', text: LANG_LABEL[l] + ': no derivable gap (' + (stockRun(model, l) ? 'no delta run' : 'no stock run') + ').' }));
      return;
    }
    gapAny = true;
    gapSec.appendChild(el('div', { class: 'dd-cat' },
      el('span', { class: 'cname', text: LANG_LABEL[l] }),
      el('span', { class: 'cnum', text: 'stock ' + fmtPct(g.stock.r) + ' (' + frac(g.stock.pass, g.stock.total) + ') · delta ' + fmtPct(g.delta.r) + ' (' + frac(g.delta.pass, g.delta.total) + ')' }),
      el('span', { class: 'cnum', style: 'font-weight:700;color:var(--ink)', text: fmtPP(g.gap) })));
  });
  if (!gapAny && !f.hasDelta) gapSec.appendChild(el('p', { class: 'sub', text: 'No delta runs exist in this release.' }));
  bodyEl.appendChild(gapSec);

  /* --- runs table --- */
  var runsSec = el('section', { class: 'drawer-sec' },
    el('h4', { text: 'Runs & budgets' }),
    el('p', { class: 'sub', text: 'One row per (language, condition) cell this model ran.' }));
  if (cells.length) {
    var rt = el('table', { class: 'runs-tbl' },
      el('thead', null, el('tr', null,
        ['Cell', 'Tokens', 'Wall clock', 'T5 peak RSS', 'Det. viol.', 'Crashes', 'Hangs', 'Exec'].map(function (h) { return el('th', { scope: 'col', text: h }); }))),
      el('tbody', null, cells.map(function (c) {
        var r = c.run, inc = r.t6_incidents || {};
        var tokB = num(r.budget && r.budget.tokens), tokS = num(r.spent && r.spent.tokens);
        var wB = num(r.budget && r.budget.wall_seconds), wS = num(r.spent && r.spent.wall_seconds);
        return el('tr', null,
          el('td', { text: LANG_LABEL[c.lang] + ' · ' + COND_SHORT[c.cond] }),
          el('td', { text: tokB > 0 ? fmtTok(tokS) + ' / ' + fmtTok(tokB) : fmtTok(tokS) }),
          el('td', { text: wB > 0 ? fmtDur(wS) + ' / ' + fmtDur(wB) : fmtDur(wS) }),
          el('td', { text: fmtBytes(num(r.t5_peak_rss_bytes)) }),
          el('td', { text: fmtInt(num(r.determinism_violations)) }),
          el('td', { text: fmtInt(num(inc.crashes)) }),
          el('td', { text: fmtInt(num(inc.hangs)) }),
          el('td', { text: fmtInt(num(inc.exec_attempts)) + (num(inc.exec_attempts) > 0 ? ' ⚠' : '') }));
      })));
    runsSec.appendChild(el('div', { class: 'scroll-x' }, rt));
  } else {
    runsSec.appendChild(el('p', { class: 'sub', text: 'No runs recorded.' }));
  }
  bodyEl.appendChild(runsSec);

  root.appendChild(scrim);
  root.appendChild(drawer);
}

/* ================= go ================= */
if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
else boot();
})();
