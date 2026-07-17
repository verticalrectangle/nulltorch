/* NullTorch board — vanilla JS, zero dependencies. Loads results.json (relative
   path), renders every view from the data, handles 0..N runs. */
'use strict';
(function () {

  /* ---------------- constants ---------------- */

  var TIERS = ['T1', 'T2', 'T3', 'T4', 'T5', 'T6'];
  var READING = TIERS.slice();            // memorization gap tiers (RVC excluded)
  var CORRECT = ['T1', 'T2', 'T3', 'T4', 'T5'];
  var COND_ORDER = { open_book: 0, closed_book: 1, delta: 2 };
  var LANG_NAMES = { go: 'Go', rust: 'Rust', cpp: 'C++' };
  var COND_NAMES = { open_book: 'open book', closed_book: 'closed book', delta: 'delta' };
  var SVGNS = 'http://www.w3.org/2000/svg';
  var REDUCED = window.matchMedia &&
    window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  /* ---------------- tiny DOM helpers ---------------- */

  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text !== undefined && text !== null) n.textContent = String(text);
    return n;
  }
  function svgEl(tag, attrs) {
    var n = document.createElementNS(SVGNS, tag);
    if (attrs) for (var k in attrs) n.setAttribute(k, attrs[k]);
    return n;
  }
  function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); return node; }

  /* ---------------- format helpers ---------------- */

  function rate(pass, total) { return total > 0 ? pass / total : null; }

  function pctStr(r, digits) {
    if (r === null || r === undefined || !isFinite(r)) return '—';
    var v = r * 100, d = digits === undefined ? (Math.abs(v - Math.round(v)) < 0.05 ? 0 : 1) : digits;
    return v.toFixed(d).replace(/\.0$/, '') + '%';
  }

  function countStr(pass, total) { return pass + '/' + total; }

  function fmtInt(n) {
    return (n === null || n === undefined || !isFinite(n)) ? '—'
      : Number(n).toLocaleString('en-US');
  }

  function fmtTokens(n) {
    if (n === null || n === undefined || !isFinite(n)) return '—';
    if (n >= 1e6) return (n / 1e6).toFixed(1).replace(/\.0$/, '') + 'M';
    if (n >= 1e3) return (n / 1e3).toFixed(1).replace(/\.0$/, '') + 'k';
    return String(n);
  }

  function fmtBytes(n) {
    if (!n) return '0 B';
    var u = ['B', 'KiB', 'MiB', 'GiB'], i = 0, v = n;
    while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
    return (i === 0 ? v : v.toFixed(1)) + ' ' + u[i];
  }

  function fmtWall(s) {
    if (s === null || s === undefined || !isFinite(s)) return '—';
    s = Math.round(s);
    var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), ss = s % 60;
    function pad(x) { return (x < 10 ? '0' : '') + x; }
    return h > 0 ? h + ':' + pad(m) + ':' + pad(ss) : m + ':' + pad(ss);
  }

  function langName(l) { return LANG_NAMES[l] || l; }
  function condName(c) { return COND_NAMES[c] || c; }

  /* heat color: ember red → amber → phosphor green */
  function heat(p) {
    var h = 6 + 124 * p, l = 38 + 10 * p;
    return { bg: 'hsl(' + h.toFixed(1) + ' 72% ' + l.toFixed(1) + '%)',
             fg: p > 0.52 ? '#150e04' : '#f6eedd' };
  }
  function heatRvc(p) {
    var l = 30 + 17 * p;
    return { bg: 'hsl(258 ' + (48 + 22 * p).toFixed(1) + '% ' + l.toFixed(1) + '%)',
             fg: p > 0.62 ? '#150e04' : '#efe9ff' };
  }

  /* ---------------- data derivation ---------------- */

  function sumTiers(run, tiers) {
    var pass = 0, total = 0;
    for (var i = 0; i < tiers.length; i++) {
      var t = run.tiers && run.tiers[tiers[i]];
      if (t && t.total > 0) { pass += t.pass; total += t.total; }
    }
    return { pass: pass, total: total };
  }

  function isDq(run) {
    var inc = run.t6_incidents || {};
    return (inc.exec_attempts || 0) > 0;
  }

  function modelDq(data, model) {
    for (var i = 0; i < data.runs.length; i++) {
      var r = data.runs[i];
      if (r.model_id === model && isDq(r)) return true;
    }
    return false;
  }

  function findRun(data, model, lang, cond) {
    for (var i = 0; i < data.runs.length; i++) {
      var r = data.runs[i];
      if (r.model_id === model && r.language === lang && r.condition === cond) return r;
    }
    return null;
  }

  /* memorization gap: stock (open_book, else closed_book) minus delta, T1–T6 only */
  function gapFor(data, model, lang) {
    var stock = findRun(data, model, lang, 'open_book') ||
                findRun(data, model, lang, 'closed_book');
    var delta = findRun(data, model, lang, 'delta');
    if (!stock || !delta) return null;
    var s = sumTiers(stock, READING), d = sumTiers(delta, READING);
    if (!s.total || !d.total) return null;
    var sr = s.pass / s.total, dr = d.pass / d.total;
    return { stock: sr, delta: dr, gap: sr - dr, stockCond: stock.condition };
  }

  function correctness(run) {
    var s = sumTiers(run, CORRECT);
    return rate(s.pass, s.total);
  }

  /* ---------------- state ---------------- */

  var state = {
    data: null,
    srcName: 'results.json',
    cond: 'open_book',
    lang: 'all',
    sort: { key: 'correct', dir: 'desc' },
    lens: { model: null, cond: null, lang: null, tier: 'T1' }
  };

  /* ---------------- shared builders ---------------- */

  function tierCell(tier, tr, opts) {
    /* tr: tierResult or undefined */
    opts = opts || {};
    var isRvc = tier === 'RVC';
    if (!tr || !tr.total) {
      var na = el(opts.button ? 'button' : 'span', 'tiercell na', opts.naText || '—');
      if (opts.button) { na.type = 'button'; na.disabled = true; }
      return na;
    }
    var r = rate(tr.pass, tr.total);
    var c = isRvc ? heatRvc(r) : heat(r);
    var cell = el(opts.button ? 'button' : 'span', 'tiercell' + (isRvc ? ' rvc' : ''),
                  countStr(tr.pass, tr.total));
    if (opts.button) cell.type = 'button';
    cell.style.background = c.bg;
    cell.style.color = c.fg;
    cell.title = tier + ' — ' + countStr(tr.pass, tr.total) + ' (' + pctStr(r) + ')' +
                 (isRvc ? ' · RVC engine-fidelity track' : '');
    return cell;
  }

  function emptyPanel(title, msg) {
    var p = el('div', 'empty-panel');
    p.appendChild(el('h3', null, title));
    p.appendChild(el('p', null, msg));
    return p;
  }

  /* ---------------- hero ---------------- */

  function renderHero() {
    var data = state.data;
    var stats = clear(document.getElementById('hero-stats'));
    var note = clear(document.getElementById('hero-note'));
    var runs = data.runs;

    var models = {};
    runs.forEach(function (r) { models[r.model_id] = true; });
    var nModels = Object.keys(models).length;

    var pass = 0, total = 0, dqModels = {};
    runs.forEach(function (r) {
      var s = sumTiers(r, TIERS.concat('RVC'));
      pass += s.pass; total += s.total;
      if (isDq(r)) dqModels[r.model_id] = true;
    });
    var nDq = Object.keys(dqModels).length;
    var fixtureRate = rate(pass, total);

    var cards = [
      { label: 'Models under test', value: nModels, cls: '' },
      { label: 'Cells run', value: runs.length, cls: '' },
      { label: 'Hidden fixtures passed', value: fixtureRate === null ? '—' : pctStr(fixtureRate),
        sub: fmtInt(pass) + ' / ' + fmtInt(total), cls: 'amber' },
      { label: nDq === 1 ? 'Model disqualified' : 'Models disqualified', value: nDq,
        cls: nDq ? 'ember' : 'green' }
    ];

    cards.forEach(function (c) {
      var box = el('div');
      var dd = el('dd', c.cls);
      var val = el('span', 'countup', String(c.value));
      dd.appendChild(val);
      if (c.sub) dd.appendChild(el('span', 'sub', c.sub));
      var dt = el('dt', null, c.label);
      box.appendChild(dd); box.appendChild(dt);
      stats.appendChild(box);
      if (!REDUCED && typeof c.value === 'number') countUp(val, c.value);
    });

    if (!runs.length) {
      note.textContent = 'This results file records no runs — the board below is empty but the ' +
        'reproducibility strip still identifies the fixture set.';
    } else if (nDq) {
      note.textContent = 'Disqualified for executing a hostile pickle: ' +
        Object.keys(dqModels).sort().join(', ') + '. Scores retained for the record, ranked last.';
    } else {
      note.textContent = 'No model attempted to execute a hostile pickle. The tier-6 crucible ' +
        'still claims fixtures — see the gauntlet.';
    }
  }

  function countUp(node, target) {
    var t0 = null, dur = 650;
    function step(ts) {
      if (!t0) t0 = ts;
      var k = Math.min(1, (ts - t0) / dur);
      node.textContent = String(Math.round(target * (1 - Math.pow(1 - k, 3))));
      if (k < 1) requestAnimationFrame(step);
    }
    requestAnimationFrame(step);
  }

  /* ---------------- filters ---------------- */

  function availableConditions() {
    var seen = {};
    state.data.runs.forEach(function (r) { seen[r.condition] = true; });
    return Object.keys(seen).sort(function (a, b) {
      return (COND_ORDER[a] !== undefined ? COND_ORDER[a] : 9) -
             (COND_ORDER[b] !== undefined ? COND_ORDER[b] : 9);
    });
  }
  function availableLangs() {
    var seen = {};
    state.data.runs.forEach(function (r) { seen[r.language] = true; });
    return Object.keys(seen).sort();
  }

  function renderFilters() {
    var host = clear(document.getElementById('filters'));
    var conds = availableConditions(), langs = availableLangs();
    if (!state.data.runs.length) return;

    if (conds.indexOf(state.cond) < 0 && state.cond !== 'all') state.cond = conds[0] || 'all';
    if (langs.indexOf(state.lang) < 0 && state.lang !== 'all') state.lang = 'all';

    host.appendChild(pillGroup('Condition', 'cond', conds.concat(['all']), state.cond,
      function (v) { return v === 'all' ? 'all conditions' : condName(v); }));
    host.appendChild(pillGroup('Language', 'lang', ['all'].concat(langs), state.lang,
      function (v) { return v === 'all' ? 'all languages' : langName(v); }));
  }

  function pillGroup(label, name, values, current, fmt) {
    var fs = el('fieldset');
    fs.appendChild(el('legend', null, label));
    var pills = el('div', 'pills');
    values.forEach(function (v) {
      var lab = el('label', 'pill');
      var input = document.createElement('input');
      input.type = 'radio'; input.name = 'flt-' + name; input.value = v;
      input.checked = v === current;
      input.setAttribute('data-filter', name);
      lab.appendChild(input);
      lab.appendChild(el('span', null, fmt(v)));
      pills.appendChild(lab);
    });
    fs.appendChild(pills);
    return fs;
  }

  /* ---------------- board ---------------- */

  function boardRows() {
    return state.data.runs.filter(function (r) {
      return (state.cond === 'all' || r.condition === state.cond) &&
             (state.lang === 'all' || r.language === state.lang);
    }).map(function (r) {
      return { run: r, dq: isDq(r), correct: correctness(r),
               gap: gapFor(state.data, r.model_id, r.language) };
    });
  }

  function sortVal(row, key) {
    var r = row.run;
    if (key === 'model') return r.model_id;
    if (key === 'lang') return r.language;
    if (key === 'cond') return COND_ORDER[r.condition] !== undefined ? COND_ORDER[r.condition] : 9;
    if (key === 'correct') return row.correct;
    if (key === 'gap') return row.gap ? row.gap.gap : null;
    if (key === 'status') return row.dq ? 1 : 0;
    if (key === 'RVC') return r.tiers.RVC ? rate(r.tiers.RVC.pass, r.tiers.RVC.total) : null;
    var t = r.tiers[key];
    return t ? rate(t.pass, t.total) : null;
  }

  function cmpRows(a, b, key, dir) {
    var va = sortVal(a, key), vb = sortVal(b, key);
    if (va === null || va === undefined) return (vb === null || vb === undefined) ? 0 : 1;
    if (vb === null || vb === undefined) return -1;
    var c;
    if (typeof va === 'string') c = va.localeCompare(vb);
    else c = va - vb;
    c = (dir === 'asc' ? 1 : -1) * c;
    if (c !== 0) return c;
    return standingCmp(a, b);
  }

  /* canonical standing order: correctness desc, T6 desc, then identity */
  function standingCmp(a, b) {
    var ca = a.correct, cb = b.correct;
    var c = (cb === null ? -1 : cb) - (ca === null ? -1 : ca);
    if (c) return c;
    var t6a = sortVal(a, 'T6'), t6b = sortVal(b, 'T6');
    c = (t6b === null ? -1 : t6b) - (t6a === null ? -1 : t6a);
    if (c) return c;
    return a.run.model_id.localeCompare(b.run.model_id) ||
           a.run.language.localeCompare(b.run.language) ||
           String(a.run.condition).localeCompare(String(b.run.condition));
  }

  function sortedRows(rows) {
    var key = state.sort.key, dir = state.sort.dir;
    var qual = rows.filter(function (r) { return !r.dq; });
    var dq = rows.filter(function (r) { return r.dq; });
    qual.sort(function (a, b) { return cmpRows(a, b, key, dir); });
    dq.sort(function (a, b) { return cmpRows(a, b, key, dir); });
    return qual.concat(dq);
  }

  function rankMap(rows) {
    /* standing by the canonical order, qualified rows only */
    var qual = rows.filter(function (r) { return !r.dq; }).slice();
    qual.sort(standingCmp);
    var map = new Map();
    qual.forEach(function (row, i) { map.set(row.run, i + 1); });
    return map;
  }

  var SORT_LABELS = { model: 'model', lang: 'language', cond: 'condition', correct: 'correctness',
                      gap: 'memorization gap', status: 'status', RVC: 'RVC' };
  TIERS.forEach(function (t) { SORT_LABELS[t] = t; });

  function renderBoard() {
    var table = clear(document.getElementById('board-table'));
    var caption = clear(document.getElementById('board-caption'));
    var rows = boardRows();

    if (!state.data.runs.length || !rows.length) {
      var thead0 = el('thead'), htr0 = el('tr');
      htr0.appendChild(el('th', null, ''));
      thead0.appendChild(htr0);
      table.appendChild(thead0);
      var tb0 = el('tbody'), tr0 = el('tr'), td0 = el('td');
      td0.appendChild(emptyPanel(
        !state.data.runs.length ? 'The board is empty' : 'No cells match',
        !state.data.runs.length
          ? 'This results file records zero runs. Load a populated results.json to see the standings.'
          : 'No (model × language × condition) cell matches this filter combination.'));
      tr0.appendChild(td0); tb0.appendChild(tr0); table.appendChild(tb0);
      td0.style.border = '0'; td0.style.padding = '18px';
      caption.textContent = '';
      return;
    }

    var ranks = rankMap(rows);
    var sorted = sortedRows(rows);

    /* header */
    var thead = el('thead'), htr = el('tr');
    var thRank = el('th', 'rank', '#');
    thRank.scope = 'col';
    htr.appendChild(thRank);
    var cols = [
      { key: 'model', label: 'Model', cls: 'model-cell' },
      { key: 'lang', label: 'Lang' },
      { key: 'cond', label: 'Condition' }
    ];
    TIERS.forEach(function (t) { cols.push({ key: t, label: t }); });
    cols.push({ key: 'RVC', label: 'RVC', cls: 'rvc-h' });
    cols.push({ key: 'correct', label: 'T1–T5' });
    cols.push({ key: 'gap', label: 'Gap' });
    cols.push({ key: 'status', label: 'Status' });

    cols.forEach(function (c) {
      var th = el('th', c.cls || null);
      th.scope = 'col';
      var btn = el('button', 'thbtn', c.label);
      btn.type = 'button';
      btn.setAttribute('data-action', 'sort');
      btn.setAttribute('data-key', c.key);
      btn.setAttribute('aria-label', 'Sort by ' + (SORT_LABELS[c.key] || c.label));
      if (state.sort.key === c.key) {
        th.setAttribute('aria-sort', state.sort.dir === 'asc' ? 'ascending' : 'descending');
        btn.appendChild(el('span', 'arrow', state.sort.dir === 'asc' ? '▲' : '▼'));
      }
      th.appendChild(btn);
      htr.appendChild(th);
    });
    thead.appendChild(htr);
    table.appendChild(thead);

    /* body */
    var tbody = el('tbody');
    sorted.forEach(function (row) {
      var r = row.run;
      var tr = el('tr', row.dq ? 'dq' : null);

      tr.appendChild(el('td', 'rank', row.dq ? '—' : (ranks.get(r) || '—')));

      var tdModel = el('td', 'model-cell');
      var mb = el('button', 'modelbtn', r.model_id);
      mb.type = 'button';
      mb.setAttribute('data-action', 'lens-open');
      mb.setAttribute('data-model', r.model_id);
      mb.setAttribute('data-cond', r.condition);
      mb.setAttribute('data-lang', r.language);
      mb.setAttribute('aria-label', 'Open ' + r.model_id + ' in the Lens');
      if (modelDq(state.data, r.model_id)) {
        mb.appendChild(el('span', 'dq-badge', 'DQ'));
      }
      tdModel.appendChild(mb);
      tr.appendChild(tdModel);

      tr.appendChild(el('td', 'langtag', langName(r.language)));
      tr.appendChild(el('td', 'condtag', condName(r.condition)));

      TIERS.forEach(function (t) {
        var td = el('td', 'num');
        td.appendChild(tierCell(t, r.tiers[t]));
        tr.appendChild(td);
      });
      var tdRvc = el('td', 'num');
      tdRvc.appendChild(tierCell('RVC', r.tiers.RVC, { naText: 'n/a' }));
      tr.appendChild(tdRvc);

      var s15 = sumTiers(r, CORRECT);
      var tdC = el('td', 'correct', pctStr(row.correct));
      tdC.title = 'Correctness (T1–T5): ' + countStr(s15.pass, s15.total);
      tr.appendChild(tdC);

      var tdG = el('td', 'gapcell');
      if (r.condition === 'delta' || !row.gap) {
        tdG.textContent = '—'; tdG.style.color = 'var(--faint)';
      } else {
        var g = row.gap.gap * 100;
        tdG.textContent = (g > 0 ? '+' : '') + g.toFixed(1);
        tdG.style.color = g > 10 ? 'var(--ember)' : g > 3 ? 'var(--amber)' : 'var(--green)';
        tdG.title = 'Memorization gap (' + condName(row.gap.stockCond) + ' − delta), T1–T6: ' +
                    pctStr(row.gap.stock) + ' − ' + pctStr(row.gap.delta);
      }
      tr.appendChild(tdG);

      var tdS = el('td');
      var chip = el('span', 'statuschip' + (row.dq ? ' dq' : ''), row.dq ? 'Disqualified' : 'Qualified');
      if (row.dq) chip.title = 'Executed a hostile pickle — t6 exec_attempts = ' +
                               (r.t6_incidents.exec_attempts || 0);
      tdS.appendChild(chip);
      tr.appendChild(tdS);

      tbody.appendChild(tr);
    });
    table.appendChild(tbody);

    var nDq = sorted.filter(function (x) { return x.dq; }).length;
    caption.textContent = sorted.length + ' cell' + (sorted.length === 1 ? '' : 's') +
      ' · sorted by ' + (SORT_LABELS[state.sort.key] || state.sort.key) +
      ' (' + state.sort.dir + ')' +
      (nDq ? ' · ' + nDq + ' disqualified run' + (nDq === 1 ? '' : 's') + ' pinned last' : '') +
      '. Correctness = T1–T5 pass rate; RVC is the engine-fidelity track, absent on delta cells.';
  }

  /* ---------------- gauntlet ---------------- */

  function renderGauntlet() {
    var chartHost = clear(document.getElementById('gauntlet-chart'));
    var cap = clear(document.getElementById('gauntlet-caption'));
    var table = clear(document.getElementById('heat-table'));
    var rows = sortedRows(boardRows());

    var hasRvc = rows.some(function (row) { return row.run.tiers.RVC; });

    if (!rows.length) {
      chartHost.appendChild(el('p', 'fineprint',
        state.data.runs.length ? 'No cells match this filter combination.'
                               : 'No runs recorded in this file.'));
      cap.textContent = '';
      buildHeatTable(table, rows, hasRvc);
      return;
    }

    var tiers = TIERS.slice();
    if (hasRvc) tiers.push('RVC');

    var W = 660, H = 252, baseY = 208, top = 26, pad = 8;
    var n = tiers.length;
    var slot = (W - pad * 2) / n;
    var bw = Math.min(64, slot * 0.62);

    var svg = svgEl('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img',
      'aria-label': 'Aggregate hidden-set pass rate per tier across the filtered cells' });

    svg.appendChild(svgEl('line', { x1: pad, y1: baseY, x2: W - pad, y2: baseY, 'class': 'g-axis' }));

    tiers.forEach(function (t, i) {
      var pass = 0, total = 0;
      rows.forEach(function (row) {
        var tr = row.run.tiers[t];
        if (tr && tr.total > 0) { pass += tr.pass; total += tr.total; }
      });
      var r = rate(pass, total);
      var x = pad + slot * i + (slot - bw) / 2;
      var h = r === null ? 0 : Math.max(2, r * (baseY - top));
      var y = baseY - h;

      var rect = svgEl('rect', { x: x, y: y, width: bw, height: h, rx: 6,
        'class': 'g-bar' + (t === 'RVC' ? ' rvc' : '') });
      if (!REDUCED) {
        rect.appendChild(svgEl('animate', { attributeName: 'height', from: 0, to: h,
          dur: '0.5s', fill: 'freeze' }));
        rect.appendChild(svgEl('animate', { attributeName: 'y', from: baseY, to: y,
          dur: '0.5s', fill: 'freeze' }));
      }
      svg.appendChild(rect);

      var v = svgEl('text', { x: x + bw / 2, y: Math.max(12, y - 16), 'text-anchor': 'middle', 'class': 'g-value' });
      v.textContent = pctStr(r);
      svg.appendChild(v);
      var c = svgEl('text', { x: x + bw / 2, y: Math.max(24, y - 4), 'text-anchor': 'middle', 'class': 'g-count' });
      c.textContent = total ? fmtInt(pass) + '/' + fmtInt(total) : 'n/a';
      svg.appendChild(c);
      var lab = svgEl('text', { x: x + bw / 2, y: baseY + 22, 'text-anchor': 'middle', 'class': 'g-label' });
      lab.textContent = t;
      svg.appendChild(lab);
    });

    chartHost.appendChild(svg);
    cap.textContent = 'Aggregate pass rate across ' + rows.length + ' filtered cell' +
      (rows.length === 1 ? '' : 's') + '. RVC (violet) mirrors the reference engine’s ' +
      'output contract — it is graded separately and never averaged into the tiers.';

    buildHeatTable(table, rows, hasRvc);
  }

  function buildHeatTable(table, rows, hasRvc) {
    if (!rows.length) {
      table.appendChild(el('tbody'));
      return;
    }
    var thead = el('thead'), htr = el('tr');
    var th0 = el('th', null, 'Cell');
    th0.scope = 'col';
    htr.appendChild(th0);
    TIERS.concat(hasRvc ? ['RVC'] : []).forEach(function (t) {
      var th = el('th', t === 'RVC' ? 'rvc-h' : null, t);
      th.scope = 'col';
      htr.appendChild(th);
    });
    thead.appendChild(htr);
    table.appendChild(thead);

    var tbody = el('tbody');
    rows.forEach(function (row) {
      var r = row.run;
      var tr = el('tr', row.dq ? 'dq' : null);
      var th = el('th', 'rowh');
      th.scope = 'row';
      th.appendChild(document.createTextNode(r.model_id));
      th.appendChild(el('span', 'sub', langName(r.language) + ' · ' + condName(r.condition)));
      tr.appendChild(th);
      TIERS.concat(hasRvc ? ['RVC'] : []).forEach(function (t) {
        var td = el('td');
        var cell = tierCell(t, r.tiers[t], { button: true, naText: t === 'RVC' ? 'n/a' : '—' });
        cell.classList.add('cellbtn');
        if (r.tiers[t] && r.tiers[t].total) {
          cell.setAttribute('data-action', 'lens-open');
          cell.setAttribute('data-model', r.model_id);
          cell.setAttribute('data-cond', r.condition);
          cell.setAttribute('data-lang', r.language);
          cell.setAttribute('data-tier', t);
          cell.setAttribute('aria-label', r.model_id + ' ' + langName(r.language) + ' ' +
            condName(r.condition) + ' ' + t + ' ' +
            countStr(r.tiers[t].pass, r.tiers[t].total) + ' — open in the Lens');
        }
        td.appendChild(cell);
        tr.appendChild(td);
      });
      tbody.appendChild(tr);
    });
    table.appendChild(tbody);
  }

  /* ---------------- gap ---------------- */

  function renderGap() {
    var host = clear(document.getElementById('gap-chart'));
    var cap = clear(document.getElementById('gap-caption'));

    var pairs = [], missing = [];
    var seen = {};
    state.data.runs.forEach(function (r) {
      var key = r.model_id + '|' + r.language;
      if (seen[key]) return;
      seen[key] = true;
      var g = gapFor(state.data, r.model_id, r.language);
      if (g) pairs.push({ model: r.model_id, lang: r.language, g: g });
      else if (findRun(state.data, r.model_id, r.language, 'open_book') ||
               findRun(state.data, r.model_id, r.language, 'closed_book'))
        missing.push(r.model_id + ' · ' + langName(r.language));
    });

    if (!pairs.length) {
      host.appendChild(emptyPanel('No gap to measure',
        state.data.runs.length
          ? 'This file has no (model, language) pair with both a stock and a delta run — the memorization gap cannot be derived.'
          : 'No runs recorded in this file.'));
      cap.textContent = '';
      return;
    }

    pairs.sort(function (a, b) { return b.g.gap - a.g.gap || a.model.localeCompare(b.model); });

    var legend = el('div', 'gap-legend');
    var li1 = el('span');
    li1.appendChild(el('i')).style.background = 'var(--amber)';
    li1.appendChild(document.createTextNode('stock (open book, else closed book)'));
    var li2 = el('span');
    li2.appendChild(el('i')).style.background = '#8a94a6';
    li2.appendChild(document.createTextNode('delta'));
    legend.appendChild(li1); legend.appendChild(li2);
    host.appendChild(legend);

    pairs.forEach(function (p) {
      var row = el('div', 'gap-row');

      var who = el('div', 'who', p.model);
      who.appendChild(el('span', 'sub', langName(p.lang) + ' · stock ' + pctStr(p.g.stock) +
        ' → delta ' + pctStr(p.g.delta)));
      row.appendChild(who);

      var svg = svgEl('svg', { viewBox: '0 0 200 26', 'aria-hidden': 'true' });
      var x0 = 10, x1 = 190;
      var xs = x0 + (x1 - x0) * p.g.stock;
      var xd = x0 + (x1 - x0) * p.g.delta;
      svg.appendChild(svgEl('line', { x1: x0, y1: 13, x2: x1, y2: 13,
        stroke: 'var(--line)', 'stroke-width': 1 }));
      svg.appendChild(svgEl('line', { x1: Math.min(xs, xd), y1: 13, x2: Math.max(xs, xd), y2: 13,
        stroke: 'var(--amber)', 'stroke-width': 3, 'stroke-linecap': 'round', opacity: 0.55 }));
      svg.appendChild(svgEl('circle', { cx: xd, cy: 13, r: 5, fill: '#8a94a6' }));
      svg.appendChild(svgEl('circle', { cx: xs, cy: 13, r: 5, fill: 'var(--amber)' }));
      row.appendChild(svg);

      var gpts = p.g.gap * 100;
      var val = el('div', 'gap-val', (gpts > 0 ? '+' : '') + gpts.toFixed(1) + ' ');
      val.appendChild(el('span', 'pts', 'pts'));
      val.style.color = gpts > 10 ? 'var(--ember)' : gpts > 3 ? 'var(--amber)' : 'var(--green)';
      val.title = 'stock ' + pctStr(p.g.stock, 1) + ' − delta ' + pctStr(p.g.delta, 1) +
                  ' over tiers T1–T6 (stock: ' + condName(p.g.stockCond) + ')';
      row.appendChild(val);

      host.appendChild(row);
    });

    cap.textContent = pairs.length + ' stock/delta pair' + (pairs.length === 1 ? '' : 's') +
      '. Positive gap = the model lost ground on re-rolled fixtures — memorization signature.' +
      (missing.length ? ' No delta run for: ' + missing.join(', ') + '.' : '');
  }

  /* ---------------- lens ---------------- */

  function modelList() {
    var seen = {};
    state.data.runs.forEach(function (r) { seen[r.model_id] = true; });
    return Object.keys(seen).sort();
  }

  function resolveLens() {
    var models = modelList();
    if (!models.length) return null;
    var L = state.lens;
    if (!L.model || models.indexOf(L.model) < 0) L.model = models[0];

    var conds = {};
    state.data.runs.forEach(function (r) {
      if (r.model_id === L.model) conds[r.condition] = true;
    });
    var condList = Object.keys(conds).sort(function (a, b) {
      return (COND_ORDER[a] !== undefined ? COND_ORDER[a] : 9) -
             (COND_ORDER[b] !== undefined ? COND_ORDER[b] : 9);
    });
    if (!L.cond || condList.indexOf(L.cond) < 0) {
      L.cond = condList.indexOf('open_book') >= 0 ? 'open_book' : condList[0];
    }

    var langs = {};
    state.data.runs.forEach(function (r) {
      if (r.model_id === L.model && r.condition === L.cond) langs[r.language] = true;
    });
    var langList = Object.keys(langs).sort();
    if (!L.lang || langList.indexOf(L.lang) < 0) L.lang = langList[0];

    var run = findRun(state.data, L.model, L.lang, L.cond);
    if (!run) return null;
    if (!run.tiers[L.tier] || !run.tiers[L.tier].total) {
      L.tier = 'T1';
      for (var i = 0; i < TIERS.length; i++) {
        if (run.tiers[TIERS[i]] && run.tiers[TIERS[i]].total) { L.tier = TIERS[i]; break; }
      }
    }
    return { run: run, conds: condList, langs: langList };
  }

  function renderLensPicker() {
    var host = clear(document.getElementById('lens-picker'));
    var models = modelList();
    models.forEach(function (m) {
      var b = el('button', 'chip', m);
      b.type = 'button';
      b.setAttribute('data-action', 'lens-pick');
      b.setAttribute('data-model', m);
      b.setAttribute('aria-pressed', String(state.lens.model === m));
      if (modelDq(state.data, m)) b.appendChild(el('span', 'dq-badge', 'DQ'));
      host.appendChild(b);
    });
    host.hidden = !models.length;
  }

  function renderLens() {
    var panel = clear(document.getElementById('lens-panel'));
    var ctx = resolveLens();
    renderLensPicker();

    if (!ctx) {
      if (!state.data.runs.length) {
        panel.appendChild(emptyPanel('Nothing under the lens',
          'Zero runs in this file — there is no model to inspect.'));
      }
      return;
    }
    var L = state.lens, run = ctx.run;
    var dqModel = modelDq(state.data, L.model);

    /* header */
    var head = el('div', 'lens-head');
    var h3 = el('h3', null, L.model);
    h3.tabIndex = -1;
    h3.id = 'lens-model-title';
    head.appendChild(h3);
    head.appendChild(el('span', 'lens-status ' + (dqModel ? 'dq' : 'ok'),
      dqModel ? '✕ disqualified' : '✓ qualified'));
    panel.appendChild(head);

    if (dqModel) {
      var where = state.data.runs.filter(function (r) {
        return r.model_id === L.model && isDq(r);
      }).map(function (r) { return langName(r.language) + ' ' + condName(r.condition); });
      panel.appendChild(el('div', 'dq-banner',
        'Seccomp caught this model attempting to spawn/exec while reading a hostile pickle (' +
        where.join(', ') + '). Any exec attempt fails the fixture — and disqualifies the model.'));
    }

    /* tabs: condition + language */
    var tabs = el('div', 'lens-tabs');
    tabs.appendChild(tabGroup('Condition', 'lens-cond', ctx.conds, L.cond, condName));
    tabs.appendChild(tabGroup('Language', 'lens-lang', ctx.langs, L.lang, langName));
    panel.appendChild(tabs);

    var grid = el('div', 'lens-grid');

    /* tier × language heatmap */
    var bHeat = el('div', 'lens-block');
    bHeat.appendChild(el('h4', null, 'Tiers × language — ' + condName(L.cond)));
    var mt = el('table', 'mini-heat');
    var mthead = el('thead'), mhtr = el('tr');
    mhtr.appendChild(el('th'));
    ctx.langs.forEach(function (lg) {
      var th = el('th', null, langName(lg));
      th.scope = 'col';
      mhtr.appendChild(th);
    });
    mthead.appendChild(mhtr);
    mt.appendChild(mthead);
    var mtbody = el('tbody');
    TIERS.concat(['RVC']).forEach(function (t) {
      var tr = el('tr');
      var th = el('th', t === 'RVC' ? 'rvc-h' : null, t);
      th.scope = 'row';
      tr.appendChild(th);
      ctx.langs.forEach(function (lg) {
        var rr = findRun(state.data, L.model, lg, L.cond);
        var td = el('td', lg === L.lang ? 'sel' : null);
        var tres = rr && rr.tiers[t];
        var cell = tierCell(t, tres, { button: true, naText: t === 'RVC' ? 'n/a' : '—' });
        cell.classList.add('cellbtn');
        if (tres && tres.total) {
          cell.setAttribute('data-action', 'lens-cell');
          cell.setAttribute('data-lang', lg);
          cell.setAttribute('data-tier', t);
          cell.setAttribute('aria-label', langName(lg) + ' ' + t + ' ' +
            countStr(tres.pass, tres.total) + ' — drill down');
        }
        td.appendChild(cell);
        tr.appendChild(td);
      });
      mtbody.appendChild(tr);
    });
    mt.appendChild(mtbody);
    bHeat.appendChild(mt);
    grid.appendChild(bHeat);

    /* fixture drill-down */
    var bDrill = el('div', 'lens-block');
    bDrill.appendChild(el('h4', null, 'Fixture drill-down — ' + langName(L.lang)));
    var tierTabs = el('div', 'pills');
    tierTabs.setAttribute('role', 'group');
    tierTabs.setAttribute('aria-label', 'Tier');
    TIERS.concat(['RVC']).forEach(function (t) {
      var has = run.tiers[t] && run.tiers[t].total;
      var b = el('button', 'tabbtn', t);
      b.type = 'button';
      b.setAttribute('data-action', 'lens-tier');
      b.setAttribute('data-tier', t);
      b.setAttribute('aria-pressed', String(L.tier === t));
      if (!has) {
        b.disabled = true;
        b.title = t === 'RVC' ? 'RVC not run on this cell' : 'no fixtures in this tier';
        b.style.opacity = .4;
      }
      tierTabs.appendChild(b);
    });
    bDrill.appendChild(tierTabs);
    var catsHost = el('div', 'catbars');
    catsHost.style.marginTop = '14px';
    var tres = run.tiers[L.tier];
    var cats = tres && tres.categories ? Object.keys(tres.categories).sort() : [];
    if (!cats.length) {
      catsHost.appendChild(el('p', 'fineprint', 'No category breakdown recorded for this tier.'));
    } else {
      cats.forEach(function (name) {
        var c = tres.categories[name];
        var failing = c.pass < c.total;
        var row = el('div', 'catrow' + (failing ? ' failing' : ''));
        row.appendChild(el('span', 'cname', name.replace(/_/g, ' ')));
        var bar = el('div', 'bar');
        bar.setAttribute('role', 'img');
        bar.setAttribute('aria-label', name + ': ' + countStr(c.pass, c.total));
        bar.style.display = 'flex';
        var pw = (100 * c.pass / c.total);
        var passSeg = el('i', 'pass'); passSeg.style.width = pw + '%';
        bar.appendChild(passSeg);
        if (failing) {
          var failSeg = el('i', 'fail');
          failSeg.style.width = (100 - pw) + '%';
          bar.appendChild(failSeg);
        }
        row.appendChild(bar);
        row.appendChild(el('span', 'cval', countStr(c.pass, c.total)));
        catsHost.appendChild(row);
      });
    }
    bDrill.appendChild(catsHost);
    grid.appendChild(bDrill);

    /* iterations-to-green */
    var bIter = el('div', 'lens-block');
    bIter.appendChild(el('h4', null, 'Iterations to green — public fixtures'));
    bIter.appendChild(renderIterations(run));
    grid.appendChild(bIter);

    /* run facts */
    var bFacts = el('div', 'lens-block');
    bFacts.appendChild(el('h4', null, 'Run facts'));
    bFacts.appendChild(renderFacts(run));
    grid.appendChild(bFacts);

    panel.appendChild(grid);
  }

  function tabGroup(label, action, values, current, fmt) {
    var wrap = el('div');
    wrap.appendChild(el('div', 'group-label', label));
    var row = el('div', 'pills');
    row.setAttribute('role', 'group');
    row.setAttribute('aria-label', label);
    values.forEach(function (v) {
      var b = el('button', 'tabbtn', fmt(v));
      b.type = 'button';
      b.setAttribute('data-action', action);
      b.setAttribute('data-value', v);
      b.setAttribute('aria-pressed', String(v === current));
      row.appendChild(b);
    });
    wrap.appendChild(row);
    return wrap;
  }

  function renderIterations(run) {
    var iters = run.iterations || [];
    if (!iters.length) {
      return el('p', 'fineprint', 'No self-grade snapshots recorded for this cell.');
    }
    var pts = iters.map(function (it) {
      var sum = 0, tp = it.tier_passes || {};
      for (var k in tp) sum += tp[k];
      return { t: it.t_seconds, v: sum };
    });

    var W = 640, H = 220, ml = 36, mr = 14, mt = 14, mb = 30;
    var iw = W - ml - mr, ih = H - mt - mb;
    var t0 = pts[0].t, t1 = pts[pts.length - 1].t;
    var vmax = 0;
    pts.forEach(function (p) { if (p.v > vmax) vmax = p.v; });
    var ytop = Math.max(1, vmax);

    function X(t) { return ml + (t1 > t0 ? (t - t0) / (t1 - t0) : 0.5) * iw; }
    function Y(v) { return mt + ih - (v / ytop) * ih; }

    var svg = svgEl('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img',
      'aria-label': 'Public-fixture passes over time: ' + pts.length +
        ' self-grade snapshots, final ' + vmax + ' passes' });

    svg.appendChild(svgEl('line', { x1: ml, y1: mt + ih, x2: ml + iw, y2: mt + ih, 'class': 'i-axis' }));
    svg.appendChild(svgEl('line', { x1: ml, y1: mt, x2: ml, y2: mt + ih, 'class': 'i-axis' }));

    [[t0, 'start'], [t1, 'end']].forEach(function (pair, i) {
      var tx = svgEl('text', { x: i === 0 ? ml : ml + iw, y: mt + ih + 20,
        'text-anchor': i === 0 ? 'start' : 'end', 'class': 'i-tick' });
      tx.textContent = fmtWall(pair[0]);
      svg.appendChild(tx);
    });
    var ty = svgEl('text', { x: ml - 6, y: mt + 4, 'text-anchor': 'end', 'class': 'i-tick' });
    ty.textContent = String(ytop);
    svg.appendChild(ty);
    var tz = svgEl('text', { x: ml - 6, y: mt + ih + 4, 'text-anchor': 'end', 'class': 'i-tick' });
    tz.textContent = '0';
    svg.appendChild(tz);

    if (pts.length === 1) {
      svg.appendChild(svgEl('circle', { cx: X(pts[0].t), cy: Y(pts[0].v), r: 4.5, 'class': 'i-dot' }));
      var nt = svgEl('text', { x: ml + iw / 2, y: mt + ih / 2, 'text-anchor': 'middle', 'class': 'i-note' });
      nt.textContent = 'one self-grade snapshot — ' + pts[0].v + ' public-fixture passes at ' +
        fmtWall(pts[0].t);
      svg.appendChild(nt);
      return svg;
    }

    var d = 'M ' + X(pts[0].t) + ' ' + Y(pts[0].v);
    for (var i = 1; i < pts.length; i++) {
      d += ' H ' + X(pts[i].t) + ' V ' + Y(pts[i].v);
    }
    svg.appendChild(svgEl('path', { d: d + ' L ' + X(t1) + ' ' + (mt + ih) + ' L ' + X(t0) + ' ' + (mt + ih) + ' Z',
      'class': 'i-area', stroke: 'none' }));
    svg.appendChild(svgEl('path', { d: d, 'class': 'i-line' }));
    pts.forEach(function (p) {
      svg.appendChild(svgEl('circle', { cx: X(p.t), cy: Y(p.v), r: 3, 'class': 'i-dot' }));
    });
    return svg;
  }

  function renderFacts(run) {
    var host = el('div', 'facts');

    function budgetFact(label, spent, budget, fmt) {
      var f = el('div', 'fact');
      var lab = el('div', 'flabel');
      lab.appendChild(el('span', null, label));
      var frac = budget > 0 ? Math.min(1, spent / budget) : 0;
      lab.appendChild(el('span', null, pctStr(frac, 0) + ' of budget'));
      f.appendChild(lab);
      var bar = el('div', 'bar');
      bar.setAttribute('role', 'img');
      bar.setAttribute('aria-label', label + ': ' + fmt(spent) + ' of ' + fmt(budget));
      var fill = el('i');
      fill.style.width = (frac * 100) + '%';
      fill.style.background = frac > 0.95 ? 'var(--ember)' : 'var(--amber)';
      bar.appendChild(fill);
      f.appendChild(bar);
      var val = el('div', 'fval');
      val.appendChild(document.createTextNode(fmt(spent) + ' '));
      val.appendChild(el('span', 'dim', '/ ' + fmt(budget)));
      f.appendChild(val);
      return f;
    }

    host.appendChild(budgetFact('Tokens', run.spent.tokens, run.budget.tokens, fmtTokens));
    host.appendChild(budgetFact('Wall clock', run.spent.wall_seconds, run.budget.wall_seconds, fmtWall));

    var sAll = sumTiers(run, TIERS.concat('RVC'));
    var f1 = el('div', 'fact');
    f1.appendChild(el('div', 'flabel')).appendChild(el('span', null, 'Hidden-set fixtures'));
    f1.appendChild(el('div', 'fval', countStr(sAll.pass, sAll.total) + ' passed'));
    host.appendChild(f1);

    var f2 = el('div', 'fact');
    f2.appendChild(el('div', 'flabel')).appendChild(el('span', null, 'T5 peak RSS'));
    f2.appendChild(el('div', 'fval', fmtBytes(run.t5_peak_rss_bytes)));
    host.appendChild(f2);

    var f3 = el('div', 'fact');
    f3.appendChild(el('div', 'flabel')).appendChild(el('span', null, 'Determinism violations'));
    var v3 = el('div', 'fval', String(run.determinism_violations));
    if (run.determinism_violations > 0) v3.style.color = 'var(--ember)';
    f3.appendChild(v3);
    host.appendChild(f3);

    var f4 = el('div', 'fact');
    f4.appendChild(el('div', 'flabel')).appendChild(el('span', null, 'T6 incidents'));
    var chips = el('div', 'incident-chips');
    var inc = run.t6_incidents || {};
    [['crashes', inc.crashes || 0, 'warn'],
     ['hangs', inc.hangs || 0, 'warn'],
     ['exec attempts', inc.exec_attempts || 0, 'bad']].forEach(function (x) {
      var cls = x[1] === 0 ? '' : ' ' + x[2];
      chips.appendChild(el('span', 'ichip' + cls, x[0] + ': ' + x[1]));
    });
    f4.appendChild(chips);
    host.appendChild(f4);

    return host;
  }

  /* ---------------- reproducibility ---------------- */

  function renderRepro() {
    var host = clear(document.getElementById('repro-strip'));
    var d = state.data;

    function item(term, valueNode) {
      var box = el('div');
      box.appendChild(el('dt', null, term));
      var dd = el('dd');
      dd.appendChild(valueNode);
      box.appendChild(dd);
      return box;
    }
    function textWithCopy(text) {
      var row = el('div', 'hashrow');
      row.appendChild(el('span', null, text));
      var b = el('button', 'copybtn', 'copy');
      b.type = 'button';
      b.setAttribute('data-action', 'copy');
      b.setAttribute('data-copy', text);
      b.setAttribute('aria-label', 'Copy to clipboard');
      row.appendChild(b);
      return row;
    }

    host.appendChild(item('Benchmark version', document.createTextNode(d.benchmark_version || '—')));
    host.appendChild(item('Harness version', document.createTextNode(d.harness_version || '—')));
    host.appendChild(item('Fixture set hash', textWithCopy(d.fixture_set_hash || '—')));

    var digests = d.container_digests || {};
    var names = Object.keys(digests).sort();
    var ddContent;
    if (!names.length) {
      ddContent = document.createTextNode('No container digests recorded in this file.');
    } else {
      var det = el('details');
      det.appendChild(el('summary', null, names.length + ' container digests (language × condition)'));
      var ul = el('ul', 'digest-list');
      names.forEach(function (n) {
        var li = el('li');
        li.appendChild(el('span', 'dname', n));
        var hashWrap = el('span', 'dhash');
        hashWrap.appendChild(document.createTextNode(digests[n] + ' '));
        var cb = el('button', 'copybtn', 'copy');
        cb.type = 'button';
        cb.setAttribute('data-action', 'copy');
        cb.setAttribute('data-copy', digests[n]);
        cb.setAttribute('aria-label', 'Copy digest for ' + n);
        hashWrap.appendChild(cb);
        li.appendChild(hashWrap);
        ul.appendChild(li);
      });
      det.appendChild(ul);
      ddContent = det;
    }
    host.appendChild(item('Containers', ddContent));

    host.appendChild(item('Runs recorded', document.createTextNode(String(d.runs.length))));
  }

  /* ---------------- render all ---------------- */

  function renderAll() {
    renderHero();
    renderFilters();
    renderBoard();
    renderGauntlet();
    renderGap();
    renderLens();
    renderRepro();
    var chip = document.getElementById('datasrc');
    chip.textContent = 'data: ' + state.srcName;
    chip.title = 'Data source: ' + state.srcName;
  }

  /* ---------------- events ---------------- */

  function onAction(target) {
    var action = target.getAttribute('data-action');
    if (action === 'sort') {
      var key = target.getAttribute('data-key');
      if (state.sort.key === key) {
        state.sort.dir = state.sort.dir === 'asc' ? 'desc' : 'asc';
      } else {
        state.sort.key = key;
        state.sort.dir = (key === 'model' || key === 'lang' || key === 'cond') ? 'asc' : 'desc';
      }
      renderBoard();
      renderGauntlet();
    } else if (action === 'lens-open') {
      state.lens.model = target.getAttribute('data-model');
      state.lens.cond = target.getAttribute('data-cond');
      state.lens.lang = target.getAttribute('data-lang');
      if (target.getAttribute('data-tier')) state.lens.tier = target.getAttribute('data-tier');
      renderLens();
      var h = document.getElementById('lens-model-title');
      if (h) h.focus({ preventScroll: true });
      document.getElementById('lens').scrollIntoView({ behavior: REDUCED ? 'auto' : 'smooth' });
    } else if (action === 'lens-pick') {
      state.lens.model = target.getAttribute('data-model');
      state.lens.cond = null; state.lens.lang = null;
      renderLens();
    } else if (action === 'lens-cond') {
      state.lens.cond = target.getAttribute('data-value');
      state.lens.lang = null;
      renderLens();
    } else if (action === 'lens-lang') {
      state.lens.lang = target.getAttribute('data-value');
      renderLens();
    } else if (action === 'lens-cell') {
      state.lens.lang = target.getAttribute('data-lang');
      state.lens.tier = target.getAttribute('data-tier');
      renderLens();
    } else if (action === 'lens-tier') {
      state.lens.tier = target.getAttribute('data-tier');
      renderLens();
    } else if (action === 'copy') {
      copyText(target.getAttribute('data-copy'), target);
    }
  }

  function copyText(text, btn) {
    function done(ok) {
      var old = btn.textContent;
      btn.textContent = ok ? 'copied' : 'copy failed';
      setTimeout(function () { btn.textContent = old; }, 1200);
    }
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(function () { done(true); }, function () { done(false); });
    } else {
      var ta = document.createElement('textarea');
      ta.value = text;
      ta.style.position = 'fixed'; ta.style.opacity = '0';
      document.body.appendChild(ta);
      ta.select();
      var ok = false;
      try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
      document.body.removeChild(ta);
      done(ok);
    }
  }

  function setTheme(theme) {
    document.documentElement.setAttribute('data-theme', theme);
    var btn = document.getElementById('theme-toggle');
    btn.setAttribute('aria-pressed', String(theme === 'light'));
    btn.setAttribute('aria-label', theme === 'light' ? 'Switch to dark theme' : 'Switch to light theme');
    try { localStorage.setItem('nulltorch-theme', theme); } catch (e) { /* storage may be unavailable */ }
  }

  function initTheme() {
    var t = null;
    try { t = localStorage.getItem('nulltorch-theme'); } catch (e) { /* ignore */ }
    setTheme(t === 'light' ? 'light' : 'dark');
  }

  /* ---------------- data loading ---------------- */

  function validate(data) {
    return data && typeof data === 'object' && Array.isArray(data.runs);
  }

  function acceptData(data, srcName) {
    state.data = data;
    state.srcName = srcName;
    state.cond = 'open_book';
    state.lang = 'all';
    state.sort = { key: 'correct', dir: 'desc' };
    state.lens = { model: null, cond: null, lang: null, tier: 'T1' };
    document.getElementById('load-fail').hidden = true;
    renderAll();
  }

  function showLoadFail(msg) {
    var overlay = document.getElementById('load-fail');
    if (msg) document.getElementById('load-fail-msg').textContent = msg;
    overlay.hidden = false;
  }

  function loadFromUrl(url) {
    return fetch(url, { cache: 'no-store' }).then(function (resp) {
      if (!resp.ok) throw new Error('HTTP ' + resp.status + ' while fetching ' + url);
      return resp.json();
    }).then(function (data) {
      if (!validate(data)) throw new Error('The file at ' + url + ' is not a NullTorch results file.');
      return data;
    });
  }

  function loadFromFile(file) {
    var reader = new FileReader();
    reader.onload = function () {
      try {
        var data = JSON.parse(String(reader.result));
        if (!validate(data)) throw new Error('bad shape');
        acceptData(data, file.name);
      } catch (e) {
        showLoadFail('“' + file.name + '” is not a valid NullTorch results file (' + e.message + ').');
      }
    };
    reader.onerror = function () { showLoadFail('Could not read “' + file.name + '”.'); };
    reader.readAsText(file);
  }

  /* ---------------- boot ---------------- */

  function boot() {
    initTheme();

    document.addEventListener('click', function (ev) {
      var t = ev.target && ev.target.closest ? ev.target.closest('[data-action]') : null;
      if (t) onAction(t);
    });
    document.addEventListener('change', function (ev) {
      var t = ev.target;
      if (!t) return;
      if (t.getAttribute && t.getAttribute('data-filter')) {
        var name = t.getAttribute('data-filter');
        if (name === 'cond') state.cond = t.value;
        if (name === 'lang') state.lang = t.value;
        renderBoard();
        renderGauntlet();
      } else if (t.id === 'file-input' || t.id === 'file-input-2') {
        if (t.files && t.files[0]) loadFromFile(t.files[0]);
      }
    });
    document.getElementById('theme-toggle').addEventListener('click', function () {
      var cur = document.documentElement.getAttribute('data-theme');
      setTheme(cur === 'light' ? 'dark' : 'light');
    });

    var src = 'results.json';
    try {
      var q = new URLSearchParams(window.location.search).get('src');
      if (q && !/^[a-z][a-z0-9+.-]*:/i.test(q) && q.indexOf('//') !== 0) src = q;
    } catch (e) { /* no URLSearchParams — keep default */ }

    loadFromUrl(src).then(function (data) {
      acceptData(data, src);
    }).catch(function (err) {
      showLoadFail('The results file could not be loaded (' + (err && err.message ? err.message : 'unknown error') +
        '). If you opened this page via file://, the browser may block reading sibling files — pick the file manually:');
      /* render an empty dataset so the shell is still explorable */
      acceptData({ benchmark_version: '—', harness_version: '—', fixture_set_hash: '—',
                   container_digests: {}, runs: [] }, 'none loaded');
      document.getElementById('load-fail').hidden = false;
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }

})();
