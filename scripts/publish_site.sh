#!/usr/bin/env bash
# Publish a built site to nulltorch.com (GitHub Pages serves main / root).
# Copies the site into the repo root, keeps CNAME + .nojekyll, drops in the real
# results.json, gates it, and commits. The PUSH is left to you.
#
# Usage:   scripts/publish_site.sh <site_dir> [--force]
# Example: scripts/publish_site.sh eval-board/kimi-3/submission
#          scripts/publish_site.sh showcase          # a gallery bundle
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
SRC="${1:?usage: publish_site.sh <site_dir> [--force]}"
FORCE="${2:-}"
SRC="${SRC%/}"

[ -f "$SRC/index.html" ] || { echo "no index.html in $SRC" >&2; exit 1; }

# make sure the site has the real data next to it, then gate it
cp "$ROOT/harness/example_results.json" "$SRC/results.json"
echo "== gating $SRC =="
if ! python3 "$ROOT/scripts/board_gate.py" "$SRC" >/tmp/_pubgate.txt 2>&1; then
  tail -6 /tmp/_pubgate.txt
  if [ "$FORCE" != "--force" ]; then
    echo "GATE FAILED — refusing to publish (re-run with --force to override)" >&2
    exit 1
  fi
  echo "(--force: publishing despite gate failure)"
else
  echo "gate: PASS"
fi

# copy the site into the repo root (never touch .git / CNAME / .nojekyll / the
# benchmark dirs). The site's own files land at root; Pages serves them.
echo "== copying $SRC -> repo root =="
rsync -a --exclude='.git' "$SRC"/ "$ROOT"/
cp "$ROOT/harness/example_results.json" "$ROOT/results.json"

git add -A
if git diff --cached --quiet; then
  echo "nothing changed — site already published."
  exit 0
fi
git commit -q -m "site: publish $(basename "$(dirname "$SRC")")/$(basename "$SRC") to nulltorch.com"
echo
echo "committed. review with:  git show --stat"
echo "then publish live with:  git push origin main"
echo "nulltorch.com serves main/root — it goes live on push (once DNS resolves)."
