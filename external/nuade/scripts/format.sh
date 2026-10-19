#!/usr/bin/env bash
# Format our own C/C++ code with clang-format (.clang-format at the repo root).
# Build trees (build*/) are never touched.
#
#   scripts/format.sh                format every file of ours in place
#   scripts/format.sh --check        list files that need formatting, change nothing (exit 1 if any)
#   scripts/format.sh --changed      only files that differ from HEAD or are untracked
#   scripts/format.sh <path>...      only these files or directories (still limited to our roots)

set -euo pipefail
cd "$(dirname "$0")/.."

ROOTS=(include src players)
EXT='\.(c|h|cpp|hpp)$'

check=0
changed=0
paths=()
for arg in "$@"; do
    case "$arg" in
        --check) check=1 ;;
        --changed) changed=1 ;;
        -h | --help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) paths+=("$arg") ;;
    esac
done

if [ ${#paths[@]} -eq 0 ]; then
    paths=("${ROOTS[@]}")
fi

files=()
if [ "$changed" -eq 1 ]; then
    while IFS= read -r f; do files+=("$f"); done < <(
        { git diff --name-only HEAD -- "${paths[@]}"; git ls-files --others --exclude-standard -- "${paths[@]}"; } \
            | grep -E "$EXT" | sort -u | while read -r f; do [ -f "$f" ] && echo "$f"; done)
else
    while IFS= read -r f; do files+=("$f"); done < <(git ls-files -- "${paths[@]}" | grep -E "$EXT")
fi

# Tracked files only (git ls-files) keeps build trees out; filter them again so that an explicit path cannot reach one.
kept=()
for f in "${files[@]}"; do
    case "$f" in
        build*/*) ;;
        *) kept+=("$f") ;;
    esac
done

if [ ${#kept[@]} -eq 0 ]; then
    echo "format.sh: nothing to format"
    exit 0
fi

if [ "$check" -eq 1 ]; then
    bad=0
    for f in "${kept[@]}"; do
        if ! clang-format --dry-run --Werror "$f" >/dev/null 2>&1; then
            echo "$f"
            bad=1
        fi
    done
    exit "$bad"
fi

clang-format -i "${kept[@]}"
echo "format.sh: formatted ${#kept[@]} files"
