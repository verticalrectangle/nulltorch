#!/usr/bin/env bash
# Create an isolated eval workspace for a model-under-test.
#
# Why: running an agent in the full repo is contaminated — it will find the
# reference solution (reference/), the oracle (fixtures/gen/), the held-out
# answers (fixtures/hidden/), or a prior submission (submissions/) and grade
# THAT instead of solving the task. This builds a workspace by WHITELIST — only
# the task, the open-book docs, the public fixtures (with their self-grade
# ground truth), and the grader — so the model must write its own converter and
# CANNOT cheat.
#
# Usage:   scripts/new_eval.sh <name> [language] [dest_dir] [--delta]
# Example: scripts/new_eval.sh kimi-cpp cpp
#          scripts/new_eval.sh kimi-delta cpp --delta   # memorization-gap cell
set -euo pipefail
cd "$(dirname "$0")/.."
SRC="$(pwd)"

# optional --delta anywhere in args: build a workspace for the delta-format
# variant (DELTA_SPEC.md deviations) — used to measure the memorization gap.
DELTA=0; args=()
for a in "$@"; do if [ "$a" = "--delta" ]; then DELTA=1; else args+=("$a"); fi; done
set -- "${args[@]}"

NAME="${1:?usage: new_eval.sh <name> [language] [dest_dir] [--delta]}"
LANG="${2:-cpp}"
DEST="${3:-$SRC/eval/$NAME}"

if [ -e "$DEST" ]; then echo "refusing: $DEST already exists" >&2; exit 1; fi
mkdir -p "$DEST/harness" "$DEST/docs" "$DEST/grader" "$DEST/fixtures" "$DEST/submission"

# --- whitelist copy (nothing that reveals or contains an answer) -------------
cp harness/TASK.md                                   "$DEST/harness/"
cp -r docs/openbook                                  "$DEST/docs/"
cp grader/grade.py grader/schema.py grader/selftest.py "$DEST/grader/"
# public dev fixtures WITH ground truth (self-grading is allowed); the hidden
# set is deliberately NOT copied — that is the authoritative held-out score.
if [ "$DELTA" = 1 ]; then
  cp delta/DELTA_SPEC.md "$DEST/"
  cp -r fixtures/public_delta "$DEST/fixtures/public"   # delta-encoded + gt
  COND=delta; HIDDEN=fixtures/hidden_delta
else
  for s in public t4 t5 t6 rvc; do
    [ -d "fixtures/$s" ] && cp -r "fixtures/$s" "$DEST/fixtures/"
  done
  COND=open_book; HIDDEN=fixtures/hidden
fi

# language-appropriate build.sh stub the model fills in
case "$LANG" in
  cpp)  ext="cpp"; build='g++ -std=c++20 -O2 convert.cpp -o convert' ;;
  rust) ext="rs";  build='rustc -O convert.rs -o convert' ;;
  go)   ext="go";  build='go build -o convert .' ;;
  *)    ext="src"; build='echo "TODO: compile to ./convert"; exit 1' ;;
esac
cat > "$DEST/submission/build.sh" <<EOF
#!/usr/bin/env bash
# Compile your converter to ./convert  (invocation: ./convert <file.pth> <out>)
set -e
cd "\$(dirname "\$0")"
$build
EOF
chmod +x "$DEST/submission/build.sh"

cat > "$DEST/START.md" <<EOF
# NullTorch eval — write your own solution

Read \`harness/TASK.md\` and implement the converter it specifies, in **$LANG**,
from scratch. There is NO reference solution in this workspace — you must build
it from the task, \`docs/openbook/\`, and by inspecting the fixtures.
$([ "$DELTA" = 1 ] && printf '%s\n' '' '**DELTA VARIANT — read `DELTA_SPEC.md` first.** The `.pth` files here deviate' 'from stock (changed zip magic, storage class names, and persistent-id tuple' 'order). A stock-format reader will NOT work unchanged; adapt to the spec.')

- Put your source + \`build.sh\` in \`submission/\` (invocation:
  \`./submission/convert <file.pth> <out_dir>\`; edit \`submission/build.sh\`).
- Self-grade against the public fixtures as you go:
    for d in fixtures/public/*/; do ./submission/convert "\$d/fixture.pth" \\
      "/tmp/o/\$(basename \$d)"; done
    python3 grader/grade.py --set fixtures/public /tmp/o
  (also \`fixtures/t4\` for the harder cases.)
- Do not fetch anything from the network. Stdlib only.

You are done when \`grade.py --set fixtures/public\` passes. Your real score is
computed later on a hidden fixture set you cannot see here.
EOF

echo "workspace ready: $DEST"
echo
echo "next:"
echo "  1) cd $DEST   # open opencode (with your model) HERE, then say:"
echo "       \"Read harness/TASK.md and START.md, then do it.\""
echo "  2) when it finishes, score it on the HIDDEN set from the real repo:"
echo "       cd $SRC && python3 harness/orchestrate.py \\"
echo "         --submission $DEST/submission \\"
echo "         --model-id $NAME --language $LANG --condition $COND \\"
echo "         --public $DEST/fixtures/public --hidden $HIDDEN \\"
echo "         --out results.json"
