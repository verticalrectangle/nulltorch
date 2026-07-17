#!/usr/bin/env bash
# Create an isolated workspace for the NullTorch-Board (front-end) task: a model
# builds a static, self-contained dashboard that visualizes a results.json.
#
# Unlike the code task there is no ground-truth answer to hide — the dashboard
# is judged, not graded. Isolation here just means a clean workspace with the
# brief + the hard requirements + data to build against, and NOTHING that biases
# it (no other board submission, no judge rubric).
#
# Usage:   scripts/new_board_eval.sh <name> [dest_dir]
# Example: scripts/new_board_eval.sh gpt-sol-board
set -euo pipefail
cd "$(dirname "$0")/.."
SRC="$(pwd)"
NAME="${1:?usage: new_board_eval.sh <name> [dest_dir]}"
DEST="${2:-$SRC/eval-board/$NAME}"
[ -e "$DEST" ] && { echo "refusing: $DEST exists" >&2; exit 1; }

mkdir -p "$DEST/schema" "$DEST/sample_data" "$DEST/submission"
cp board/BRIEF.md board/GATE.md              "$DEST/"          # task + hard gate
cp board/schema/results.schema.json          "$DEST/schema/"
cp board/sample_data/*.json                  "$DEST/sample_data/"  # incl. degenerate
# the data the dashboard loads by default: the real six-model run
cp harness/example_results.json              "$DEST/results.json"

cat > "$DEST/START.md" <<EOF
# NullTorch-Board eval — build the dashboard

Read \`BRIEF.md\` (the task) and \`GATE.md\` (the hard requirements you MUST pass
before any judging). Build a **static, self-contained, offline** results
explorer in \`submission/\` — a single \`index.html\` (inline CSS/JS) or a small
bundle of sibling files loaded by relative path.

- It loads \`results.json\` from its own directory (schema: \`schema/\`). A real
  six-model run is in \`./results.json\`; the \`sample_data/\` datasets
  (\`empty\`, \`one_model\`, \`many_models\`) are for development and are the
  degenerate cases you must handle gracefully.
- **ZERO external dependencies** — no CDN, no framework, no web fonts, no
  network. Charts are hand-drawn (inline SVG/canvas). This is gate item G0 and
  is auto-checked; any external URL fails you before a human looks.
- No horizontal page scroll; keyboard navigable; renders with no console errors
  on every dataset including the degenerate ones.

Test yourself: \`python3 <repo>/scripts/board_gate.py $DEST/submission\`.
EOF

echo "board workspace ready: $DEST"
echo "  1) build the dashboard in $DEST/submission/ (open an agent there)"
echo "  2) gate it:  python3 $SRC/scripts/board_gate.py $DEST/submission"
echo "  3) if it passes, it goes to blind rubric judging (board/RUBRIC.md)"
