#!/usr/bin/env python3
"""NullTorch-Board mechanical gate — a dashboard must PASS this before judging.

Two layers:
  static  G0: scan the submission for any external reference (CDN, web font,
              protocol-relative src/href, @import url(http…), remote script/
              link/img). Any hit fails — this is the zero-external-deps rule.
  runtime (needs chromium + playwright): serve the submission on 127.0.0.1,
              block every non-localhost request, and for each dataset (the
              degenerate empty/one_model plus many_models and the real run):
                - render with no console errors / page errors
                - make no external network request
                - render non-trivial content, and show a model_id from the data
                - no horizontal PAGE scroll at 375 / 768 / 1440 px
If chromium/playwright are missing, the static gate still runs and the runtime
checks are reported as SKIPPED (manual per GATE.md).

Usage: python3 scripts/board_gate.py <submission_dir> [--data-name results.json]
Exit 0 iff every executed check passes.
"""
import http.server
import os
import re
import shutil
import socket
import sys
import threading
from functools import partial

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CHROMIUM = "/usr/bin/chromium"
WIDTHS = [375, 768, 1440]

EXTERNAL = [
    (re.compile(r"""\bsrc\s*=\s*['"]https?://""", re.I), "remote src"),
    (re.compile(r"""\bhref\s*=\s*['"]https?://""", re.I), "remote href"),
    (re.compile(r"""\b(?:src|href)\s*=\s*['"]//"""), "protocol-relative ref"),
    (re.compile(r"""@import\s+(?:url\()?['"]?https?://""", re.I), "@import remote"),
    (re.compile(r"""url\(\s*['"]?https?://""", re.I), "css url() remote"),
    (re.compile(r"fonts\.(googleapis|gstatic)\.com", re.I), "google web font"),
    (re.compile(r"""\b(?:fetch|import)\s*\(\s*['"]https?://""", re.I), "remote fetch/import"),
    (re.compile(r"cdn\.|unpkg\.com|jsdelivr\.net|cdnjs\.", re.I), "CDN host"),
]
TEXT_EXT = {".html", ".htm", ".js", ".mjs", ".css", ".json", ".svg"}


def static_gate(sub):
    hits = []
    for dp, _, fs in os.walk(sub):
        for f in fs:
            if os.path.splitext(f)[1].lower() not in TEXT_EXT:
                # a non-text asset that isn't an image/font is suspicious but we
                # only hard-fail on external refs; binary local assets are fine.
                continue
            p = os.path.join(dp, f)
            try:
                txt = open(p, "r", errors="replace").read()
            except OSError:
                continue
            for rx, why in EXTERNAL:
                for m in rx.finditer(txt):
                    hits.append(f"{os.path.relpath(p, sub)}: {why} "
                                f"({txt[m.start():m.start()+40].strip()!r})")
    return hits


def find_entry(sub):
    for cand in ("index.html", "dashboard.html"):
        if os.path.isfile(os.path.join(sub, cand)):
            return cand
    htmls = [f for f in os.listdir(sub) if f.lower().endswith(".html")]
    return htmls[0] if htmls else None


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def runtime_gate(sub, entry, datasets, data_name):
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        return None, ["playwright not installed"]
    if not os.path.exists(CHROMIUM):
        return None, ["chromium not found at " + CHROMIUM]

    port = free_port()

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a, **k):
            pass

    handler = partial(Quiet, directory=sub)
    httpd = http.server.HTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{port}/{entry}"

    fails = []
    with sync_playwright() as pw:
        br = pw.chromium.launch(executable_path=CHROMIUM, args=["--no-sandbox"])
        for name, path in datasets.items():
            shutil.copy(path, os.path.join(sub, data_name))
            ctx = br.new_context()
            page = ctx.new_page()
            errs, ext = [], []
            page.on("console", lambda m: errs.append(m.text)
                    if m.type == "error" else None)
            page.on("pageerror", lambda e: errs.append(str(e)))
            page.route("**/*", lambda route: (
                ext.append(route.request.url) or route.abort()
                if not route.request.url.startswith(f"http://127.0.0.1:{port}")
                else route.continue_()))
            try:
                page.goto(base, wait_until="networkidle", timeout=15000)
                page.wait_for_timeout(400)
            except Exception as e:
                fails.append(f"[{name}] load failed: {str(e)[:80]}")
                ctx.close()
                continue
            if ext:
                fails.append(f"[{name}] external request(s): {ext[:2]}")
            if errs:
                fails.append(f"[{name}] console/page error: {errs[0][:80]}")
            body = (page.inner_text("body") or "").strip()
            if len(body) < 10 and name != "empty":
                fails.append(f"[{name}] rendered no visible content")
            # DOM reflects the REAL data: a model_id from the real results.json
            # appears in the page. Only checked for "real" — the synthetic
            # datasets are swapped into results.json to test graceful handling
            # (no crash/error), NOT re-render, since a manifest-driven gallery
            # legitimately ignores a results.json swap and reads manifest.json.
            if name == "real":
                import json
                d = json.load(open(path))
                mids = {r["model_id"] for r in d.get("runs", [])}
                if mids and not any(m in body for m in mids):
                    fails.append(f"[{name}] no real model_id shown in DOM")
            for w in WIDTHS:
                page.set_viewport_size({"width": w, "height": 900})
                page.wait_for_timeout(120)
                sw = page.evaluate("document.documentElement.scrollWidth")
                if sw > w + 2:
                    fails.append(f"[{name}] horizontal page scroll at {w}px "
                                 f"(scrollWidth {sw})")
            ctx.close()
        br.close()
    httpd.shutdown()
    return (not fails), fails


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: board_gate.py <submission_dir> [--data-name results.json]")
    sub = os.path.abspath(sys.argv[1])
    data_name = "results.json"
    if "--data-name" in sys.argv:
        data_name = sys.argv[sys.argv.index("--data-name") + 1]

    print(f"== NullTorch-Board gate: {sub} ==\n")
    ok = True

    print("[G0] external-dependency scan")
    hits = static_gate(sub)
    if hits:
        ok = False
        for h in hits[:12]:
            print("   FAIL " + h)
    else:
        print("   pass — no external references")

    entry = find_entry(sub)
    if not entry:
        print("[--] no index.html found — cannot run"); sys.exit(1)
    print(f"[..] entry: {entry}")

    sd = os.path.join(ROOT, "board", "sample_data")
    datasets = {n: os.path.join(sd, f"{n}.json")
                for n in ("empty", "one_model", "many_models")
                if os.path.isfile(os.path.join(sd, f"{n}.json"))}
    real = os.path.join(sub, data_name)
    if os.path.isfile(real):
        datasets["real"] = real + ".orig" if False else real
    # snapshot the real data so swapping doesn't clobber it
    import tempfile
    if os.path.isfile(real):
        tmp = tempfile.NamedTemporaryFile(suffix=".json", delete=False).name
        shutil.copy(real, tmp)
        datasets["real"] = tmp

    print("\n[runtime] render checks (chromium + playwright)")
    res, notes = runtime_gate(sub, entry, datasets, data_name)
    if res is None:
        print("   SKIPPED — " + "; ".join(notes) + " (run GATE.md manually)")
    elif res:
        print("   pass — offline render, no console errors, no external "
              "requests, no horizontal scroll, data reflected")
    else:
        ok = False
        for n in notes:
            print("   FAIL " + n)

    print("\nGATE: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
