#!/usr/bin/env bash
# Apply an exact reviewed patch without discarding unrelated local changes.
# The optional SHA-256 permits migrating exactly one known previous patch.
set -euo pipefail
if [[ $# != 4 ]]; then
  echo "Usage: $0 <checkout> <patch> <previous-patch-sha256-or-dash> <label>" >&2
  exit 2
fi
CHECKOUT=$1
PATCH=$2
PREVIOUS_SHA=$3
LABEL=$4
[[ -f "$PATCH" ]] || { echo "Missing reviewed patch: $PATCH" >&2; exit 1; }
ACTUAL=$(mktemp "${TMPDIR:-/tmp}/anyios-reviewed-patch.XXXXXX")
trap 'rm -f "$ACTUAL"' EXIT

if git -C "$CHECKOUT" apply --reverse --check "$PATCH" >/dev/null 2>&1; then
  echo "$LABEL patch is already applied"
elif git -C "$CHECKOUT" apply --check "$PATCH" >/dev/null 2>&1; then
  # Check before changing anything, even if the patch happens to apply in a
  # checkout where some unrelated tracked file was edited by the developer.
  git -C "$CHECKOUT" diff --binary --full-index --ignore-submodules=all HEAD > "$ACTUAL"
  if [[ -s "$ACTUAL" ]]; then
    echo "$LABEL checkout contains unrelated local changes; patch was not applied" >&2
    exit 1
  fi
  git -C "$CHECKOUT" apply "$PATCH"
  echo "Applied $LABEL patch"
else
  # A cached or developer checkout may contain the exact prior reviewed patch.
  # Never reset or overwrite a tree with any other tracked modifications.
  git -C "$CHECKOUT" diff --binary --full-index --ignore-submodules=all HEAD > "$ACTUAL"
  actual_sha=$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$ACTUAL")
  if [[ "$PREVIOUS_SHA" == - || "$actual_sha" != "$PREVIOUS_SHA" ]]; then
    echo "$LABEL patch does not apply cleanly; refusing to replace unknown local changes" >&2
    exit 1
  fi
  # Reversing a worktree patch does not update Git's index. Refuse a staged
  # previous patch rather than leaving stale staged content behind. SDL's
  # normal intent-to-add entries have no staged contents and are fine.
  if ! git -C "$CHECKOUT" diff --cached --quiet --ignore-submodules=all HEAD; then
    echo "$LABEL checkout has staged changes; patch was not upgraded" >&2
    exit 1
  fi
  git -C "$CHECKOUT" apply --reverse --check "$ACTUAL"
  git -C "$CHECKOUT" apply --reverse "$ACTUAL"
  if ! git -C "$CHECKOUT" apply --check "$PATCH"; then
    git -C "$CHECKOUT" apply "$ACTUAL"
    echo "Could not upgrade $LABEL patch; restored previous patch" >&2
    exit 1
  fi
  if ! git -C "$CHECKOUT" apply "$PATCH"; then
    git -C "$CHECKOUT" apply "$ACTUAL"
    echo "Could not upgrade $LABEL patch; restored previous patch" >&2
    exit 1
  fi
  echo "Upgraded the exact previous $LABEL patch"
fi

# `git apply` creates untracked files, which a normal diff omits. Mark only
# files explicitly created by this patch; unrelated untracked files stay alone.
while IFS= read -r created_file; do
  [[ -n "$created_file" ]] || continue
  git -C "$CHECKOUT" add --intent-to-add -- "$created_file"
done < <(git -C "$CHECKOUT" apply --summary "$PATCH" | sed -nE 's/^ create mode [0-7]+ //p')

git -C "$CHECKOUT" diff --binary --full-index --ignore-submodules=all HEAD > "$ACTUAL"
if ! cmp -s "$PATCH" "$ACTUAL"; then
  echo "$LABEL checkout contains changes outside the reviewed patch" >&2
  exit 1
fi
git -C "$CHECKOUT" diff --check --ignore-submodules=all
