#!/usr/bin/env bash
# NullTorch CI — exercise the whole benchmark end-to-end with NO torch:
# committed fixtures + the stdlib grader/reference/harness. Exits nonzero on
# any regression. Run locally with `bash scripts/ci.sh`.
set -euo pipefail
cd "$(dirname "$0")/.."
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== grader controls (positive + negative) on every fixture set =="
for s in public hidden public_delta hidden_delta t4 t5 rvc; do
  python3 grader/selftest.py "fixtures/$s" >/dev/null
  echo "  selftest $s: PASS"
done

echo "== reference converter must pass the grader (public, t4) =="
for set in public t4; do
  sub="$TMP/ref_$set"; mkdir -p "$sub"
  for fx in fixtures/$set/*/; do
    [ -f "${fx}fixture.pth" ] || continue
    python3 reference/convert.py "${fx}fixture.pth" "$sub/$(basename "$fx")" 2>/dev/null
  done
  python3 grader/grade.py --set "fixtures/$set" "$sub" >/dev/null
  echo "  reference $set: all pass"
done

echo "== RVC reference (config + f16->f32) must pass the grader =="
for fx in fixtures/rvc/*/; do
  [ -f "${fx}fixture.pth" ] || continue
  out="$TMP/rvc_$(basename "$fx")"; mkdir -p "$out"
  python3 reference/rvc_convert.py "${fx}fixture.pth" "$out" 2>/dev/null
  python3 grader/grade.py "$fx" "$out" >/dev/null
done
echo "  RVC: all pass"

echo "== board sample data validates against the results schema =="
python3 - <<'PY'
import json
try:
    import jsonschema
except ImportError:
    print("  jsonschema not installed; skipping board validation"); raise SystemExit(0)
s = json.load(open("board/schema/results.schema.json"))
for f in ("empty", "one_model", "many_models"):
    jsonschema.validate(json.load(open(f"board/sample_data/{f}.json")), s)
print("  board: 3 datasets valid")
PY

echo "== T6 safety: a robust reader must never execute/crash/hang on hostile input =="
python3 - <<'PY'
import json, os, subprocess, sys, tempfile
canary = json.load(open("fixtures/t6/t6_exec_reduce/t6.json"))["canary"]
cpath = os.path.join(tempfile.gettempdir(), canary)
if os.path.exists(cpath):
    os.remove(cpath)
bad = 0
for d in sorted(os.listdir("fixtures/t6")):
    fx = os.path.join("fixtures/t6", d, "fixture.pth")
    if not os.path.isfile(fx):
        continue
    out = tempfile.mkdtemp()
    try:
        r = subprocess.run([sys.executable, "reference/convert.py", fx, out],
                           timeout=30, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        if r.returncode < 0:
            print(f"  CRASH {d} (signal {-r.returncode})"); bad += 1
    except subprocess.TimeoutExpired:
        print(f"  HANG {d}"); bad += 1
if os.path.exists(cpath):
    print("  EXEC canary fired — code was executed!"); bad += 1
print("  T6 reference safety: OK" if not bad else f"  T6 safety issues: {bad}")
sys.exit(1 if bad else 0)
PY

echo "== example C++ submission compiles and holds its hidden-set score =="
if command -v g++ >/dev/null 2>&1 && [ -f submissions/claude-cpp/convert.cpp ]; then
  g++ -std=c++20 -O2 submissions/claude-cpp/convert.cpp -o "$TMP/convert"
  hsub="$TMP/claude_hidden"; mkdir -p "$hsub"
  for fx in fixtures/hidden/*/; do
    "$TMP/convert" "${fx}fixture.pth" "$hsub/$(basename "$fx")" >/dev/null 2>&1 || true
  done
  python3 grader/grade.py --set fixtures/hidden "$hsub" >/dev/null
  echo "  claude-cpp hidden set: all pass"
else
  echo "  (skipped: no g++ or submission source)"
fi

echo "ALL NULLTORCH CI CHECKS PASSED"
