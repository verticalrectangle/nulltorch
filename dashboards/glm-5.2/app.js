/* NullTorch Board — vanilla JS, zero dependencies */
"use strict";
(function(){

/* ---------- constants ---------- */
var TIERS = ["T1","T2","T3","T4","T5","T6"];
var TIER_LABEL = {
  T1:"T1 · flat tensors", T2:"T2 · nested", T3:"T3 · views",
  T4:"T4 · exotic dtypes", T5:"T5 · resource envelope", T6:"T6 · hostile",
  RVC:"RVC · engine fidelity"
};
var TIER_COLOR = {T1:"#34d399",T2:"#22d3ee",T3:"#818cf8",T4:"#c084fc",T5:"#f472b6",T6:"#ef4444",RVC:"#a78bfa"};
var COND_LABEL = {open_book:"open book", closed_book:"closed book", delta:"delta"};
var LANG_LABEL = {go:"Go", rust:"Rust", cpp:"C++"};

/* ---------- state ---------- */
var DATA = null;
var S = {
  tab:"overview",
  lb:{lang:null, cond:null, sort:"corr", dir:-1},
  md:{model:null, cond:null, lang:null, itLang:null, itCond:null},
  gap:{sort:"gap", dir:-1},
  cmp:{set:{}, lang:null, cond:null},
  fx:{model:null, lang:null, cond:null},
  theme:"dark"
};

/* ---------- utils ---------- */
function $(id){return document.getElementById(id)}
function el(tag, cls, txt){var e=document.createElement(tag); if(cls)e.className=cls; if(txt!=null)e.textContent=txt; return e}
function clear(n){while(n.firstChild)n.removeChild(n.firstChild)}
function fmtPct(r){if(r==null||isNaN(r))return "—"; return Math.round(r*1000)/10 + "%"}
function fmtPct1(r){if(r==null||isNaN(r))return "—"; return (Math.round(r*1000)/10).toFixed(1) + "%"}
function rate(t){if(!t||!t.total)return null; return t.pass/t.total}
function tierRate(run,t){return run&&run.tiers[t]?rate(run.tiers[t]):null}
function disqualified(run){return run&&run.t6_incidents&&run.t6_incidents.exec_attempts>0}
function key(model,lang,cond){return model+"|"+lang+"|"+cond}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return {"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"}[c]})}
function toast(msg){var t=$("toast"); t.textContent=msg; t.classList.add("show"); t.hidden=false; clearTimeout(t._t); t._t=setTimeout(function(){t.classList.remove("show"); setTimeout(function(){t.hidden=true},250)},1600)}

/* ---------- index ---------- */
function buildIndex(data){
  var idx={byKey:{}, byModel:{}, langs:[], conds:[], models:[]};
  var lm={}, cm={}, mm={};
  (data.runs||[]).forEach(function(r){
    var k=key(r.model_id,r.language,r.condition);
    idx.byKey[k]=r;
    if(!mm[r.model_id]){mm[r.model_id]=true; idx.models.push(r.model_id)}
    if(!lm[r.language]){lm[r.language]=true; idx.langs.push(r.language)}
    if(!cm[r.condition]){cm[r.condition]=true; idx.conds.push(r.condition)}
    var mb=idx.byModel[r.model_id]||(idx.byModel[r.model_id]={});
    mb[k]=r;
  });
  idx.langs.sort(); idx.conds.sort(function(a,b){var o={open_book:0,closed_book:1,delta:2};return (o[a]||3)-(o[b]||3)}); idx.models.sort();
  return idx;
}
function IDX(){return buildIndex(DATA)}
function runFor(model,lang,cond){return DATA ? buildIndex(DATA).byKey[key(model,lang,cond)] : null}
function stockRun(model,lang){var i=buildIndex(DATA); return (i.byKey[key(model,lang,"open_book")])||(i.byKey[key(model,lang,"closed_book")])||null}
function deltaRun(model,lang){return DATA?buildIndex(DATA).byKey[key(model,lang,"delta")]:null}

/* aggregate T1-T6 pass/total for a run (RVC excluded) */
function readingTotals(run){
  var p=0,t=0; if(!run)return {pass:0,total:0,rate:null};
  TIERS.forEach(function(k){var tr=run.tiers[k]; if(tr){p+=tr.pass; t+=tr.total}});
  return {pass:p,total:t,rate:t? p/t: null};
}
function correctness(run){ // T1-T5
  var p=0,t=0; if(!run)return null;
  for(var i=0;i<5;i++){var tr=run.tiers[TIERS[i]]; if(tr){p+=tr.pass; t+=tr.total}}
  return t? p/t: null;
}
function gapFor(model,lang){
  var s=stockRun(model,lang), d=deltaRun(model,lang);
  if(!s||!d)return null;
  var sr=readingTotals(s).rate, dr=readingTotals(d).rate;
  if(sr==null||dr==null)return null;
  return sr-dr;
}

/* ---------- color ---------- */
function heatColor(r, tier){
  if(r==null||isNaN(r))return null;
  if(tier==="RVC"){var l=38+r*16; return "hsl(265,55%,"+l+"%)"}
  var hue=r*135; return "hsl("+hue+",58%,42%)"
}
function textColorFor(r){
  if(r==null)return "var(--faint)";
  return r<0.42 ? "#fff" : "#0a0a0f";
}

/* ---------- data loading ---------- */
function load(data){
  DATA=data;
  try{localStorage.setItem("nt_theme", S.theme)}catch(e){}
  renderAll();
}
function fetchData(){
  fetch("results.json",{cache:"no-store"}).then(function(r){
    if(!r.ok)throw new Error("http "+r.status);
    return r.json();
  }).then(function(d){load(d)}).catch(function(err){
    showLoadFailed(err);
  });
}
function showLoadFailed(err){
  var main=$("main");
  clear(main);
  var sec=el("section","view"); sec.dataset.view="overview";
  var es=el("div","empty-state");
  es.innerHTML="<h2>Couldn't load <code>results.json</code></h2>"+
    "<p>"+esc(String(err&&err.message||err||"unknown error"))+". This is expected when opening the file directly with <code>file://</code>. Pick a local results file to view it offline.</p>";
  var dz=el("div","dropzone"); dz.tabIndex=0; dz.setAttribute("role","button"); dz.setAttribute("aria-label","Load a results JSON file");
  dz.innerHTML="<div class='dz-icon'>📂</div><div class='dz-main'>Choose or drop a results.json</div><div class='dz-sub'>parsed locally in your browser — nothing is uploaded</div>";
  var inp=el("input"); inp.type="file"; inp.accept="application/json,.json";
  dz.appendChild(inp);
  dz.addEventListener("click",function(){inp.click()});
  dz.addEventListener("keydown",function(e){if(e.key==="Enter"||e.key===" "){e.preventDefault();inp.click()}});
  dz.addEventListener("dragover",function(e){e.preventDefault();dz.classList.add("drag")});
  dz.addEventListener("dragleave",function(){dz.classList.remove("drag")});
  dz.addEventListener("drop",function(e){e.preventDefault();dz.classList.remove("drag"); if(e.dataTransfer.files[0])readFile(e.dataTransfer.files[0])});
  inp.addEventListener("change",function(){if(inp.files[0])readFile(inp.files[0])});
  es.appendChild(dz);
  sec.appendChild(es);
  main.appendChild(sec);
}
function readFile(file){
  var fr=new FileReader();
  fr.onload=function(){try{load(JSON.parse(fr.result))}catch(e){toast("Invalid JSON: "+e.message)}};
  fr.onerror=function(){toast("Could not read file")};
  fr.readAsText(file);
}

/* ---------- render dispatch ---------- */
function renderAll(){
  renderOverview();
  renderLeaderboard();
  renderModelDetail();
  renderGap();
  renderCompare();
  renderFixtures();
  switchTab(S.tab, true);
}

/* ---------- tabs ---------- */
function switchTab(name, initial){
  S.tab=name;
  document.querySelectorAll(".tab").forEach(function(t){t.setAttribute("aria-selected", t.dataset.tab===name?"true":"false")});
  document.querySelectorAll(".view").forEach(function(v){v.hidden = v.dataset.view!==name});
  if(!initial) window.scrollTo({top:0, behavior:"smooth"});
}

/* ---------- overview ---------- */
function renderOverview(){
  var idx=IDX();
  var ov=$("ovStats"), ver=$("ovVersion");
  ver.textContent="benchmark v"+(DATA.benchmark_version||"—");
  if(!idx.models.length){
    clear(ov);
    var es=el("div","empty-state");
    es.style.padding="20px";
    es.innerHTML="<h2>No runs in this dataset</h2><p>The loaded <code>results.json</code> contains zero runs. Everything else still renders — pick another dataset to see results.</p>";
    ov.appendChild(es);
    $("ovTop").innerHTML="<p class='muted' style='padding:8px 0'>No data.</p>";
    $("ovIncidents").innerHTML="<p class='muted' style='padding:8px 0'>No data.</p>";
    $("ovTierBars").innerHTML="<p class='muted' style='padding:8px 0'>No data.</p>";
    renderRepro();
    return;
  }
  // stats
  var runs=DATA.runs;
  var nQual=0, nDQ=0, bestCorr=-1, bestModel=null, bestLang=null, bestCond=null;
  runs.forEach(function(r){if(disqualified(r))nDQ++; else nQual++; var c=correctness(r); if(c!=null&&c>bestCorr){bestCorr=c;bestModel=r.model_id;bestLang=r.language;bestCond=r.condition}});
  clear(ov);
  ov.appendChild(statCard(idx.models.length, "models tested"));
  ov.appendChild(statCard(idx.langs.length, "languages", idx.langs.map(function(l){return LANG_LABEL[l]||l}).join(" · ")));
  ov.appendChild(statCard(runs.length, "runs", idx.conds.map(function(c){return COND_LABEL[c]||c}).join(" · ")));
  var qCard=statCard(nQual, "qualified", nDQ? (nDQ+" disqualified (exec'd a hostile pickle)"):"no disqualifications");
  qCard.querySelector(".stat-bar").appendChild(bar(nQual/(nQual+nDQ||1),"var(--good)"));
  ov.appendChild(qCard);
  if(bestModel){
    var bc=statCard(fmtPct1(bestCorr), "best correctness", bestModel+" · "+(LANG_LABEL[bestLang]||bestLang)+" · "+(COND_LABEL[bestCond]||bestCond), true);
    ov.appendChild(bc);
  }
  // top performers
  var ranked=idx.models.map(function(m){
    var best=-1,bl=null,bc=null;
    idx.langs.forEach(function(l){idx.conds.forEach(function(c){var r=runFor(m,l,c); if(!r||disqualified(r))return; var v=correctness(r); if(v!=null&&v>best){best=v;bl=l;bc=c}})});
    return {m:m, corr:best, lang:bl, cond:bc, dq: idx.conds.some(function(c){return idx.langs.some(function(l){var r=runFor(m,l,c); return r&&disqualified(r)})})}
  }).filter(function(o){return o.corr>=0}).sort(function(a,b){return b.corr-a.corr});
  var top=$("ovTop"); clear(top);
  ranked.slice(0,5).forEach(function(o,i){
    var row=el("div","ov-row"); row.style.cssText="display:flex;align-items:center;gap:10px;padding:8px 0;border-bottom:1px solid var(--line)";
    var rk=el("span","faint"); rk.style.cssText="width:18px;font-weight:700"; rk.textContent=(i+1);
    var nm=el("span"); nm.style.cssText="font-weight:700;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap"; nm.textContent=o.m;
    if(o.dq){var b=el("span","badge dq"); b.style.marginLeft="6px"; b.innerHTML='<span class="dot"></span>DQ'; nm.appendChild(b)}
    var v=el("span"); v.style.cssText="font-weight:800;font-variant-numeric:tabular-nums;color:var(--amber)"; v.textContent=fmtPct1(o.corr);
    row.appendChild(rk); row.appendChild(nm); row.appendChild(v); top.appendChild(row);
  });
  if(!ranked.length) top.innerHTML="<p class='muted' style='padding:8px 0'>No qualified runs.</p>";
  // incidents
  var inc=$("ovIncidents"); clear(inc);
  var totCrash=0,totHang=0,totExec=0;
  runs.forEach(function(r){totCrash+=r.t6_incidents.crashes; totHang+=r.t6_incidents.hangs; totExec+=r.t6_incidents.exec_attempts});
  var ig=el("div","incident-grid");
  ig.appendChild(incidentBox(totExec, "exec attempts", totExec>0?"bad":"ok"));
  ig.appendChild(incidentBox(totCrash, "crashes", totCrash>0?"bad":"ok"));
  ig.appendChild(incidentBox(totHang, "hangs", totHang>0?"bad":"ok"));
  inc.appendChild(ig);
  var note=el("p","muted"); note.style.cssText="margin:12px 0 0;font-size:12.5px";
  note.textContent = totExec>0 ? "One or more models executed a hostile pickle — those runs are disqualified." : "No model executed a hostile pickle. T6 robustness is the only safety signal.";
  inc.appendChild(note);
  // tier landscape
  var tb=$("ovTierBars"); clear(tb);
  var allTiers=TIERS.concat(["RVC"]);
  allTiers.forEach(function(t){
    var sum=0,cnt=0;
    runs.forEach(function(r){var tr=r.tiers[t]; if(tr&&tr.total){sum+=tr.pass/tr.total; cnt++}});
    var mean=cnt? sum/cnt: null;
    var row=el("div"); row.style.cssText="display:flex;align-items:center;gap:10px;margin-bottom:8px";
    var lab=el("span"); lab.style.cssText="width:46px;font-size:12px;font-weight:700;color:"+(TIER_COLOR[t]); lab.textContent=t;
    var track=el("div","gap-track"); track.style.cssText="flex:1;height:8px;border-radius:99px;background:var(--line);overflow:hidden";
    var bar=el("i"); bar.style.cssText="display:block;height:100%;border-radius:99px;background:"+(TIER_COLOR[t]); bar.style.width=(mean==null?0:mean*100)+"%";
    track.appendChild(bar);
    var v=el("span"); v.style.cssText="width:52px;text-align:right;font-size:12px;font-weight:700;font-variant-numeric:tabular-nums"; v.textContent=fmtPct1(mean);
    row.appendChild(lab); row.appendChild(track); row.appendChild(v); tb.appendChild(row);
  });
  renderRepro();
}
function statCard(val,lbl,sub,small){
  var c=el("div","stat");
  var v=el("div","stat-val"+(small?" sm":"")); v.textContent=val; c.appendChild(v);
  var l=el("div","stat-lbl"); l.textContent=lbl; c.appendChild(l);
  if(sub){var s=el("div","stat-lbl"); s.style.color="var(--faint)"; s.style.marginTop="2px"; s.textContent=sub; c.appendChild(s)}
  var b=el("div","stat-bar"); c.appendChild(b);
  return c;
}
function bar(r,color){var i=el("i"); i.style.cssText="display:block;height:100%;width:"+(r*100)+"%;background:"+(color||"var(--amber)"); return i}
function incidentBox(v,l,cls){var d=el("div","incident "+cls); d.innerHTML='<div class="v">'+v+'</div><div class="l">'+l+'</div>'; return d}

function renderRepro(){
  var box=$("reproStrip"); clear(box);
  if(!DATA){box.hidden=true; return}
  box.hidden=false;
  box.appendChild(reproField("benchmark version", DATA.benchmark_version));
  box.appendChild(reproField("harness version", DATA.harness_version));
  box.appendChild(reproHashField("fixture set hash", DATA.fixture_set_hash));
  var digests=DATA.container_digests||{};
  var dkeys=Object.keys(digests);
  var dField=el("div","repro-field");
  dField.innerHTML='<div class="repro-label">container digests</div><div class="repro-value">'+dkeys.length+' image'+(dkeys.length===1?"":"s")+'</div>';
  if(dkeys.length){
    var tg=el("button","digests-toggle"); tg.textContent="show digests"; tg.setAttribute("aria-expanded","false");
    var list=el("div","digest-list");
    dkeys.sort().forEach(function(k){
      var r=el("div","digest-row");
      r.innerHTML='<span class="k">'+esc(k)+'</span><span class="v" title="'+esc(digests[k])+'">'+esc(digests[k])+'</span>';
      var cp=el("button","copy-btn"); cp.textContent="copy"; cp.addEventListener("click",function(){copyText(digests[k])});
      r.appendChild(cp); list.appendChild(r);
    });
    tg.addEventListener("click",function(){var open=list.classList.toggle("open"); tg.setAttribute("aria-expanded",open?"true":"false"); tg.textContent=open?"hide digests":"show digests"});
    dField.appendChild(tg); dField.appendChild(list);
  }
  box.appendChild(dField);
}
function reproField(label,val){
  var f=el("div","repro-field");
  f.innerHTML='<div class="repro-label">'+esc(label)+'</div><div class="repro-value">'+esc(val||"—")+'</div>';
  return f;
}
function reproHashField(label,val){
  var f=el("div","repro-field");
  var lab=el("div","repro-label"); lab.textContent=label; f.appendChild(lab);
  if(!val){var v=el("div","repro-value"); v.textContent="—"; f.appendChild(v); return f}
  var v=el("div","repro-value");
  var trunc=el("span","trunc"); trunc.textContent=val; v.appendChild(trunc);
  var cp=el("button","copy-btn"); cp.textContent="copy"; cp.addEventListener("click",function(){copyText(val)});
  v.appendChild(cp);
  var full=el("button","copy-btn"); full.textContent="full"; full.addEventListener("click",function(){openHashDialog(label,val)});
  v.appendChild(full);
  f.appendChild(v);
  return f;
}
function copyText(t){if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(t).then(function(){toast("Copied")},function(){fallbackCopy(t)})}else{fallbackCopy(t)}}
function fallbackCopy(t){var ta=document.createElement("textarea"); ta.value=t; ta.style.position="fixed"; ta.style.opacity="0"; document.body.appendChild(ta); ta.select(); try{document.execCommand("copy");toast("Copied")}catch(e){toast("Copy failed")}document.body.removeChild(ta)}
function openHashDialog(label,val){
  var d=$("hashDialog"); $("hashDialogTitle").textContent=label;
  $("hashDialogValue").textContent=val;
  d.showModal();
}
$("hashDialogClose").addEventListener("click",function(){$("hashDialog").close()});
$("hashDialogCopy").addEventListener("click",function(){copyText($("hashDialogValue").textContent)});
$("hashDialog").addEventListener("click",function(e){if(e.target===$("hashDialog"))$("hashDialog").close()});

/* ---------- leaderboard ---------- */
function renderLeaderboard(){
  var idx=IDX();
  var fbox=$("lbFilters"); clear(fbox);
  if(!idx.models.length){fbox.parentNode.style.display="none"; var tb=$("lbTable"); tb.querySelector("thead").innerHTML=""; tb.querySelector("tbody").innerHTML='<tr><td class="muted" style="padding:24px">No runs.</td></tr>'; $("lbLegend").innerHTML=""; return}
  fbox.parentNode.style.display="";
  // language selector
  if(!S.lb.lang||idx.langs.indexOf(S.lb.lang)<0)S.lb.lang=idx.langs[0];
  if(!S.lb.cond||idx.conds.indexOf(S.lb.cond)<0)S.lb.cond=idx.conds.indexOf("open_book")>=0?"open_book":idx.conds[0];
  fbox.appendChild(filtLabel("language"));
  fbox.appendChild(seg(idx.langs.map(function(l){return {v:l,l:LANG_LABEL[l]||l}}), S.lb.lang, function(v){S.lb.lang=v; renderLeaderboard()}));
  fbox.appendChild(filtLabel("condition"));
  fbox.appendChild(seg(idx.conds.map(function(c){return {v:c,l:COND_LABEL[c]||c}}), S.lb.cond, function(v){S.lb.cond=v; renderLeaderboard()}));
  // table
  var cols=[
    {k:"model",l:"Model",cls:""},
    {k:"T1",l:"T1"},{k:"T2",l:"T2"},{k:"T3",l:"T3"},{k:"T4",l:"T4"},{k:"T5",l:"T5"},
    {k:"T6",l:"T6"},{k:"RVC",l:"RVC"},
    {k:"corr",l:"Correct",cls:"num"},{k:"status",l:"Status",cls:""}
  ];
  var thead=$("lbTable").querySelector("thead"); clear(thead);
  var tr=el("tr");
  cols.forEach(function(c){
    var th=el("th",c.cls); if(c.k!=="model"&&c.k!=="status")th.className=(c.cls||"")+" sortable";
    th.setAttribute("scope","col");
    if(c.k!=="model"&&c.k!=="status"){th.tabIndex=0; th.setAttribute("role","columnheader")}
    var span=el("span"); span.textContent=c.l;
    th.appendChild(span);
    if(c.k!=="model"&&c.k!=="status"){var ar=el("span","arrow"); ar.textContent="▲"; th.appendChild(ar);
      if(S.lb.sort===c.k)th.classList.add(S.lb.dir<0?"sort-desc":"sort-asc");
      th.addEventListener("click",function(){sortLb(c.k)});
      th.addEventListener("keydown",function(e){if(e.key==="Enter"||e.key===" "){e.preventDefault();sortLb(c.k)}});
    }
    tr.appendChild(th);
  });
  thead.appendChild(tr);
  // rows
  var rows=idx.models.map(function(m){
    var r=runFor(m,S.lb.lang,S.lb.cond);
    var dq=idx.conds.some(function(c){return idx.langs.some(function(l){var rr=runFor(m,l,c); return rr&&disqualified(rr)})});
    return {m:m, r:r, dq:dq,
      T1:tierRate(r,"T1"),T2:tierRate(r,"T2"),T3:tierRate(r,"T3"),T4:tierRate(r,"T4"),T5:tierRate(r,"T5"),T6:tierRate(r,"T6"),
      RVC: r&&r.tiers.RVC? rate(r.tiers.RVC): null,
      corr: r?correctness(r):null
    };
  });
  rows.sort(function(a,b){return cmpRows(a,b,S.lb.sort,S.lb.dir)});
  var tbody=$("lbTable").querySelector("tbody"); clear(tbody);
  rows.forEach(function(o){
    var tr=el("tr");
    var tdM=el("td","model",o.m); tdM.tabIndex=0; tdM.setAttribute("role","button"); tdM.title="Open model detail";
    tdM.addEventListener("click",function(){openModel(o.m)});
    tdM.addEventListener("keydown",function(e){if(e.key==="Enter"||e.key===" "){e.preventDefault();openModel(o.m)}});
    tr.appendChild(tdM);
    ["T1","T2","T3","T4","T5","T6","RVC"].forEach(function(t){
      var td=el("td","num"); td.appendChild(rateCell(o[t],t)); tr.appendChild(td);
    });
    var tdC=el("td","num"); tdC.appendChild(rateCell(o.corr,"corr")); tr.appendChild(tdC);
    var tdS=el("td");
    if(o.dq){var b=el("span","badge dq"); b.innerHTML='<span class="dot"></span>DQ'; tdS.appendChild(b)}
    else {var b=el("span","badge ok"); b.innerHTML='<span class="dot"></span>ok'; tdS.appendChild(b)}
    tr.appendChild(tdS);
    tbody.appendChild(tr);
  });
  // legend
  var lg=$("lbLegend"); clear(lg);
  var html="";
  ["T1","T2","T3","T4","T5","T6","RVC"].forEach(function(t,i){
    html+='<span class="sw bg-'+t+'"></span>'+t+(t==="RVC"?" (engine fidelity, optional)":"");
  });
  html+='<span style="margin-left:14px">DQ = disqualified (executed a hostile pickle)</span>';
  lg.innerHTML=html;
}
function cmpRows(a,b,k,dir){
  var av=a[k],bv=b[k];
  if(av==null&&bv==null)return 0;
  if(av==null)return 1;
  if(bv==null)return -1;
  if(k==="model")return av<bv?-dir:av>bv?dir:0;
  return (av-bv)*dir;
}
function sortLb(k){
  if(S.lb.sort===k){S.lb.dir=-S.lb.dir}else{S.lb.sort=k; S.lb.dir=(k==="model"?1:-1)}
  renderLeaderboard();
}
function rateCell(r,tier){
  var c=el("div","cell-rate");
  if(r==null){
    var n=el("span","na"); n.textContent="n/a"; c.appendChild(n); return c;
  }
  var p=el("span","pct"); p.textContent=fmtPct1(r); p.style.color = tier==="corr"?"var(--amber)":(TIER_COLOR[tier]||"var(--text)"); c.appendChild(p);
  var f=el("span","frac"); var run=null;
  c.appendChild(f);
  var mb=el("div","mini-bar"); var bi=el("i"); bi.style.width=(r*100)+"%"; bi.style.background=tier==="corr"?"var(--amber)":(TIER_COLOR[tier]||"var(--text)"); mb.appendChild(bi); c.appendChild(mb);
  return c;
}
function seg(opts,cur,onChange){
  var g=el("div","seg"); g.setAttribute("role","group");
  opts.forEach(function(o){
    var b=el("button",null,o.l); b.type="button";
    b.setAttribute("aria-pressed",o.v===cur?"true":"false");
    b.addEventListener("click",function(){onChange(o.v)});
    g.appendChild(b);
  });
  return g;
}
function filtLabel(t){var s=el("span","filt-label"); s.textContent=t; return s}

/* ---------- model detail ---------- */
function openModel(m){
  S.md.model=m;
  var idx=IDX();
  if(!S.md.cond||idx.conds.indexOf(S.md.cond)<0)S.md.cond=idx.conds.indexOf("open_book")>=0?"open_book":idx.conds[0];
  if(!S.md.lang||idx.langs.indexOf(S.md.lang)<0)S.md.lang=idx.langs[0];
  if(!S.md.itLang||idx.langs.indexOf(S.md.itLang)<0)S.md.itLang=idx.langs[0];
  if(!S.md.itCond||idx.conds.indexOf(S.md.itCond)<0)S.md.itCond=idx.conds.indexOf("open_book")>=0?"open_book":idx.conds[0];
  renderModelDetail();
  switchTab("model");
}
function renderModelDetail(){
  var idx=IDX();
  var fbox=$("mdFilters"); clear(fbox);
  var body=$("mdBody"); clear(body);
  if(!idx.models.length){fbox.parentNode.style.display="none"; body.appendChild(emptyView("No runs.","Load a dataset with runs to inspect a model.")); return}
  fbox.parentNode.style.display="";
  // model selector
  if(!S.md.model||idx.models.indexOf(S.md.model)<0)S.md.model=idx.models[0];
  if(!S.md.cond||idx.conds.indexOf(S.md.cond)<0)S.md.cond=idx.conds.indexOf("open_book")>=0?"open_book":idx.conds[0];
  if(!S.md.lang||idx.langs.indexOf(S.md.lang)<0)S.md.lang=idx.langs[0];
  if(!S.md.itLang||idx.langs.indexOf(S.md.itLang)<0)S.md.itLang=idx.langs[0];
  if(!S.md.itCond||idx.conds.indexOf(S.md.itCond)<0)S.md.itCond=idx.conds.indexOf("open_book")>=0?"open_book":idx.conds[0];
  fbox.appendChild(filtLabel("model"));
  fbox.appendChild(select(idx.models.map(function(m){return {v:m,l:m}}), S.md.model, function(v){S.md.model=v; renderModelDetail()}));
  fbox.appendChild(filtLabel("heatmap condition"));
  fbox.appendChild(seg(idx.conds.map(function(c){return {v:c,l:COND_LABEL[c]||c}}), S.md.cond, function(v){S.md.cond=v; renderModelDetail()}));

  var m=S.md.model;
  // build layout
  var grid=el("div","md-grid");
  // heatmap card
  var hmCard=el("div","card");
  hmCard.appendChild(el("h2","card-title","Tier × language heatmap <span class='muted'>"+esc(COND_LABEL[S.md.cond]||S.md.cond)+"</span>"));
  hmCard.appendChild(heatmap(m,S.md.cond));
  // click-to-fixtures hint
  var hint=el("p","muted"); hint.style.cssText="font-size:11.5px;margin:10px 0 0"; hint.textContent="Click a cell to open its fixture breakdown.";
  hmCard.appendChild(hint);
  grid.appendChild(hmCard);
  // side card: meta
  var side=el("div","card");
  side.appendChild(el("h2","card-title","Run envelope"));
  side.appendChild(metaSection(m,S.md.lang,S.md.cond));
  // language selector for envelope
  var envSel=el("div","filters"); envSel.style.cssText="margin-bottom:12px";
  envSel.appendChild(filtLabel("envelope language"));
  envSel.appendChild(seg(idx.langs.map(function(l){return {v:l,l:LANG_LABEL[l]||l}}), S.md.lang, function(v){S.md.lang=v; renderModelDetail()}));
  side.insertBefore(envSel, side.querySelector(".md-meta"));
  grid.appendChild(side);
  body.appendChild(grid);
  // iterations chart
  var itCard=el("div","card chart-card");
  var itHead=el("div","view-head"); itHead.style.cssText="margin-bottom:8px";
  itHead.appendChild(el("h2","card-title","Iterations to green"));
  var itF=el("div","filters");
  itF.appendChild(filtLabel("language")); itF.appendChild(seg(idx.langs.map(function(l){return {v:l,l:LANG_LABEL[l]||l}}), S.md.itLang, function(v){S.md.itLang=v; renderModelDetail()}));
  itF.appendChild(filtLabel("condition")); itF.appendChild(seg(idx.conds.map(function(c){return {v:c,l:COND_LABEL[c]||c}}), S.md.itCond, function(v){S.md.itCond=v; renderModelDetail()}));
  itHead.appendChild(itF);
  itCard.appendChild(itHead);
  itCard.appendChild(iterationsChart(m,S.md.itLang,S.md.itCond));
  body.appendChild(itCard);
  // gap per language
  var gapCard=el("div","card chart-card");
  gapCard.appendChild(el("h2","card-title","Memorization gap <span class='muted'>per language</span>"));
  gapCard.appendChild(modelGapTable(m));
  body.appendChild(gapCard);
}
function heatmap(model,cond){
  var idx=IDX();
  var langs=idx.langs;
  var tiers=TIERS.concat(["RVC"]);
  var wrap=el("div","heatmap-wrap");
  var grid=el("div","heatmap");
  // columns: 1 label + langs
  var ncol=1+langs.length;
  grid.style.gridTemplateColumns="90px repeat("+langs.length+",1fr)";
  // header row
  var corner=el("div","hm-head"); corner.textContent="tier \\ lang"; grid.appendChild(corner);
  langs.forEach(function(l){var h=el("div","hm-head"); h.textContent=LANG_LABEL[l]||l; grid.appendChild(h)});
  tiers.forEach(function(t){
    var lab=el("div","hm-head"); lab.style.cssText="justify-content:flex-start;color:"+(TIER_COLOR[t]); lab.textContent=t; grid.appendChild(lab);
    langs.forEach(function(l){
      var r=runFor(model,l,cond);
      var tr=r&&r.tiers[t];
      var rr=tr?rate(tr):null;
      var cell=el("div","hm-cell"+(t==="RVC"?" rvc":""));
      if(rr==null){
        cell.classList.add("empty"); cell.textContent = (t==="RVC"?"n/a":"—");
        if(t==="RVC")cell.title="No RVC track for this cell (RVC is optional and absent on delta runs).";
        else cell.title="No run for "+model+" · "+l+" · "+cond;
      }else{
        cell.style.background=heatColor(rr,t);
        cell.style.color=textColorFor(rr);
        cell.tabIndex=0; cell.setAttribute("role","button");
        cell.title=TIER_LABEL[t]+" · "+(LANG_LABEL[l]||l)+" · "+fmtPct1(rr)+" ("+tr.pass+"/"+tr.total+")";
        cell.innerHTML='<span class="hm-pct">'+fmtPct1(rr)+'</span><span class="hm-frac">'+tr.pass+'/'+tr.total+'</span>';
        cell.addEventListener("click",function(){openFixtures(model,l,cond)});
        cell.addEventListener("keydown",function(e){if(e.key==="Enter"||e.key===" "){e.preventDefault();openFixtures(model,l,cond)}});
      }
      grid.appendChild(cell);
    });
  });
  wrap.appendChild(grid);
  return wrap;
}
function metaSection(model,lang,cond){
  var r=runFor(model,lang,cond);
  var wrap=el("div","md-meta");
  if(!r){
    wrap.appendChild(el("div","md-empty")); wrap.firstChild.innerHTML="<p class='muted'>No run for "+esc(model)+" · "+esc(LANG_LABEL[lang]||lang)+" · "+esc(COND_LABEL[cond]||cond)+".</p>";
    return wrap;
  }
  // budget vs spent
  wrap.appendChild(statCard(fmtBudget(r.spent.tokens)+"/"+fmtBudget(r.budget.tokens),"tokens spent / budget"));
  wrap.appendChild(statCard(fmtDur(r.spent.wall_seconds)+"/"+fmtDur(r.budget.wall_seconds),"wall time spent / budget"));
  wrap.appendChild(statCard(r.determinism_violations,"determinism violations", r.determinism_violations?"non-byte-identical passes":"byte-identical"));
  wrap.appendChild(statCard(fmtBytes(r.t5_peak_rss_bytes),"T5 peak RSS", r.t5_peak_rss_bytes?"during T5":"T5 not attempted"));
  // incidents
  var inc=el("div","incident-grid"); inc.style.cssText="grid-column:1/-1";
  inc.appendChild(incidentBox(r.t6_incidents.exec_attempts,"exec attempts",r.t6_incidents.exec_attempts>0?"bad":"ok"));
  inc.appendChild(incidentBox(r.t6_incidents.crashes,"crashes",r.t6_incidents.crashes>0?"bad":"ok"));
  inc.appendChild(incidentBox(r.t6_incidents.hangs,"hangs",r.t6_incidents.hangs>0?"bad":"ok"));
  wrap.appendChild(inc);
  var dq=disqualified(r);
  var note=el("p"); note.style.cssText="grid-column:1/-1;margin:6px 0 0;font-size:12.5px";
  if(dq){note.className="badge dq"; note.innerHTML='<span class="dot"></span>Disqualified — executed a hostile pickle'}
  else{note.className="muted"; note.textContent="Qualified — no hostile-pickle execution."}
  wrap.appendChild(note);
  return wrap;
}
function fmtBudget(n){if(n>=1e6)return (n/1e6).toFixed(2)+"M"; if(n>=1e3)return (n/1e3).toFixed(1)+"k"; return String(n)}
function fmtDur(s){if(s>=3600)return (s/3600).toFixed(1)+"h"; if(s>=60)return (s/60).toFixed(1)+"m"; return s+"s"}
function fmtBytes(b){if(!b)return "0"; var u=["B","KB","MB","GB","TB"]; var i=0; while(b>=1024&&i<u.length-1){b/=1024;i++} return b.toFixed(b>=100?0:1)+u[i]}

function iterationsChart(model,lang,cond){
  var r=runFor(model,lang,cond);
  var wrap=el("div");
  if(!r||!r.iterations||!r.iterations.length){
    wrap.appendChild(emptyView("No iteration data.","This cell has no public-fixture time series."));
    return wrap;
  }
  var its=r.iterations;
  var tiers=TIERS.concat(r.iterations[0].tier_passes.RVC!=null?["RVC"]:[]);
  // figure which tiers have any movement (max>0)
  var present=tiers.filter(function(t){return its.some(function(it){return (it.tier_passes[t]||0)>0})});
  if(!present.length){wrap.appendChild(emptyView("No passes recorded.","All iterations show zero passes.")); return wrap}
  var tMax=its[its.length-1].t_seconds||1;
  var yMax=0; present.forEach(function(t){its.forEach(function(it){var v=it.tier_passes[t]||0; if(v>yMax)yMax=v})});
  if(yMax<=0)yMax=1;
  var W=720,H=240,padL=38,padR=16,padT=16,padB=28;
  var iw=W-padL-padR, ih=H-padT-padB;
  var sx=function(t){return padL+(t/tMax)*iw};
  var sy=function(v){return padT+ih-(v/yMax)*ih};
  var svg=ns("svg"); svg.setAttribute("class","svg-chart"); svg.setAttribute("viewBox","0 0 "+W+" "+H); svg.setAttribute("preserveAspectRatio","xMidYMid meet");
  // grid + y labels
  var ticks=4;
  for(var i=0;i<=ticks;i++){
    var v=(yMax/ticks)*i; var y=sy(v);
    var g=ns("line"); g.setAttribute("class","grid"); g.setAttribute("x1",padL); g.setAttribute("x2",W-padR); g.setAttribute("y1",y); g.setAttribute("y2",y); svg.appendChild(g);
    var t=ns("text"); t.setAttribute("class","lbl"); t.setAttribute("x",padL-6); t.setAttribute("y",y+3); t.setAttribute("text-anchor","end"); t.textContent=Math.round(v); svg.appendChild(t);
  }
  // x labels
  var xticks=Math.min(4, its.length);
  for(var j=0;j<=xticks;j++){
    var tt=(tMax/xticks)*j; var x=sx(tt);
    var xt=ns("text"); xt.setAttribute("class","lbl"); xt.setAttribute("x",x); xt.setAttribute("y",H-padB+16); xt.setAttribute("text-anchor","middle"); xt.textContent=fmtDur(tt); svg.appendChild(xt);
  }
  // axis
  var ax=ns("line"); ax.setAttribute("class","axis"); ax.setAttribute("x1",padL); ax.setAttribute("x2",padL); ax.setAttribute("y1",padT); ax.setAttribute("y2",H-padB); svg.appendChild(ax);
  var ax2=ns("line"); ax2.setAttribute("class","axis"); ax2.setAttribute("x1",padL); ax2.setAttribute("x2",W-padR); ax2.setAttribute("y1",H-padB); ax2.setAttribute("y2",H-padB); svg.appendChild(ax2);
  // lines
  present.forEach(function(t){
    var d=""; its.forEach(function(it,i){var v=it.tier_passes[t]||0; d+=(i?" L":"M")+sx(it.t_seconds).toFixed(1)+" "+sy(v).toFixed(1)});
    var path=ns("path"); path.setAttribute("class","line"); path.setAttribute("d",d); path.setAttribute("stroke",TIER_COLOR[t]); svg.appendChild(path);
    // end dot
    var last=its[its.length-1]; var v=last.tier_passes[t]||0;
    var dot=ns("circle"); dot.setAttribute("class","dot"); dot.setAttribute("cx",sx(last.t_seconds)); dot.setAttribute("cy",sy(v)); dot.setAttribute("r",3.2); dot.setAttribute("fill",TIER_COLOR[t]); svg.appendChild(dot);
  });
  // legend
  var leg=el("div","chart-legend");
  present.forEach(function(t){
    var li=el("span","li"); var sw=el("span","sw"); sw.style.background=TIER_COLOR[t]; li.appendChild(sw); li.appendChild(document.createTextNode(TIER_LABEL[t])); leg.appendChild(li);
  });
  wrap.appendChild(leg);
  wrap.appendChild(svg);
  return wrap;
}
function ns(tag){return document.createElementNS("http://www.w3.org/2000/svg",tag)}

function modelGapTable(model){
  var idx=IDX();
  var wrap=el("div");
  var rows=idx.langs.map(function(l){return {lang:l, gap:gapFor(model,l)}}).filter(function(o){return o.gap!=null});
  if(!rows.length){wrap.appendChild(emptyView("No gap data.","Memorization gap needs both a stock and a delta run for this model in a language.")); return wrap}
  rows.forEach(function(o){
    var row=el("div","cmp-row"); row.style.cssText="grid-template-columns:60px 1fr 60px";
    var lab=el("span"); lab.style.cssText="font-weight:700"; lab.textContent=LANG_LABEL[o.lang]||o.lang; row.appendChild(lab);
    var track=el("div","gap-track");
    var i=el("i"); var g=o.gap; // -1..1, 0 center
    var half=50; var w=Math.abs(g)*half; var left = g>=0? half : half-w;
    i.style.cssText="position:absolute;top:0;bottom:0;left:"+left+"%;width:"+w+"%;background:"+(g>0.05?"var(--warn)":g<-0.05?"var(--good)":"var(--muted)");
    track.appendChild(i);
    // center marker
    var mk=el("i"); mk.style.cssText="position:absolute;top:-2px;bottom:-2px;left:50%;width:1px;background:var(--line2)"; track.appendChild(mk);
    row.appendChild(track);
    var v=el("span","gap-val "+(g>0.05?"gap-pos":g<-0.05?"gap-neg":"")); v.textContent=(g>=0?"+":"")+(Math.round(g*1000)/10).toFixed(1)+"%";
    row.appendChild(v);
    wrap.appendChild(row);
  });
  var note=el("p","muted"); note.style.cssText="font-size:11.5px;margin-top:10px"; note.textContent="Positive = model scored better on stock than delta (possible memorization). RVC excluded.";
  wrap.appendChild(note);
  return wrap;
}

/* ---------- memorization gap view ---------- */
function renderGap(){
  var idx=IDX();
  var body=$("gapBody"); clear(body);
  if(!idx.models.length){body.appendChild(emptyView("No runs.","Load a dataset with runs to compute memorization gaps.")); return}
  var rows=[];
  idx.models.forEach(function(m){idx.langs.forEach(function(l){var g=gapFor(m,l); if(g!=null){
    var s=stockRun(m,l),d=deltaRun(m,l);
    rows.push({m:m,lang:l,gap:g,stock:readingTotals(s).rate,delta:readingTotals(d).rate})
  }})});
  if(!rows.length){body.appendChild(emptyView("No gap data.","No (model, language) pair has both a stock and a delta run. The gap is derived from stock − delta.")); return}
  rows.sort(function(a,b){return cmpGap(a,b,S.gap.sort,S.gap.dir)});
  // controls
  var head=el("div","view-head"); head.style.cssText="margin-bottom:12px";
  var cols=[{k:"m",l:"Model"},{k:"lang",l:"Language"},{k:"stock",l:"Stock"},{k:"delta",l:"Delta"},{k:"gap",l:"Gap"}];
  var segs=el("div","filters"); segs.appendChild(filtLabel("sort by"));
  cols.forEach(function(c){
    var b=el("button","chip"); b.textContent=c.l; b.setAttribute("aria-pressed",S.gap.sort===c.k?"true":"false");
    b.addEventListener("click",function(){if(S.gap.sort===c.k)S.gap.dir=-S.gap.dir; else{S.gap.sort=c.k; S.gap.dir=c.k==="m"||c.k==="lang"?1:-1} renderGap()});
    segs.appendChild(b);
  });
  head.appendChild(segs);
  body.appendChild(head);
  var wrap=el("div","gap-table-wrap");
  var t=el("table","gap-table");
  var thead=el("thead"); var tr=el("tr");
  cols.forEach(function(c){var th=el("th",c.k===S.gap.sort?"sort-asc":""); th.textContent=c.l+(S.gap.sort===c.k?(S.gap.dir<0?" ▼":" ▲"):""); tr.appendChild(th)});
  thead.appendChild(tr); t.appendChild(thead);
  var tbody=el("tbody");
  rows.forEach(function(o){
    var r=el("tr");
    var tm=el("td","model",o.m); tm.style.cursor="pointer"; tm.addEventListener("click",function(){openModel(o.m)}); r.appendChild(tm);
    r.appendChild(el("td",null,LANG_LABEL[o.lang]||o.lang));
    r.appendChild(tdPct(o.stock));
    r.appendChild(tdPct(o.delta));
    var td=el("td");
    var gb=el("div","gap-bar");
    var track=el("div","gap-track"); var g=o.gap; var half=50,w=Math.abs(g)*half,left=g>=0?half:half-w;
    var bi=el("i"); bi.style.cssText="position:absolute;top:0;bottom:0;left:"+left+"%;width:"+w+"%;background:"+(g>0.05?"var(--warn)":g<-0.05?"var(--good)":"var(--muted)"); track.appendChild(bi);
    var mk=el("i"); mk.style.cssText="position:absolute;top:-2px;bottom:-2px;left:50%;width:1px;background:var(--line2)"; track.appendChild(mk);
    gb.appendChild(track);
    var v=el("span","gap-val "+(g>0.05?"gap-pos":g<-0.05?"gap-neg":"")); v.textContent=(g>=0?"+":"")+(Math.round(g*1000)/10).toFixed(1)+"%";
    gb.appendChild(v); td.appendChild(gb); r.appendChild(td);
    tbody.appendChild(r);
  });
  t.appendChild(tbody); wrap.appendChild(t); body.appendChild(wrap);
  var note=el("p","muted"); note.style.cssText="font-size:12px;margin:12px 2px 0"; note.textContent="Stock = open_book (or closed_book). Delta = delta condition. Rates are over T1–T6 only; RVC is excluded. "+rows.length+" pair"+(rows.length===1?"":"s")+".";
  body.appendChild(note);
}
function cmpGap(a,b,k,dir){
  var av=a[k],bv=b[k];
  if(k==="m"||k==="lang")return av<bv?-dir:av>bv?dir:0;
  return (av-bv)*dir;
}
function tdPct(r){var td=el("td","num"); td.textContent=fmtPct1(r); td.style.fontVariantNumeric="tabular-nums"; return td}

/* ---------- compare ---------- */
function renderCompare(){
  var idx=IDX();
  var fbox=$("cmpFilters"); clear(fbox);
  var body=$("cmpBody"); clear(body);
  if(!idx.models.length){fbox.parentNode.style.display="none"; body.appendChild(emptyView("No runs.","Load a dataset with runs to compare models.")); return}
  fbox.parentNode.style.display="";
  if(!S.cmp.lang||idx.langs.indexOf(S.cmp.lang)<0)S.cmp.lang=idx.langs[0];
  if(!S.cmp.cond||idx.conds.indexOf(S.cmp.cond)<0)S.cmp.cond=idx.conds.indexOf("open_book")>=0?"open_book":idx.conds[0];
  // pick chips
  var pick=el("div","cmp-pick");
  idx.models.forEach(function(m){
    var on=!!S.cmp.set[m];
    var c=el("button","chip"); c.textContent=m; c.setAttribute("aria-pressed",on?"true":"false");
    c.addEventListener("click",function(){
      if(S.cmp.set[m]){delete S.cmp.set[m];}
      else{
        S.cmp.set[m]=true;
        var keys=Object.keys(S.cmp.set);
        if(keys.length>4){var first=keys[0]; delete S.cmp.set[first]; if(first===m){S.cmp.set[m]=true;}}
      }
      renderCompare();
    });
    pick.appendChild(c);
  });
  body.appendChild(pick);
  // cond/lang selectors
  var sel=el("div","filters"); sel.style.cssText="margin-bottom:16px";
  sel.appendChild(filtLabel("language")); sel.appendChild(seg(idx.langs.map(function(l){return {v:l,l:LANG_LABEL[l]||l}}),S.cmp.lang,function(v){S.cmp.cmp=v;S.cmp.lang=v; renderCompare()}));
  sel.appendChild(filtLabel("condition")); sel.appendChild(seg(idx.conds.map(function(c){return {v:c,l:COND_LABEL[c]||c}}),S.cmp.cond,function(v){S.cmp.cond=v; renderCompare()}));
  body.appendChild(sel);
  var chosen=idx.models.filter(function(m){return S.cmp.set[m]});
  if(!chosen.length){
    var e=el("div","cmp-empty");
    e.innerHTML="<p style='margin:0 0 6px;font-weight:700'>Pick one or more models above</p><p style='margin:0'>Compare up to four side by side. "+(idx.models.length===1?"This dataset has only one model — pick it to see its tier profile.":"")+"</p>";
    body.appendChild(e); return;
  }
  var grid=el("div","cmp-grid");
  grid.style.gridTemplateColumns="repeat(auto-fit,minmax(240px,1fr))";
  chosen.forEach(function(m){
    grid.appendChild(compareCol(m,S.cmp.lang,S.cmp.cond));
  });
  body.appendChild(grid);
  if(chosen.length===1){
    var note=el("p","muted"); note.style.cssText="margin-top:14px;font-size:12.5px"; note.textContent="Only one model selected — pick another to compare side by side.";
    body.appendChild(note);
  }
}
function compareCol(m,lang,cond){
  var r=runFor(m,lang,cond);
  var col=el("div","cmp-col");
  var head=el("div","cmp-head");
  var nm=el("div","cmp-model",m); head.appendChild(nm);
  var rm=el("button","cmp-remove","×"); rm.title="Remove"; rm.setAttribute("aria-label","Remove "+m); rm.addEventListener("click",function(){delete S.cmp.set[m]; renderCompare()}); head.appendChild(rm);
  col.appendChild(head);
  if(disqualified(r)){var b=el("span","badge dq"); b.innerHTML='<span class="dot"></span>DQ'; col.appendChild(b)}
  if(!r){col.appendChild(el("p","muted","No run for "+(LANG_LABEL[lang]||lang)+" · "+(COND_LABEL[cond]||cond)+".")); return col}
  var tiers=TIERS.concat(r.tiers.RVC?["RVC"]:[]);
  tiers.forEach(function(t){
    var tr=r.tiers[t]; var rr=tr?rate(tr):null;
    var row=el("div","cmp-row");
    var tag=el("span","tier-tag"); tag.style.background=TIER_COLOR[t]; tag.style.color="#0a0a0f"; tag.textContent=t; row.appendChild(tag);
    var track=el("div","cmp-track"); var bi=el("i"); bi.style.width=(rr==null?0:rr*100)+"%"; bi.style.background=TIER_COLOR[t]; track.appendChild(bi); row.appendChild(track);
    var pct=el("span","cmp-pct"); pct.textContent=rr==null?"n/a":fmtPct1(rr); row.appendChild(pct);
    col.appendChild(row);
  });
  // correctness summary
  var c=correctness(r);
  var sum=el("div"); sum.style.cssText="margin-top:10px;padding-top:10px;border-top:1px solid var(--line);display:flex;justify-content:space-between;font-size:12px";
  sum.innerHTML='<span class="muted">correctness T1–T5</span><strong style="color:var(--amber)">'+fmtPct1(c)+'</strong>';
  col.appendChild(sum);
  return col;
}

/* ---------- fixtures ---------- */
function openFixtures(model,lang,cond){
  S.fx.model=model; S.fx.lang=lang; S.fx.cond=cond;
  renderFixtures();
  switchTab("fixtures");
}
function renderFixtures(){
  var idx=IDX();
  var fbox=$("fxFilters"); clear(fbox);
  var body=$("fxBody"); clear(body);
  if(!idx.models.length){fbox.parentNode.style.display="none"; body.appendChild(emptyView("No runs.","Load a dataset with runs to drill into fixtures.")); return}
  fbox.parentNode.style.display="";
  if(!S.fx.model||idx.models.indexOf(S.fx.model)<0)S.fx.model=idx.models[0];
  if(!S.fx.lang||idx.langs.indexOf(S.fx.lang)<0)S.fx.lang=idx.langs[0];
  if(!S.fx.cond||idx.conds.indexOf(S.fx.cond)<0)S.fx.cond=idx.conds.indexOf("open_book")>=0?"open_book":idx.conds[0];
  fbox.appendChild(filtLabel("model")); fbox.appendChild(select(idx.models.map(function(m){return {v:m,l:m}}),S.fx.model,function(v){S.fx.model=v; renderFixtures()}));
  fbox.appendChild(filtLabel("language")); fbox.appendChild(seg(idx.langs.map(function(l){return {v:l,l:LANG_LABEL[l]||l}}),S.fx.lang,function(v){S.fx.lang=v; renderFixtures()}));
  fbox.appendChild(filtLabel("condition")); fbox.appendChild(seg(idx.conds.map(function(c){return {v:c,l:COND_LABEL[c]||c}}),S.fx.cond,function(v){S.fx.cond=v; renderFixtures()}));
  var r=runFor(S.fx.model,S.fx.lang,S.fx.cond);
  if(!r){body.appendChild(emptyView("No run.","No run for "+S.fx.model+" · "+(LANG_LABEL[S.fx.lang]||S.fx.lang)+" · "+(COND_LABEL[S.fx.cond]||S.fx.cond)+".")); return}
  var head=el("div","card"); head.style.cssText="margin-bottom:16px";
  head.innerHTML='<div style="display:flex;flex-wrap:wrap;gap:14px;align-items:center"><div><div class="muted" style="font-size:11px;text-transform:uppercase;letter-spacing:.08em">run</div><div style="font-weight:800;font-size:18px">'+esc(S.fx.model)+'</div></div><div><div class="muted" style="font-size:11px;text-transform:uppercase;letter-spacing:.08em">language</div><div style="font-weight:700">'+esc(LANG_LABEL[S.fx.lang]||S.fx.lang)+'</div></div><div><div class="muted" style="font-size:11px;text-transform:uppercase;letter-spacing:.08em">condition</div><div style="font-weight:700">'+esc(COND_LABEL[S.fx.cond]||S.fx.cond)+'</div></div>'+(disqualified(r)?'<span class="badge dq"><span class="dot"></span>DQ</span>':'')+'</div>';
  body.appendChild(head);
  var tiers=TIERS.concat(r.tiers.RVC?["RVC"]:[]);
  tiers.forEach(function(t){
    var tr=r.tiers[t]; if(!tr)return;
    body.appendChild(fixtureTier(t,tr));
  });
  // explicit RVC n/a note for delta
  if(!r.tiers.RVC){
    var card=el("div","card"); card.style.cssText="margin-top:16px;border-style:dashed";
    card.innerHTML='<div class="card-title">RVC <span class="muted">engine fidelity</span></div><p class="muted" style="margin:0">n/a — this run has no <code>tiers.RVC</code> track. RVC is optional and absent on delta runs.</p>';
    body.appendChild(card);
  }
}
function fixtureTier(t,tr){
  var card=el("div","card fx-tier");
  var head=el("div","fx-tier-head");
  var tag=el("span","fx-tier-tag"); tag.style.background=TIER_COLOR[t]; tag.textContent=t; head.appendChild(tag);
  var meta=el("span","fx-tier-meta"); meta.innerHTML='<strong style="color:'+(TIER_COLOR[t])+'">'+tr.pass+'/'+tr.total+'</strong> passed · '+fmtPct1(rate(tr))+' <span class="muted">· '+esc(TIER_LABEL[t]||"")+'</span>'; head.appendChild(meta);
  card.appendChild(head);
  var cats=el("div","fx-cats");
  Object.keys(tr.categories).sort().forEach(function(cn){
    var c=tr.categories[cn]; var rr=c.total? c.pass/c.total: null;
    var box=el("div","fx-cat");
    var nm=el("div","fx-cat-name",cn); box.appendChild(nm);
    var bar=el("div","fx-cat-bar"); var bi=el("i"); bi.style.width=(rr==null?0:rr*100)+"%"; bi.style.background=TIER_COLOR[t]; bar.appendChild(bi); box.appendChild(bar);
    var frac=el("div","fx-cat-frac"); frac.innerHTML='<strong>'+c.pass+'/'+c.total+'</strong> · '+fmtPct1(rr)+(rr!=null&&rr<1?' <span style="color:var(--bad)">✕</span>':''); box.appendChild(frac);
    cats.appendChild(box);
  });
  card.appendChild(cats);
  return card;
}
function select(opts,cur,onChange){
  var s=el("select","filt");
  opts.forEach(function(o){var op=el("option"); op.value=o.v; op.textContent=o.l; if(o.v===cur)op.selected=true; s.appendChild(op)});
  s.addEventListener("change",function(){onChange(s.value)});
  return s;
}

/* ---------- empty view helper ---------- */
function emptyView(title,sub){var e=el("div","md-empty"); e.innerHTML="<h3 style='margin:0 0 6px'>"+esc(title)+"</h3><p style='margin:0;color:var(--muted)'>"+esc(sub)+"</p>"; return e}

/* ---------- theme ---------- */
function setTheme(t){
  S.theme=t; document.documentElement.setAttribute("data-theme",t);
  try{localStorage.setItem("nt_theme",t)}catch(e){}
}
function initTheme(){
  var saved=null; try{saved=localStorage.getItem("nt_theme")}catch(e){}
  setTheme(saved==="light"?"light":"dark");
}

/* ---------- wire up ---------- */
function init(){
  initTheme();
  document.querySelectorAll(".tab").forEach(function(t){t.addEventListener("click",function(){switchTab(t.dataset.tab)})});
  $("themeToggle").addEventListener("click",function(){setTheme(S.theme==="dark"?"light":"dark")});
  document.addEventListener("keydown",function(e){if(e.key==="Escape"){var d=$("hashDialog"); if(d.open)d.close()}});
  fetchData();
}
init();
})();
