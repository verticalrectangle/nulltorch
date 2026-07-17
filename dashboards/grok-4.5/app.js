/* NullTorch board — vanilla JS, zero deps. Loads ./results.json */
(function () {
'use strict';

var TIERS = ['T1', 'T2', 'T3', 'T4', 'T5', 'T6'];
var CORR_TIERS = ['T1', 'T2', 'T3', 'T4', 'T5'];
var ALL_TRACKS = ['T1', 'T2', 'T3', 'T4', 'T5', 'T6', 'RVC'];
var LANGS = ['go', 'rust', 'cpp'];
var CONDS = ['open_book', 'closed_book', 'delta'];

var LANG_LABEL = { go: 'Go', rust: 'Rust', cpp: 'C++' };
var COND_LABEL = {
  open_book: 'Open book',
  closed_book: 'Closed book',
  delta: 'Delta'
};
var TIER_INFO = {
  T1: { name: 'Flat tensors', desc: 'Single flat f16/f32 state dicts — the on-ramp.' },
  T2: { name: 'Containers', desc: 'Nested dicts, ordered metadata, sequences of tensors.' },
  T3: { name: 'Views & aliasing', desc: 'Shared storage, offsets, transposes, zero-dim scalars.' },
  T4: { name: 'Formats & dtypes', desc: 'Wide dtypes, zip64, re-zipped archives, legacy protocols.' },
  T5: { name: 'Resource envelope', desc: 'Huge tensors under a memory ceiling — stream, don’t slurp.' },
  T6: { name: 'Adversarial', desc: 'Pickle bombs, exec opcodes, truncation, cycles.' },
  RVC: { name: 'RVC fidelity', desc: 'Engine sidetrack: config extraction + f16→f32. Not a difficulty tier.' }
};

var state = {
  raw: null,
  loadError: null,
  lang: null,       // null = all
  cond: null,       // null = all (but leaderboard prefers one)
  sort: { key: 'correctness', dir: 'desc' },
  model: null,
  cmpA: null,
  cmpB: null,
  drill: null       // { lang, tier }
};

/* ---------- helpers ---------- */
function $(id) { return document.getElementById(id); }
function el(tag, attrs) {
  var n = document.createElement(tag);
  if (attrs) apply(n, attrs);
  for (var i = 2; i < arguments.length; i++) kid(n, arguments[i]);
  return n;
}
function apply(n, attrs) {
  Object.keys(attrs).forEach(function (k) {
    var v = attrs[k];
    if (v === null || v === undefined || v === false) return;
    if (k === 'class') n.className = v;
    else if (k === 'text') n.textContent = v;
    else if (k === 'html') n.innerHTML = v;
    else if (k === 'style' && typeof v === 'object') {
      Object.keys(v).forEach(function (sk) { n.style[sk] = v[sk]; });
    } else if (k.indexOf('on') === 0 && typeof v === 'function') {
      n.addEventListener(k.slice(2).toLowerCase(), v);
    } else if (k === 'dataset' && typeof v === 'object') {
      Object.keys(v).forEach(function (dk) { n.dataset[dk] = v[dk]; });
    } else {
      n.setAttribute(k, v === true ? '' : String(v));
    }
  });
}
function kid(n, c) {
  if (c === null || c === undefined || c === false) return;
  if (Array.isArray(c)) { c.forEach(function (x) { kid(n, x); }); return; }
  n.appendChild(c.nodeType ? c : document.createTextNode(String(c)));
}
function clear(n) { while (n.firstChild) n.removeChild(n.firstChild); }

function num(v) { return typeof v === 'number' && isFinite(v) ? v : 0; }
function rate(p, t) { return t > 0 ? p / t : null; }
function fmtPct(r) {
  if (r === null || r === undefined || !isFinite(r)) return '—';
  return Math.round(r * 100) + '%';
}
function fmtPct1(r) {
  if (r === null || r === undefined || !isFinite(r)) return '—';
  return (r * 100).toFixed(1) + '%';
}
function fmtPP(x) {
  if (x === null || x === undefined || !isFinite(x)) return '—';
  var v = Math.abs(x * 100).toFixed(1);
  return (x < 0 ? '−' : '+') + v + ' pp';
}
function frac(p, t) {
  if (t === null || t === undefined) return 'n/a';
  return num(p) + '/' + num(t);
}
function fmtInt(n) {
  if (n === null || n === undefined || !isFinite(n)) return '—';
  return Math.round(n).toLocaleString('en-US');
}
function fmtBytes(b) {
  if (!b || !isFinite(b)) return '—';
  if (b >= 1073741824) return (b / 1073741824).toFixed(1) + ' GiB';
  if (b >= 1048576) return (b / 1048576).toFixed(0) + ' MiB';
  if (b >= 1024) return Math.round(b / 1024) + ' KiB';
  return b + ' B';
}
function shortHash(h) {
  if (!h) return '—';
  var s = String(h);
  if (s.length <= 20) return s;
  return s.slice(0, 12) + '…' + s.slice(-6);
}
function announce(msg) {
  var a = $('announcer');
  if (a) a.textContent = msg;
}
function toast(msg) {
  var t = $('toast');
  if (!t) return;
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toast._tm);
  toast._tm = setTimeout(function () { t.hidden = true; }, 1600);
}
function copyText(text) {
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(function () {
      toast('Copied');
    }).catch(function () {
      fallbackCopy(text);
    });
  } else {
    fallbackCopy(text);
  }
}
function fallbackCopy(text) {
  var ta = el('textarea', { style: { position: 'fixed', left: '-9999px' } });
  ta.value = text;
  document.body.appendChild(ta);
  ta.select();
  try { document.execCommand('copy'); toast('Copied'); }
  catch (e) { toast('Copy failed'); }
  document.body.removeChild(ta);
}

/* ---------- data ---------- */
function sumTiers(run, keys) {
  var p = 0, t = 0;
  keys.forEach(function (k) {
    var x = run.tiers && run.tiers[k];
    if (x) { p += num(x.pass); t += num(x.total); }
  });
  return { pass: p, total: t, rate: rate(p, t) };
}
function tierRate(run, tier) {
  var x = run.tiers && run.tiers[tier];
  if (!x) return null;
  return rate(num(x.pass), num(x.total));
}
function isDQ(run) {
  return !!(run.t6_incidents && num(run.t6_incidents.exec_attempts) > 0);
}
function modelDQ(modelId, runs) {
  for (var i = 0; i < runs.length; i++) {
    if (runs[i].model_id === modelId && isDQ(runs[i])) return true;
  }
  return false;
}

function indexRuns(runs) {
  var byModel = {};
  var models = [];
  var langs = {};
  var conds = {};
  runs.forEach(function (r) {
    if (!byModel[r.model_id]) {
      byModel[r.model_id] = [];
      models.push(r.model_id);
    }
    byModel[r.model_id].push(r);
    langs[r.language] = true;
    conds[r.condition] = true;
  });
  models.sort();
  return {
    byModel: byModel,
    models: models,
    langs: LANGS.filter(function (l) { return langs[l]; }),
    conds: CONDS.filter(function (c) { return conds[c]; }),
    runs: runs
  };
}

/** stock condition for memorization gap: open_book preferred, else closed_book */
function stockRun(modelRuns, lang) {
  var open = null, closed = null;
  modelRuns.forEach(function (r) {
    if (r.language !== lang) return;
    if (r.condition === 'open_book') open = r;
    if (r.condition === 'closed_book') closed = r;
  });
  return open || closed || null;
}
function deltaRun(modelRuns, lang) {
  for (var i = 0; i < modelRuns.length; i++) {
    if (modelRuns[i].language === lang && modelRuns[i].condition === 'delta') {
      return modelRuns[i];
    }
  }
  return null;
}
function memGap(modelRuns, lang) {
  var stock = stockRun(modelRuns, lang);
  var delta = deltaRun(modelRuns, lang);
  if (!stock || !delta) return null;
  var s = sumTiers(stock, TIERS);
  var d = sumTiers(delta, TIERS);
  if (s.rate === null || d.rate === null) return null;
  return {
    gap: s.rate - d.rate,
    stockRate: s.rate,
    deltaRate: d.rate,
    stock: s,
    delta: d,
    stockCond: stock.condition
  };
}

function heatColor(r, isRvc) {
  if (r === null || r === undefined) return 'var(--heatmap-empty)';
  // 0 → cool empty, 1 → strong
  var t = Math.max(0, Math.min(1, r));
  var theme = document.documentElement.getAttribute('data-theme') === 'light' ? 'light' : 'dark';
  if (isRvc) {
    // purple ramp
    if (theme === 'light') {
      return lerpHex(['#f3e8ff', '#d8b4fe', '#a855f7', '#7e22ce'], t);
    }
    return lerpHex(['#2a1a3a', '#4c1d75', '#7e22ce', '#c084fc'], t);
  }
  // blue-green-amber honesty ramp
  if (theme === 'light') {
    return lerpHex(['#fee2e2', '#fde68a', '#86efac', '#16a34a'], t);
  }
  return lerpHex(['#3f1d1d', '#713f12', '#14532d', '#22c55e'], t);
}
function heatInk(r) {
  if (r === null || r === undefined) return 'var(--ink-mute)';
  return r >= 0.55 ? '#ffffff' : (document.documentElement.getAttribute('data-theme') === 'light' ? '#1a1d24' : '#eef1f6');
}
function lerpHex(stops, t) {
  if (t <= 0) return stops[0];
  if (t >= 1) return stops[stops.length - 1];
  var x = t * (stops.length - 1);
  var i = Math.floor(x);
  var f = x - i;
  var a = hexToRgb(stops[i]);
  var b = hexToRgb(stops[Math.min(i + 1, stops.length - 1)]);
  var r = Math.round(a[0] + (b[0] - a[0]) * f);
  var g = Math.round(a[1] + (b[1] - a[1]) * f);
  var bl = Math.round(a[2] + (b[2] - a[2]) * f);
  return 'rgb(' + r + ',' + g + ',' + bl + ')';
}
function hexToRgb(h) {
  var s = h.replace('#', '');
  return [parseInt(s.slice(0, 2), 16), parseInt(s.slice(2, 4), 16), parseInt(s.slice(4, 6), 16)];
}

/* ---------- theme ---------- */
function theme() {
  return document.documentElement.getAttribute('data-theme') === 'light' ? 'light' : 'dark';
}
function toggleTheme() {
  var next = theme() === 'dark' ? 'light' : 'dark';
  document.documentElement.setAttribute('data-theme', next);
  try { localStorage.setItem('nt-theme', next); } catch (e) {}
  // recolor heatmaps / dependent views
  renderAll();
  announce(next === 'dark' ? 'Dark theme' : 'Light theme');
}

/* ---------- filters ---------- */
function availableLangs(idx) { return idx.langs.length ? idx.langs : []; }
function availableConds(idx) { return idx.conds.length ? idx.conds : []; }

function effectiveCond(idx) {
  if (state.cond) return state.cond;
  var cs = availableConds(idx);
  if (cs.indexOf('open_book') >= 0) return 'open_book';
  if (cs.indexOf('closed_book') >= 0) return 'closed_book';
  return cs[0] || null;
}

function filteredRuns(idx) {
  return idx.runs.filter(function (r) {
    if (state.lang && r.language !== state.lang) return false;
    if (state.cond && r.condition !== state.cond) return false;
    return true;
  });
}

/* leaderboard rows: one per (model, language) under effective condition */
function leaderboardRows(idx) {
  var cond = effectiveCond(idx);
  var rows = [];
  idx.models.forEach(function (mid) {
    var runs = idx.byModel[mid];
    var langs = state.lang ? [state.lang] : availableLangs(idx);
    langs.forEach(function (lang) {
      var run = null;
      for (var i = 0; i < runs.length; i++) {
        if (runs[i].language === lang && runs[i].condition === cond) {
          run = runs[i];
          break;
        }
      }
      if (!run && !state.cond) {
        // if no filter and missing preferred cond, try any matching lang
        for (var j = 0; j < runs.length; j++) {
          if (runs[j].language === lang) { run = runs[j]; break; }
        }
      }
      if (!run) return;
      var corr = sumTiers(run, CORR_TIERS);
      var rob = sumTiers(run, ['T6']);
      var rvc = run.tiers && run.tiers.RVC
        ? { pass: num(run.tiers.RVC.pass), total: num(run.tiers.RVC.total), rate: rate(run.tiers.RVC.pass, run.tiers.RVC.total) }
        : null;
      rows.push({
        model_id: mid,
        language: lang,
        condition: run.condition,
        run: run,
        correctness: corr.rate,
        robustness: rob.rate,
        corr: corr,
        rob: rob,
        rvc: rvc,
        dq: isDQ(run) || modelDQ(mid, runs),
        incidents: run.t6_incidents || { crashes: 0, hangs: 0, exec_attempts: 0 },
        tiers: TIERS.map(function (t) {
          var x = run.tiers[t];
          return { tier: t, pass: x ? num(x.pass) : 0, total: x ? num(x.total) : 0, rate: x ? rate(x.pass, x.total) : null };
        })
      });
    });
  });
  return rows;
}

function sortRows(rows) {
  var key = state.sort.key;
  var dir = state.sort.dir === 'asc' ? 1 : -1;
  var sorted = rows.slice().sort(function (a, b) {
    // DQ always sink to bottom on primary quality sorts
    if ((key === 'correctness' || key === 'robustness') && a.dq !== b.dq) {
      return a.dq ? 1 : -1;
    }
    var av, bv;
    if (key === 'model') { av = a.model_id; bv = b.model_id; }
    else if (key === 'language') { av = a.language; bv = b.language; }
    else if (key === 'correctness') { av = a.correctness; bv = b.correctness; }
    else if (key === 'robustness') { av = a.robustness; bv = b.robustness; }
    else if (key === 'rvc') {
      av = a.rvc ? a.rvc.rate : -1;
      bv = b.rvc ? b.rvc.rate : -1;
    } else if (key.indexOf('T') === 0) {
      var ta = a.tiers.find(function (t) { return t.tier === key; });
      var tb = b.tiers.find(function (t) { return t.tier === key; });
      av = ta ? ta.rate : null;
      bv = tb ? tb.rate : null;
    } else {
      av = a.correctness; bv = b.correctness;
    }
    if (av === null || av === undefined) av = -Infinity;
    if (bv === null || bv === undefined) bv = -Infinity;
    if (typeof av === 'string') {
      var c = av.localeCompare(bv);
      return c * dir;
    }
    if (av < bv) return -1 * dir;
    if (av > bv) return 1 * dir;
    // tie-break
    var mc = a.model_id.localeCompare(b.model_id);
    if (mc) return mc;
    return a.language.localeCompare(b.language);
  });
  return sorted;
}

/* ---------- render: hero ---------- */
function renderHero(idx) {
  var root = $('heroStats');
  if (!root) return;
  clear(root);
  var nModels = idx.models.length;
  var nRuns = idx.runs.length;
  var nDQ = idx.models.filter(function (m) { return modelDQ(m, idx.byModel[m]); }).length;
  var cond = effectiveCond(idx);
  var rows = leaderboardRows(idx);
  var best = null;
  rows.forEach(function (r) {
    if (r.dq) return;
    if (r.correctness === null) return;
    if (!best || r.correctness > best.correctness) best = r;
  });

  function card(k, v, s) {
    return el('div', { class: 'stat-card' },
      el('span', { class: 'k', text: k }),
      el('span', { class: 'v', text: v }),
      s ? el('span', { class: 's', text: s }) : null
    );
  }

  if (!nRuns) {
    kid(root, card('Models', '0', 'no runs in file'));
    kid(root, card('Runs', '0', 'empty dataset'));
    kid(root, card('Languages', '—', '—'));
    kid(root, card('Best T1–T5', '—', '—'));
    return;
  }

  kid(root, card('Models', String(nModels), nDQ ? (nDQ + ' disqualified') : 'eligible set'));
  kid(root, card('Runs', String(nRuns), (idx.langs.map(function (l) { return LANG_LABEL[l] || l; }).join(' · ') || '—')));
  kid(root, card('Condition', COND_LABEL[cond] || cond || '—', state.cond ? 'filtered' : 'default view'));
  kid(root, card(
    'Best T1–T5',
    best ? fmtPct(best.correctness) : '—',
    best ? (best.model_id + ' · ' + (LANG_LABEL[best.language] || best.language)) : 'no eligible rows'
  ));
}

/* ---------- render: filters ---------- */
function renderFilters(idx) {
  var bar = $('filterbar');
  if (!bar) return;
  if (!idx.runs.length) {
    bar.hidden = true;
    return;
  }
  bar.hidden = false;

  var langRoot = $('langFilter');
  var condRoot = $('condFilter');
  clear(langRoot);
  clear(condRoot);

  function segBtn(parent, label, pressed, onClick) {
    var b = el('button', {
      type: 'button',
      class: 'seg-btn',
      'aria-pressed': pressed ? 'true' : 'false',
      text: label,
      onClick: onClick
    });
    parent.appendChild(b);
    return b;
  }

  segBtn(langRoot, 'All', !state.lang, function () {
    state.lang = null;
    renderAll();
  });
  availableLangs(idx).forEach(function (l) {
    segBtn(langRoot, LANG_LABEL[l] || l, state.lang === l, function () {
      state.lang = l;
      renderAll();
    });
  });

  segBtn(condRoot, 'All', !state.cond, function () {
    state.cond = null;
    renderAll();
  });
  availableConds(idx).forEach(function (c) {
    segBtn(condRoot, COND_LABEL[c] || c, state.cond === c, function () {
      state.cond = c;
      renderAll();
    });
  });

  var hint = $('filterHint');
  if (hint) {
    var parts = [];
    parts.push(idx.models.length + ' model' + (idx.models.length === 1 ? '' : 's'));
    parts.push(filteredRuns(idx).length + ' run cells');
    hint.textContent = parts.join(' · ');
  }
}

/* ---------- render: leaderboard ---------- */
function thSort(label, key, extraClass) {
  var active = state.sort.key === key;
  var aria = active ? (state.sort.dir === 'asc' ? 'ascending' : 'descending') : 'none';
  return el('th', { class: extraClass || '', scope: 'col' },
    el('button', {
      type: 'button',
      class: 'th-btn',
      'aria-sort': aria,
      onClick: function () {
        if (state.sort.key === key) {
          state.sort.dir = state.sort.dir === 'desc' ? 'asc' : 'desc';
        } else {
          state.sort.key = key;
          state.sort.dir = (key === 'model' || key === 'language') ? 'asc' : 'desc';
        }
        renderLeaderboard(state._idx);
        announce('Sorted by ' + label + ', ' + state.sort.dir + 'ending');
      }
    },
      label,
      el('span', { class: 'sort-ind', 'aria-hidden': 'true' })
    )
  );
}

function rateCell(pass, total, r, opts) {
  opts = opts || {};
  if (total === null || total === undefined || (opts.missing)) {
    return el('td', { class: 'num' + (opts.cls ? ' ' + opts.cls : '') },
      el('span', { class: 'na', text: 'n/a' })
    );
  }
  return el('td', { class: 'num' + (opts.cls ? ' ' + opts.cls : '') },
    el('div', { class: 'cell-rate' },
      el('span', { class: 'pct', text: fmtPct(r) }),
      el('span', { class: 'frac', text: frac(pass, total) })
    )
  );
}

function sparkline(tiers) {
  var s = el('div', { class: 'spark', title: 'T1–T6 profile', 'aria-hidden': 'true' });
  tiers.forEach(function (t) {
    var h = t.rate === null ? 2 : Math.max(2, Math.round(t.rate * 18));
    s.appendChild(el('span', {
      'data-t': t.tier,
      style: { height: h + 'px', opacity: t.rate === null ? '0.35' : '1' },
      title: t.tier + ' ' + fmtPct(t.rate)
    }));
  });
  return s;
}

function renderLeaderboard(idx) {
  var head = $('lbHead');
  var body = $('lbBody');
  var empty = $('lbEmpty');
  if (!head || !body) return;
  clear(head);
  clear(body);

  if (!idx.runs.length) {
    empty.hidden = false;
    clear(empty);
    kid(empty,
      el('strong', { text: 'No runs yet' }),
      el('p', { text: 'This results file has an empty runs array. Filters and comparisons will light up once models are graded.' })
    );
    return;
  }
  empty.hidden = true;

  var rows = sortRows(leaderboardRows(idx));
  if (!rows.length) {
    empty.hidden = false;
    clear(empty);
    kid(empty,
      el('strong', { text: 'No rows for this filter' }),
      el('p', { text: 'Try another language or condition — the current combination has no matching cells.' })
    );
    return;
  }

  var trh = el('tr');
  kid(trh, el('th', { scope: 'col', class: 'rank', text: '#' }));
  kid(trh, thSort('Model', 'model'));
  kid(trh, thSort('Lang', 'language'));
  kid(trh, thSort('T1–T5', 'correctness'));
  kid(trh, thSort('T6', 'robustness'));
  kid(trh, thSort('RVC', 'rvc', 'rvc-head'));
  TIERS.forEach(function (t) { kid(trh, thSort(t, t)); });
  kid(trh, el('th', { scope: 'col', text: 'Profile' }));
  head.appendChild(trh);

  rows.forEach(function (row, i) {
    var tr = el('tr', {
      class: (row.dq ? 'is-dq ' : '') + (state.model === row.model_id ? 'is-selected' : ''),
      dataset: { model: row.model_id, lang: row.language }
    });
    kid(tr, el('td', { class: 'rank', text: String(i + 1) }));

    var modelCell = el('td');
    var btn = el('button', {
      type: 'button',
      class: 'model-btn',
      text: row.model_id,
      title: 'Open detail for ' + row.model_id,
      onClick: function () {
        state.model = row.model_id;
        if (!state.lang) { /* keep */ }
        renderModelSelect(idx);
        renderDetail(idx);
        renderCompare(idx);
        // highlight
        renderLeaderboard(idx);
        var detail = $('detail');
        if (detail) detail.scrollIntoView({ behavior: 'smooth', block: 'start' });
      }
    });
    modelCell.appendChild(btn);
    if (row.dq) {
      modelCell.appendChild(el('span', {
        class: 'badge badge-dq',
        text: 'DQ',
        title: 'Disqualified: hostile pickle exec_attempts > 0'
      }));
    }
    kid(tr, modelCell);

    kid(tr, el('td', {},
      el('span', { class: 'badge badge-lang', text: LANG_LABEL[row.language] || row.language })
    ));

    kid(tr, el('td', { class: 'num' },
      el('div', { class: 'score-pair' },
        el('span', { class: 'main', text: fmtPct(row.correctness) }),
        el('span', { class: 'sub', text: frac(row.corr.pass, row.corr.total) })
      )
    ));
    kid(tr, el('td', { class: 'num' },
      el('div', { class: 'score-pair' },
        el('span', { class: 'main', text: fmtPct(row.robustness) }),
        el('span', { class: 'sub', text: frac(row.rob.pass, row.rob.total) })
      )
    ));

    if (row.rvc) {
      kid(tr, rateCell(row.rvc.pass, row.rvc.total, row.rvc.rate, { cls: 'rvc-cell' }));
    } else {
      kid(tr, el('td', { class: 'num rvc-cell' }, el('span', { class: 'na', text: 'n/a' })));
    }

    row.tiers.forEach(function (t) {
      kid(tr, rateCell(t.pass, t.total, t.rate));
    });

    kid(tr, el('td', {}, sparkline(row.tiers)));
    body.appendChild(tr);
  });
}

/* ---------- model select + detail ---------- */
function renderModelSelect(idx) {
  var sel = $('modelSelect');
  if (!sel) return;
  var prev = state.model;
  clear(sel);
  if (!idx.models.length) {
    sel.appendChild(el('option', { value: '', text: '— no models —' }));
    sel.disabled = true;
    state.model = null;
    return;
  }
  sel.disabled = false;
  idx.models.forEach(function (m) {
    sel.appendChild(el('option', { value: m, text: m }));
  });
  if (prev && idx.models.indexOf(prev) >= 0) state.model = prev;
  else state.model = idx.models[0];
  sel.value = state.model;
}

function renderDetail(idx) {
  var body = $('detailBody');
  var empty = $('detailEmpty');
  if (!body) return;
  clear(body);

  if (!idx.models.length) {
    empty.hidden = false;
    clear(empty);
    kid(empty,
      el('strong', { text: 'Nothing to inspect' }),
      el('p', { text: 'Load a results file with at least one model run to see the tier × language heatmap.' })
    );
    return;
  }
  empty.hidden = true;

  var mid = state.model || idx.models[0];
  var runs = idx.byModel[mid] || [];
  var cond = effectiveCond(idx);
  var langs = availableLangs(idx);

  // meta chips
  var meta = el('div', { class: 'detail-meta' });
  kid(meta, el('span', { class: 'chip' }, el('strong', { text: mid })));
  kid(meta, el('span', { class: 'chip' }, 'Condition ', el('strong', { text: COND_LABEL[cond] || cond || '—' })));
  if (modelDQ(mid, runs)) {
    kid(meta, el('span', { class: 'badge badge-dq', text: 'Disqualified · exec attempt' }));
  }
  var incidents = { crashes: 0, hangs: 0, exec_attempts: 0 };
  runs.forEach(function (r) {
    if (r.condition !== cond && state.cond) return;
    if (r.t6_incidents) {
      incidents.crashes += num(r.t6_incidents.crashes);
      incidents.hangs += num(r.t6_incidents.hangs);
      incidents.exec_attempts += num(r.t6_incidents.exec_attempts);
    }
  });
  kid(meta, el('span', { class: 'chip' },
    'T6 incidents ',
    el('strong', { text: incidents.crashes + ' crash · ' + incidents.hangs + ' hang · ' + incidents.exec_attempts + ' exec' })
  ));

  // pick runs for heatmap under condition
  function runFor(lang) {
    for (var i = 0; i < runs.length; i++) {
      if (runs[i].language === lang && runs[i].condition === cond) return runs[i];
    }
    return null;
  }

  var tracks = TIERS.slice();
  var anyRvc = langs.some(function (l) {
    var r = runFor(l);
    return r && r.tiers && r.tiers.RVC;
  });
  if (anyRvc) tracks.push('RVC');

  var wrap = el('div', { class: 'heatmap-wrap' });
  var table = el('table', { class: 'heatmap', role: 'grid', 'aria-label': 'Tier by language heatmap for ' + mid });
  var thead = el('thead');
  var hr = el('tr');
  kid(hr, el('th', { text: '' }));
  langs.forEach(function (l) {
    kid(hr, el('th', { scope: 'col', text: LANG_LABEL[l] || l }));
  });
  thead.appendChild(hr);
  table.appendChild(thead);

  var tbody = el('tbody');
  tracks.forEach(function (tier) {
    var tr = el('tr');
    var lab = tier === 'RVC' ? 'RVC' : tier;
    kid(tr, el('th', {
      class: 'row-lab',
      scope: 'row',
      title: (TIER_INFO[tier] && TIER_INFO[tier].desc) || '',
      text: lab
    }));
    langs.forEach(function (lang) {
      var run = runFor(lang);
      var td = el('td');
      if (!run || !run.tiers || !run.tiers[tier]) {
        var missing = el('button', {
          type: 'button',
          class: 'hm-cell is-empty' + (tier === 'RVC' ? ' is-rvc' : ''),
          disabled: 'disabled',
          title: tier === 'RVC' ? 'RVC not run for this cell' : 'No data',
          'aria-label': mid + ' ' + lang + ' ' + tier + ' n/a'
        }, el('span', { text: 'n/a' }));
        td.appendChild(missing);
      } else {
        var x = run.tiers[tier];
        var r = rate(x.pass, x.total);
        var selected = state.drill && state.drill.lang === lang && state.drill.tier === tier;
        var btn = el('button', {
          type: 'button',
          class: 'hm-cell' + (tier === 'RVC' ? ' is-rvc' : '') + (selected ? ' is-selected' : ''),
          style: {
            background: heatColor(r, tier === 'RVC'),
            color: heatInk(r)
          },
          title: tier + ' · ' + (LANG_LABEL[lang] || lang) + ' · ' + frac(x.pass, x.total) + ' (' + fmtPct1(r) + ')',
          'aria-label': mid + ' ' + (LANG_LABEL[lang] || lang) + ' ' + tier + ' ' + frac(x.pass, x.total) + ', ' + fmtPct(r) + '. Activate to drill into categories.',
          'aria-pressed': selected ? 'true' : 'false',
          onClick: function () {
            if (state.drill && state.drill.lang === lang && state.drill.tier === tier) {
              state.drill = null;
            } else {
              state.drill = { lang: lang, tier: tier };
            }
            renderDetail(idx);
          }
        },
          el('span', { text: fmtPct(r) }),
          el('span', { class: 'frac', text: frac(x.pass, x.total) })
        );
        td.appendChild(btn);
      }
      tr.appendChild(td);
    });
    tbody.appendChild(tr);
  });
  table.appendChild(tbody);
  wrap.appendChild(table);

  var grid = el('div', { class: 'detail-grid' });
  var left = el('div');
  left.appendChild(wrap);

  // legend
  left.appendChild(el('div', {
    class: 'gap-legend',
    style: { borderTop: '1px solid var(--line)' }
  },
    el('span', { text: 'Color = pass rate. RVC outlined — fidelity track, not a difficulty tier.' }),
    el('span', { text: 'Missing RVC on delta cells shows as n/a (not zero).' })
  ));

  grid.appendChild(left);

  // drill panel
  var right = el('div', { class: 'drill' });
  if (!state.drill) {
    kid(right,
      el('h3', { text: 'Fixture drill-down' }),
      el('p', {
        style: { margin: 0, color: 'var(--ink-mute)', fontSize: '0.9rem' },
        text: 'Select a heatmap cell to see per-category pass/total for that tier.'
      })
    );
  } else {
    var dLang = state.drill.lang;
    var dTier = state.drill.tier;
    var dRun = runFor(dLang);
    var dTierData = dRun && dRun.tiers && dRun.tiers[dTier];
    kid(right, el('h3', {
      text: dTier + ' · ' + (LANG_LABEL[dLang] || dLang) + (dTier === 'RVC' ? ' (fidelity)' : '')
    }));
    if (!dTierData) {
      kid(right, el('p', {
        style: { margin: 0, color: 'var(--ink-mute)' },
        text: dTier === 'RVC'
          ? 'RVC was not graded for this cell — shown as n/a, not zero.'
          : 'No category data for this cell.'
      }));
    } else {
      kid(right, el('p', {
        style: { margin: '0 0 0.75rem', color: 'var(--ink-soft)', fontSize: '0.88rem' },
        text: (TIER_INFO[dTier] ? TIER_INFO[dTier].desc + ' ' : '') +
          'Total ' + frac(dTierData.pass, dTierData.total) + ' (' + fmtPct1(rate(dTierData.pass, dTierData.total)) + ').'
      }));
      var cats = Object.keys(dTierData.categories || {}).sort();
      var ul = el('ul', { class: 'cat-list' });
      cats.forEach(function (name) {
        var c = dTierData.categories[name];
        var rr = rate(c.pass, c.total);
        var li = el('li', { class: 'cat-row' },
          el('span', { class: 'cat-name', text: name, title: name }),
          el('div', { class: 'cat-bar' },
            el('div', { class: 'bar', title: fmtPct1(rr) },
              el('i', { style: { width: Math.round((rr || 0) * 100) + '%', background: dTier === 'RVC' ? 'var(--rvc)' : 'var(--accent)' } })
            ),
            el('span', { class: 'cat-frac', text: frac(c.pass, c.total) })
          )
        );
        ul.appendChild(li);
      });
      right.appendChild(ul);
    }
  }
  grid.appendChild(right);

  body.appendChild(meta);
  body.appendChild(grid);
}

/* ---------- gap ---------- */
function renderGap(idx) {
  var body = $('gapBody');
  var empty = $('gapEmpty');
  if (!body) return;
  clear(body);

  if (!idx.models.length) {
    empty.hidden = false;
    clear(empty);
    kid(empty,
      el('strong', { text: 'No memorization gaps' }),
      el('p', { text: 'Need stock (open or closed book) and delta runs for the same model × language.' })
    );
    return;
  }

  var items = [];
  idx.models.forEach(function (mid) {
    var runs = idx.byModel[mid];
    var langs = state.lang ? [state.lang] : availableLangs(idx);
    // also include langs present for this model
    if (!state.lang) {
      var seen = {};
      runs.forEach(function (r) { seen[r.language] = true; });
      langs = LANGS.filter(function (l) { return seen[l]; });
      if (!langs.length) langs = Object.keys(seen).sort();
    }
    langs.forEach(function (lang) {
      var g = memGap(runs, lang);
      if (!g) return;
      items.push({
        model_id: mid,
        language: lang,
        gap: g.gap,
        stockRate: g.stockRate,
        deltaRate: g.deltaRate,
        stockCond: g.stockCond
      });
    });
  });

  if (!items.length) {
    empty.hidden = false;
    clear(empty);
    kid(empty,
      el('strong', { text: 'No paired stock/delta cells' }),
      el('p', { text: 'Memorization gap needs both a stock condition and delta for the same model and language. RVC is excluded from the gap.' })
    );
    return;
  }
  empty.hidden = true;

  items.sort(function (a, b) { return b.gap - a.gap; });
  var maxAbs = 0;
  items.forEach(function (it) { maxAbs = Math.max(maxAbs, Math.abs(it.gap)); });
  if (maxAbs <= 0) maxAbs = 1;

  var list = el('div', { class: 'gap-list' });
  items.forEach(function (it) {
    var width = Math.round((Math.abs(it.gap) / maxAbs) * 100);
    var cls = it.gap > 0.0005 ? 'pos' : (it.gap < -0.0005 ? 'neg' : 'zero');
    list.appendChild(el('div', { class: 'gap-row' },
      el('div', { class: 'gap-model' },
        it.model_id,
        el('div', { class: 'gap-lang', text: (LANG_LABEL[it.language] || it.language) + ' · stock ' + (COND_LABEL[it.stockCond] || it.stockCond) })
      ),
      el('div', { class: 'gap-track', title: 'stock ' + fmtPct1(it.stockRate) + ' − delta ' + fmtPct1(it.deltaRate) },
        el('div', {
          class: 'gap-fill',
          style: {
            width: width + '%',
            left: it.gap < 0 ? (100 - width) + '%' : '0',
            background: it.gap < 0
              ? 'linear-gradient(90deg, var(--good), color-mix(in srgb, var(--good) 70%, var(--accent)))'
              : undefined
          }
        })
      ),
      el('div', { class: 'gap-val ' + cls, text: fmtPP(it.gap) })
    ));
  });
  body.appendChild(list);
  body.appendChild(el('div', { class: 'gap-legend' },
    el('span', { text: 'Gap = stock pass rate − delta pass rate over T1–T6.' }),
    el('span', { text: 'Positive ⇒ higher stock than delta (memorization signal).' }),
    el('span', { text: 'RVC excluded by design.' })
  ));
}

/* ---------- compare ---------- */
function fillCmpSelects(idx) {
  ['cmpA', 'cmpB'].forEach(function (id, i) {
    var sel = $(id);
    if (!sel) return;
    var prev = i === 0 ? state.cmpA : state.cmpB;
    clear(sel);
    if (!idx.models.length) {
      sel.appendChild(el('option', { value: '', text: '— none —' }));
      sel.disabled = true;
      return;
    }
    sel.disabled = false;
    idx.models.forEach(function (m) {
      sel.appendChild(el('option', { value: m, text: m }));
    });
    if (prev && idx.models.indexOf(prev) >= 0) {
      if (i === 0) state.cmpA = prev; else state.cmpB = prev;
    } else {
      if (i === 0) state.cmpA = idx.models[0];
      else state.cmpB = idx.models[Math.min(1, idx.models.length - 1)];
    }
    sel.value = i === 0 ? state.cmpA : state.cmpB;
  });
}

function profileForModel(idx, mid) {
  if (!mid || !idx.byModel[mid]) return null;
  var runs = idx.byModel[mid];
  var cond = effectiveCond(idx);
  var lang = state.lang || availableLangs(idx)[0] || null;
  // if lang filtered, use it; else pick first available for model under cond
  var run = null;
  if (lang) {
    for (var i = 0; i < runs.length; i++) {
      if (runs[i].language === lang && runs[i].condition === cond) { run = runs[i]; break; }
    }
  }
  if (!run) {
    for (var j = 0; j < runs.length; j++) {
      if (runs[j].condition === cond) { run = runs[j]; break; }
    }
  }
  if (!run) run = runs[0];
  return run;
}

function renderCompare(idx) {
  var body = $('cmpBody');
  var empty = $('cmpEmpty');
  if (!body) return;
  clear(body);

  if (!idx.models.length) {
    empty.hidden = false;
    clear(empty);
    kid(empty,
      el('strong', { text: 'Nothing to compare' }),
      el('p', { text: 'Side-by-side needs at least one model in the results file.' })
    );
    return;
  }

  if (idx.models.length === 1) {
    empty.hidden = true;
    // still show single model honestly
  } else {
    empty.hidden = true;
  }

  var a = profileForModel(idx, state.cmpA);
  var b = profileForModel(idx, state.cmpB);

  function card(run, label) {
    if (!run) {
      return el('div', { class: 'cmp-card' },
        el('h3', { text: label || '—' }),
        el('p', { style: { color: 'var(--ink-mute)', margin: 0 }, text: 'No run for this selection under the current condition/language filter.' })
      );
    }
    var tracks = TIERS.slice();
    if (run.tiers && run.tiers.RVC) tracks.push('RVC');
    var bars = el('div', { class: 'tier-bars' });
    tracks.forEach(function (t) {
      var x = run.tiers[t];
      if (!x) {
        bars.appendChild(el('div', { class: 'tier-bar-row' + (t === 'RVC' ? ' is-rvc' : ''), 'data-t': t },
          el('span', { class: 'lab', text: t }),
          el('div', { class: 'track' }, el('i', { style: { width: '0%' } })),
          el('span', { class: 'frac na', text: 'n/a' })
        ));
        return;
      }
      var r = rate(x.pass, x.total);
      bars.appendChild(el('div', {
        class: 'tier-bar-row' + (t === 'RVC' ? ' is-rvc' : ''),
        'data-t': t,
        title: (TIER_INFO[t] ? TIER_INFO[t].name + ': ' : '') + frac(x.pass, x.total)
      },
        el('span', { class: 'lab', text: t }),
        el('div', { class: 'track' }, el('i', { style: { width: Math.round((r || 0) * 100) + '%' } })),
        el('span', { class: 'frac', text: frac(x.pass, x.total) })
      ));
    });
    var corr = sumTiers(run, CORR_TIERS);
    var rob = sumTiers(run, ['T6']);
    var meta = el('div', { class: 'cmp-meta' },
      el('span', { class: 'chip' }, el('strong', { text: LANG_LABEL[run.language] || run.language })),
      el('span', { class: 'chip' }, el('strong', { text: COND_LABEL[run.condition] || run.condition })),
      el('span', { class: 'chip' }, 'T1–T5 ', el('strong', { text: fmtPct(corr.rate) })),
      el('span', { class: 'chip' }, 'T6 ', el('strong', { text: fmtPct(rob.rate) }))
    );
    if (isDQ(run)) {
      kid(meta, el('span', { class: 'badge badge-dq', text: 'DQ' }));
    }
    if (run.tiers && !run.tiers.RVC) {
      kid(meta, el('span', { class: 'chip' }, 'RVC ', el('strong', { text: 'n/a' })));
    }
    return el('div', { class: 'cmp-card' },
      el('h3', { text: run.model_id }),
      bars,
      meta
    );
  }

  var grid = el('div', { class: 'cmp-grid' });
  grid.appendChild(card(a, state.cmpA));
  if (idx.models.length === 1) {
    grid.appendChild(el('div', { class: 'cmp-card' },
      el('h3', { text: 'Only one model' }),
      el('p', {
        style: { margin: 0, color: 'var(--ink-mute)' },
        text: 'This dataset has a single model. Comparison will unlock when a second model appears — showing the one profile on the left for now.'
      })
    ));
  } else {
    grid.appendChild(card(b, state.cmpB));
  }
  body.appendChild(grid);

  var note = 'Profiles use the active condition filter';
  if (state.lang) note += ' and language ' + (LANG_LABEL[state.lang] || state.lang);
  note += '. Tier bars keep the full shape visible.';
  if (a && a.condition === 'delta') note += ' Delta cells have no RVC — shown as n/a.';
  body.appendChild(el('div', { class: 'cmp-note', text: note }));
}

/* ---------- repro ---------- */
function renderRepro(raw) {
  var body = $('reproBody');
  if (!body) return;
  clear(body);

  if (!raw) {
    body.appendChild(el('div', { class: 'empty-state' },
      el('strong', { text: 'No results loaded' }),
      el('p', { text: 'Reproducibility metadata appears once results.json is read.' })
    ));
    return;
  }

  function row(k, v, full) {
    var val = v === null || v === undefined || v === '' ? '—' : String(v);
    var display = full ? shortHash(val) : val;
    var r = el('div', { class: 'repro-row' },
      el('div', { class: 'repro-k', text: k }),
      el('div', {
        class: 'repro-v',
        text: display,
        title: val
      })
    );
    if (val !== '—') {
      r.appendChild(el('button', {
        type: 'button',
        class: 'copy-btn',
        text: 'Copy',
        'aria-label': 'Copy ' + k,
        onClick: function () { copyText(val); }
      }));
    } else {
      r.appendChild(el('span'));
    }
    return r;
  }

  var grid = el('div', { class: 'repro-grid' });
  grid.appendChild(row('Harness', raw.harness_version));
  grid.appendChild(row('Benchmark', raw.benchmark_version));
  grid.appendChild(row('Fixture set', raw.fixture_set_hash, true));
  body.appendChild(grid);

  var digests = raw.container_digests || {};
  var keys = Object.keys(digests).sort();
  body.appendChild(el('div', { class: 'repro-head', text: 'Container digests' }));
  if (!keys.length) {
    body.appendChild(el('div', {
      class: 'empty-state',
      style: { padding: '1rem' }
    }, el('p', { text: 'No container digests recorded in this file (empty map).' })));
  } else {
    var ul = el('ul', { class: 'digest-list' });
    keys.forEach(function (k) {
      var h = digests[k];
      ul.appendChild(el('li', { class: 'digest-item' },
        el('span', { class: 'name', text: k, title: k }),
        el('span', { class: 'hash', text: shortHash(h), title: h }),
        el('button', {
          type: 'button',
          class: 'copy-btn',
          text: 'Copy',
          'aria-label': 'Copy digest for ' + k,
          onClick: function () { copyText(h); }
        })
      ));
    });
    // always expose at least one full digest in a dedicated row for gate G3
    body.appendChild(ul);
    var firstKey = keys[0];
    body.appendChild(row('Digest (' + firstKey + ')', digests[firstKey], true));
  }
}

/* ---------- master render ---------- */
function renderAll() {
  var idx = state._idx;
  if (!idx) return;
  renderHero(idx);
  renderFilters(idx);
  renderLeaderboard(idx);
  renderModelSelect(idx);
  renderDetail(idx);
  renderGap(idx);
  fillCmpSelects(idx);
  renderCompare(idx);
  renderRepro(state.raw);
}

/* ---------- events ---------- */
function bind() {
  var themeBtn = $('themeBtn');
  if (themeBtn) themeBtn.addEventListener('click', toggleTheme);

  var modelSelect = $('modelSelect');
  if (modelSelect) {
    modelSelect.addEventListener('change', function () {
      state.model = modelSelect.value;
      state.drill = null;
      renderDetail(state._idx);
      renderLeaderboard(state._idx);
    });
  }
  var cmpA = $('cmpA');
  var cmpB = $('cmpB');
  if (cmpA) cmpA.addEventListener('change', function () {
    state.cmpA = cmpA.value;
    renderCompare(state._idx);
  });
  if (cmpB) cmpB.addEventListener('change', function () {
    state.cmpB = cmpB.value;
    renderCompare(state._idx);
  });

  // keyboard: '/' focuses model select when not in input
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') {
      if (state.drill) {
        state.drill = null;
        renderDetail(state._idx);
      }
    }
  });
}

/* ---------- boot ---------- */
function showFatal(msg) {
  state.loadError = msg;
  var main = document.querySelector('main');
  if (!main) return;
  var b = el('div', { class: 'banner error', role: 'alert' },
    el('strong', { text: 'Could not load results.json' }),
    el('span', { text: msg })
  );
  main.insertBefore(b, main.firstChild);
  // still paint empty shells
  state.raw = {
    benchmark_version: '—',
    harness_version: '—',
    fixture_set_hash: '—',
    container_digests: {},
    runs: []
  };
  state._idx = indexRuns([]);
  renderAll();
}

function boot(data) {
  state.raw = data;
  var runs = Array.isArray(data.runs) ? data.runs : [];
  state._idx = indexRuns(runs);
  // defaults
  if (!state.cond) {
    // leave null so "all" / effective open_book default works for leaderboard
  }
  renderAll();
}

bind();

fetch('./results.json')
  .then(function (res) {
    if (!res.ok) throw new Error('HTTP ' + res.status + ' loading results.json');
    return res.json();
  })
  .then(boot)
  .catch(function (err) {
    showFatal(err && err.message ? err.message : String(err));
  });

})();
