#!/bin/bash
set -euo pipefail

REPO="${1:?usage: git_source_identity.sh REPO SCOPE}"
SCOPE="${2:?usage: git_source_identity.sh REPO SCOPE}"
SHA="$(git -C "$REPO" rev-parse --short=12 HEAD 2>/dev/null)"

SOURCE_HASH="$({
    git -C "$REPO" diff --binary HEAD -- "$SCOPE"
    while IFS= read -r -d '' file; do
        printf 'untracked\0%s\0' "$file"
        sha256sum "$REPO/$file" | cut -d' ' -f1
    done < <(git -C "$REPO" ls-files --others --exclude-standard -z -- "$SCOPE" |
        LC_ALL=C sort -z)
} | sha256sum | cut -d' ' -f1)"

if [ -z "$(git -C "$REPO" status --porcelain --untracked-files=normal -- "$SCOPE")" ]; then
    printf '%s\n' "$SHA"
else
    printf '%s-dirty-%s\n' "$SHA" "${SOURCE_HASH:0:12}"
fi
