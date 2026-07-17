(() => {
  "use strict";

  const TIER_KEYS = ["RVC", "T1", "T2", "T3", "T4", "T5", "T6"];
  const READING_KEYS = ["T1", "T2", "T3", "T4", "T5"];
  const GAP_KEYS = ["T1", "T2", "T3", "T4", "T5", "T6"];
  const CONDITION_LABELS = { open_book: "Open book", closed_book: "Closed book", delta: "Delta" };
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  let report = null;
  let runs = [];

  function escapeHTML(value) {
    return String(value ?? "").replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;").replaceAll('"', "&quot;").replaceAll("'", "&#39;");
  }

  function label(value) {
    return CONDITION_LABELS[value] || String(value || "unknown").replaceAll("_", " ");
  }

  function tier(run, key) {
    const value = run && run.tiers && run.tiers[key];
    if (!value || !Number.isFinite(value.pass) || !Number.isFinite(value.total)) return null;
    return value;
  }

  function aggregate(run, keys) {
    return keys.reduce((sum, key) => {
      const value = tier(run, key);
      if (value) { sum.pass += value.pass; sum.total += value.total; }
      return sum;
    }, { pass: 0, total: 0 });
  }

  function rate(result) { return result && result.total > 0 ? (result.pass / result.total) * 100 : null; }
  function percent(value, digits = 0) { return Number.isFinite(value) ? `${value.toFixed(digits)}%` : "n/a"; }
  function fraction(result) { return result && result.total > 0 ? `${result.pass}/${result.total}` : "n/a"; }
  function runName(run) { return `${run.model_id} · ${String(run.language || "unknown").toUpperCase()} · ${label(run.condition)}`; }
  function unique(values) { return [...new Set(values)].sort((a, b) => String(a).localeCompare(String(b))); }
  function disqualified(run) { return Number(run?.t6_incidents?.exec_attempts || 0) > 0; }

  function populateSelect(select, values, allLabel) {
    const previous = select.value;
    const prefix = allLabel ? `<option value="all">${escapeHTML(allLabel)}</option>` : "";
    select.innerHTML = prefix + values.map(value => `<option value="${escapeHTML(value)}">${escapeHTML(label(value))}</option>`).join("");
    if ([...select.options].some(option => option.value === previous)) select.value = previous;
  }

  function setupTheme() {
    const button = $("#theme-toggle");
    let saved = "";
    try { saved = localStorage.getItem("nulltorch-theme") || ""; } catch (_) {}
    if (saved === "light" || (!saved && matchMedia("(prefers-color-scheme: light)").matches)) document.documentElement.classList.add("light");
    const update = () => {
      const light = document.documentElement.classList.contains("light");
      button.setAttribute("aria-label", light ? "Use dark theme" : "Use light theme");
      document.querySelector('meta[name="theme-color"]').content = light ? "#f3f0e6" : "#10100f";
    };
    button.addEventListener("click", () => {
      document.documentElement.classList.toggle("light");
      try { localStorage.setItem("nulltorch-theme", document.documentElement.classList.contains("light") ? "light" : "dark"); } catch (_) {}
      update();
    });
    update();
  }

  function showView(name, focusTab = false) {
    const target = document.getElementById(name) ? name : "leaderboard";
    $$(".view").forEach(view => {
      const active = view.id === target;
      view.hidden = active === false;
      view.classList.toggle("active", active);
    });
    $$(".nav-tab").forEach(tab => {
      const active = tab.dataset.view === target;
      tab.classList.toggle("active", active);
      tab.setAttribute("aria-selected", String(active));
      tab.tabIndex = active ? 0 : -1;
      if (active && focusTab) tab.focus();
    });
    if (location.hash !== `#${target}`) history.replaceState(null, "", `#${target}`);
    window.scrollTo({ top: 0, behavior: "instant" });
  }

  function setupNavigation() {
    const tabs = $$(".nav-tab");
    tabs.forEach((tab, index) => {
      tab.addEventListener("click", () => showView(tab.dataset.view));
      tab.addEventListener("keydown", event => {
        let next = null;
        if (event.key === "ArrowRight") next = (index + 1) % tabs.length;
        if (event.key === "ArrowLeft") next = (index - 1 + tabs.length) % tabs.length;
        if (event.key === "Home") next = 0;
        if (event.key === "End") next = tabs.length - 1;
        if (next !== null) { event.preventDefault(); showView(tabs[next].dataset.view, true); }
      });
    });
    const initial = location.hash.slice(1);
    if (tabs.some(tab => tab.dataset.view === initial)) showView(initial);
  }

  function renderHero() {
    const models = unique(runs.map(run => run.model_id));
    const validRuns = runs.filter(run => !disqualified(run));
    const total = validRuns.reduce((sum, run) => {
      const score = aggregate(run, READING_KEYS);
      sum.pass += score.pass; sum.total += score.total; return sum;
    }, { pass: 0, total: 0 });
    const avg = rate(total);
    const perfectSafe = runs.filter(run => {
      const correctness = aggregate(run, READING_KEYS);
      const robust = tier(run, "T6");
      return !disqualified(run) && correctness.total > 0 && correctness.pass === correctness.total && robust && robust.total > 0 && robust.pass === robust.total;
    }).length;
    const incidentRuns = runs.filter(disqualified).length;
    const languageCount = unique(runs.map(run => run.language)).length;
    $("#hero-orbit").innerHTML = `<div class="orbit-score"><strong>${percent(avg)}</strong><span>eligible correctness</span></div><span class="orbit-tag">${models.length} model${models.length === 1 ? "" : "s"}</span><span class="orbit-tag">${runs.length} run${runs.length === 1 ? "" : "s"}</span>`;
    $("#story-strip").innerHTML = `<div class="story-stat"><strong>${perfectSafe}</strong><span>runs achieved perfect reading and robustness</span></div><div class="story-stat"><strong>${incidentRuns}</strong><span>runs disqualified by a hostile execution attempt</span></div><div class="story-stat"><strong>${languageCount}</strong><span>implementation language${languageCount === 1 ? "" : "s"} in the field</span></div>`;
    $("#run-count").textContent = runs.length ? `${runs.length} runs / ${models.length} models` : "No runs reported";
  }

  function tierCell(run, key, isRvc = false) {
    const value = tier(run, key);
    const valueRate = rate(value);
    const perfect = value && value.total > 0 && value.pass === value.total;
    return `<td class="tier-cell${isRvc ? " rvc-cell" : ""}${perfect ? " perfect" : ""}" aria-label="${key} ${fraction(value)}, ${percent(valueRate, 1)}">${fraction(value)}<span class="sr-only"> (${percent(valueRate, 1)})</span></td>`;
  }

  function renderLeaderboard() {
    const language = $("#language-filter").value;
    const condition = $("#condition-filter").value;
    const sort = $("#sort-filter").value;
    let filtered = runs.filter(run => (language === "all" || run.language === language) && (condition === "all" || run.condition === condition));
    filtered.sort((a, b) => {
      if (sort === "model") return String(a.model_id).localeCompare(String(b.model_id));
      const keys = sort === "robustness" ? ["T6"] : READING_KEYS;
      return (rate(aggregate(b, keys)) ?? -1) - (rate(aggregate(a, keys)) ?? -1) || String(a.model_id).localeCompare(String(b.model_id));
    });
    $("#leaderboard-head").innerHTML = `<tr><th scope="col"><button class="sort-button" type="button" data-sort="model">Run <span aria-hidden="true">↕</span></button></th><th scope="col"><button class="sort-button" type="button" data-sort="correctness">Correctness <span aria-hidden="true">↕</span></button></th><th scope="col" class="rvc-head">RVC<br><small>engine</small></th>${["T1", "T2", "T3", "T4", "T5", "T6"].map(key => `<th scope="col">${key}</th>`).join("")}<th scope="col"><button class="sort-button" type="button" data-sort="robustness">Robustness <span aria-hidden="true">↕</span></button></th><th scope="col">Verdict</th></tr>`;
    $("#leaderboard-body").innerHTML = filtered.map((run, index) => {
      const correctness = aggregate(run, READING_KEYS);
      const robust = tier(run, "T6");
      return `<tr data-model="${escapeHTML(run.model_id)}" data-language="${escapeHTML(run.language)}" data-condition="${escapeHTML(run.condition)}"><td class="model-cell"><span class="rank">${String(index + 1).padStart(2, "0")}</span><span class="model-name">${escapeHTML(run.model_id)}</span><span class="run-meta">${escapeHTML(run.language)} / ${escapeHTML(label(run.condition))}</span></td><td class="score-cell" aria-label="Correctness ${fraction(correctness)}, ${percent(rate(correctness), 1)}"><strong>${percent(rate(correctness), 1)}</strong><small>${fraction(correctness)}</small></td>${tierCell(run, "RVC", true)}${["T1", "T2", "T3", "T4", "T5", "T6"].map(key => tierCell(run, key)).join("")}<td class="score-cell" aria-label="Robustness ${fraction(robust)}, ${percent(rate(robust), 1)}"><strong>${percent(rate(robust), 1)}</strong><small>${fraction(robust)}</small></td><td><span class="status-badge${disqualified(run) ? " dq" : ""}">${disqualified(run) ? "Disqualified" : "Eligible"}</span></td></tr>`;
    }).join("");
    $("#leaderboard-empty").hidden = filtered.length > 0;
    $$(".sort-button", $("#leaderboard-head")).forEach(button => button.addEventListener("click", () => { $("#sort-filter").value = button.dataset.sort; renderLeaderboard(); }));
  }

  function heatCell(run, key) {
    const value = tier(run, key);
    const valueRate = rate(value);
    return `<td class="heat-cell${key === "RVC" ? " rvc-cell" : ""}" style="--p:${valueRate ?? 0}" aria-label="${escapeHTML(runName(run))}, ${key}: ${fraction(value)}, ${percent(valueRate, 1)}"><strong>${fraction(value)}</strong><br><small>${percent(valueRate, 1)}</small></td>`;
  }

  function renderProfileFor(model) {
    const modelRuns = runs.filter(run => run.model_id === model);
    if (!modelRuns.length) return `<p class="empty-message">No model results are available.</p>`;
    const eligible = modelRuns.filter(run => !disqualified(run));
    const total = eligible.reduce((sum, run) => { const score = aggregate(run, READING_KEYS); sum.pass += score.pass; sum.total += score.total; return sum; }, { pass: 0, total: 0 });
    const robust = eligible.reduce((sum, run) => { const score = tier(run, "T6"); if (score) { sum.pass += score.pass; sum.total += score.total; } return sum; }, { pass: 0, total: 0 });
    const columns = [...modelRuns].sort((a, b) => String(a.language).localeCompare(String(b.language)) || String(a.condition).localeCompare(String(b.condition)));
    return `<article class="model-card" aria-label="${escapeHTML(model)} profile"><div class="profile-head"><div class="profile-title"><p class="eyebrow">Reader profile</p><h2>${escapeHTML(model)}</h2><p>${modelRuns.length} run${modelRuns.length === 1 ? "" : "s"} across ${unique(modelRuns.map(run => run.language)).length} language${unique(modelRuns.map(run => run.language)).length === 1 ? "" : "s"}.</p></div><div class="profile-score"><div><strong>${percent(rate(total), 1)}</strong><span>correctness</span></div><div><strong>${percent(rate(robust), 1)}</strong><span>robustness</span></div></div></div><div class="heatmap-shell" tabindex="0" aria-label="${escapeHTML(model)} tier heatmap"><table class="heatmap-table"><thead><tr><th scope="col">Tier</th>${columns.map(run => `<th scope="col">${escapeHTML(run.language)}<br><small>${escapeHTML(label(run.condition))}</small></th>`).join("")}</tr></thead><tbody>${TIER_KEYS.map(key => `<tr><th scope="row" class="${key === "RVC" ? "rvc-head" : ""}">${key}${key === "RVC" ? "<br><small>engine fidelity</small>" : ""}</th>${columns.map(run => heatCell(run, key)).join("")}</tr>`).join("")}</tbody></table></div></article>`;
  }

  function renderModels() {
    const primary = $("#model-select").value;
    const comparison = $("#compare-select").value;
    if (!primary) { $("#model-profile").innerHTML = `<p class="empty-message">No model runs have been reported yet. The profile view will appear when data arrives.</p>`; return; }
    $("#model-profile").innerHTML = renderProfileFor(primary) + (comparison ? `<p class="compare-note">Side-by-side comparison: each table uses its run’s own reported denominator, so missing tiers remain n/a.</p>${renderProfileFor(comparison)}` : (unique(runs.map(run => run.model_id)).length === 1 ? `<p class="compare-note">This dataset contains one model, so a second profile is not available for comparison.</p>` : ""));
  }

  function renderGap() {
    const pairs = [];
    runs.filter(run => run.condition === "delta").forEach(delta => {
      const stock = runs.find(run => run.model_id === delta.model_id && run.language === delta.language && run.condition === "open_book") || runs.find(run => run.model_id === delta.model_id && run.language === delta.language && run.condition === "closed_book");
      const stockRate = rate(aggregate(stock, GAP_KEYS));
      const deltaRate = rate(aggregate(delta, GAP_KEYS));
      if (stock && stockRate !== null && deltaRate !== null) pairs.push({ stock, delta, stockRate, deltaRate, gap: stockRate - deltaRate });
    });
    pairs.sort((a, b) => Math.abs(a.gap) - Math.abs(b.gap));
    $("#gap-chart").innerHTML = pairs.map(pair => {
      const left = Math.min(pair.stockRate, pair.deltaRate);
      const width = Math.abs(pair.stockRate - pair.deltaRate);
      return `<article class="gap-row" data-model="${escapeHTML(pair.stock.model_id)}" data-language="${escapeHTML(pair.stock.language)}"><div class="gap-label"><strong>${escapeHTML(pair.stock.model_id)}</strong><span>${escapeHTML(pair.stock.language)} / ${escapeHTML(label(pair.stock.condition))} vs delta</span></div><div class="gap-track" aria-label="${escapeHTML(pair.stock.model_id)} ${escapeHTML(pair.stock.language)}: stock ${percent(pair.stockRate, 1)}, delta ${percent(pair.deltaRate, 1)}, gap ${pair.gap.toFixed(1)} percentage points"><i class="gap-line" style="left:${left}%;width:${width}%"></i><i class="gap-point" style="left:${pair.stockRate}%"></i><i class="gap-point delta" style="left:${pair.deltaRate}%"></i></div><div class="gap-value">${pair.gap > 0 ? "+" : ""}${pair.gap.toFixed(1)}<span>percentage-point gap</span></div></article>`;
    }).join("");
    $("#gap-empty").hidden = pairs.length > 0;
  }

  function renderFixtureSelectors() {
    const select = $("#fixture-run-select");
    const previous = select.value;
    select.innerHTML = runs.map((run, index) => `<option value="${index}">${escapeHTML(runName(run))}</option>`).join("");
    if ([...select.options].some(option => option.value === previous)) select.value = previous;
    select.disabled = runs.length === 0;
    renderFixtureTiers();
  }

  function renderFixtureTiers() {
    const run = runs[Number($("#fixture-run-select").value)];
    const select = $("#fixture-tier-select");
    const previous = select.value;
    const keys = run ? TIER_KEYS.filter(key => tier(run, key)) : [];
    select.innerHTML = keys.map(key => `<option value="${key}">${key}${key === "RVC" ? " — engine fidelity" : ""}</option>`).join("");
    if (keys.includes(previous)) select.value = previous;
    select.disabled = keys.length === 0;
    renderFixtureDetail();
  }

  function renderFixtureDetail() {
    const run = runs[Number($("#fixture-run-select").value)];
    const key = $("#fixture-tier-select").value;
    const result = tier(run, key);
    if (!run || !result) { $("#fixture-detail").innerHTML = `<p class="empty-message">No fixture results are available yet.</p>`; return; }
    const categories = Object.entries(result.categories || {}).sort(([a], [b]) => a.localeCompare(b));
    $("#fixture-detail").innerHTML = `<div class="fixture-summary"><div class="fixture-total"><p class="eyebrow">${escapeHTML(runName(run))}</p><strong>${fraction(result)}</strong><span>${key} passed · ${percent(rate(result), 1)}</span>${key === "RVC" ? `<p class="compare-note">RVC measures engine fidelity and is not included in reading correctness.</p>` : ""}</div><div class="category-list">${categories.length ? categories.map(([name, score]) => { const valueRate = rate(score); return `<div class="category-row${score.pass < score.total ? " fail" : ""}${key === "RVC" ? " rvc" : ""}" data-category="${escapeHTML(name)}"><div class="category-name"><strong>${escapeHTML(name)}</strong><span><i style="--p:${valueRate ?? 0}"></i></span></div><div class="category-score" aria-label="${escapeHTML(name)}: ${fraction(score)}">${fraction(score)}</div></div>`; }).join("") : `<p class="empty-message">This tier has no category-level fixtures.</p>`}</div></div>`;
  }

  async function copyText(button) {
    const text = button.dataset.copy || "";
    try {
      await navigator.clipboard.writeText(text);
      const original = button.textContent; button.textContent = "Copied"; setTimeout(() => { button.textContent = original; }, 1200);
    } catch (_) {
      button.textContent = "Select hash";
      const code = button.previousElementSibling;
      if (code) { const selection = window.getSelection(); const range = document.createRange(); range.selectNodeContents(code); selection.removeAllRanges(); selection.addRange(range); }
    }
  }

  function renderProvenance() {
    const fixture = report?.fixture_set_hash || "Not reported";
    $("#provenance").innerHTML = `<div class="provenance-card"><span>Benchmark version</span><strong>${escapeHTML(report?.benchmark_version || "Not reported")}</strong></div><div class="provenance-card"><span>Harness version</span><strong>${escapeHTML(report?.harness_version || "Not reported")}</strong></div><div class="provenance-card"><span>Fixture set hash</span><code title="${escapeHTML(fixture)}">${escapeHTML(fixture)}</code></div><div class="provenance-card"><span>Runs / models</span><strong>${runs.length} / ${unique(runs.map(run => run.model_id)).length}</strong></div>`;
    const digests = Object.entries(report?.container_digests || {}).sort(([a], [b]) => a.localeCompare(b));
    $("#digests").innerHTML = digests.length ? digests.map(([name, digest]) => `<div class="digest-row"><strong>${escapeHTML(name)}</strong><code title="${escapeHTML(digest)}">${escapeHTML(digest)}</code><button class="copy-button" type="button" data-copy="${escapeHTML(digest)}" aria-label="Copy ${escapeHTML(name)} digest">Copy hash</button></div>`).join("") : `<p class="empty-message">No container digests were reported for this benchmark.</p>`;
    $$(".copy-button", $("#digests")).forEach(button => button.addEventListener("click", () => copyText(button)));
  }

  function renderAll() {
    const languages = unique(runs.map(run => run.language));
    const conditions = unique(runs.map(run => run.condition));
    const models = unique(runs.map(run => run.model_id));
    const previousComparison = $("#compare-select").value;
    populateSelect($("#language-filter"), languages, "All languages");
    populateSelect($("#condition-filter"), conditions, "All conditions");
    populateSelect($("#model-select"), models);
    populateSelect($("#compare-select"), models);
    $("#compare-select").insertAdjacentHTML("afterbegin", `<option value="">No comparison</option>`);
    $("#compare-select").value = models.includes(previousComparison) ? previousComparison : "";
    $("#compare-select").disabled = models.length < 2;
    renderHero(); renderLeaderboard(); renderModels(); renderGap(); renderFixtureSelectors(); renderProvenance();
    $("#footer-version").textContent = `Benchmark ${report?.benchmark_version || "—"} / Harness ${report?.harness_version || "—"}`;
  }

  function setupControls() {
    ["language-filter", "condition-filter", "sort-filter"].forEach(id => $(`#${id}`).addEventListener("change", renderLeaderboard));
    $("#model-select").addEventListener("change", () => { if ($("#compare-select").value === $("#model-select").value) $("#compare-select").value = ""; renderModels(); });
    $("#compare-select").addEventListener("change", renderModels);
    $("#fixture-run-select").addEventListener("change", renderFixtureTiers);
    $("#fixture-tier-select").addEventListener("change", renderFixtureDetail);
  }

  async function load() {
    try {
      const response = await fetch("./results.json", { cache: "no-store" });
      if (!response.ok) throw new Error(`Results file returned ${response.status}`);
      const data = await response.json();
      report = data && typeof data === "object" ? data : {};
      runs = Array.isArray(report.runs) ? report.runs.filter(run => run && typeof run === "object") : [];
      renderAll();
    } catch (_) {
      report = {}; runs = []; renderAll();
      $("#leaderboard-empty").hidden = false;
      $("#leaderboard-empty").textContent = "results.json could not be read. Serve this folder locally or place the results file beside index.html.";
    } finally { $("#loading").hidden = true; }
  }

  setupTheme(); setupNavigation(); setupControls(); load();
})();
