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
# Build the site

**Build a beautiful, mobile-friendly, interactive website that brings this
benchmark data (\`results.json\`) to life — not a boring dashboard.** Put it in
\`submission/\`.

See \`BRIEF.md\` for the data shape and the hard rules (zero external deps,
offline, mobile-responsive, loads \`results.json\`, handles the \`sample_data/\`
edge cases). Gate yourself: \`python3 <repo>/scripts/board_gate.py $DEST/submission\`.
EOF

echo "board workspace ready: $DEST"
echo "  1) build the dashboard in $DEST/submission/ (open an agent there)"
echo "  2) gate it:  python3 $SRC/scripts/board_gate.py $DEST/submission"
echo "  3) if it passes, it goes to blind rubric judging (board/RUBRIC.md)"
