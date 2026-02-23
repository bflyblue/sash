#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# Integration tests for sash

set -euo pipefail

SASH="${SASH_BIN:-$(dirname "$0")/../build/sash}"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

PASS=0
FAIL=0
TOTAL=0

pass() {
    PASS=$((PASS + 1))
    TOTAL=$((TOTAL + 1))
    printf "  PASS: %s\n" "$1"
}

fail() {
    FAIL=$((FAIL + 1))
    TOTAL=$((TOTAL + 1))
    printf "  FAIL: %s\n" "$1" >&2
}

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        pass "$desc"
    else
        fail "$desc (expected '$expected', got '$actual')"
    fi
}

assert_exit() {
    local desc="$1" expected="$2"
    shift 2
    local rc=0
    "$@" >/dev/null 2>&1 || rc=$?
    assert_eq "$desc" "$expected" "$rc"
}

assert_file_content() {
    local desc="$1" file="$2" expected="$3"
    if [ ! -f "$file" ]; then
        fail "$desc (file '$file' does not exist)"
        return
    fi
    local actual
    actual="$(cat "$file")"
    assert_eq "$desc" "$expected" "$actual"
}

echo "=== sash integration tests ==="
echo ""

# 1. Binary exists and is executable
if [ -x "$SASH" ]; then
    pass "binary exists and is executable"
else
    fail "binary exists and is executable ($SASH not found)"
fi

# 2. -h exits 0
assert_exit "-h exits 0" 0 "$SASH" -h

# 3. -h output contains "Usage:"
if "$SASH" -h 2>&1 | grep -q "Usage:"; then
    pass "-h output contains 'Usage:'"
else
    fail "-h output contains 'Usage:'"
fi

# 4. Invalid flag exits non-zero
assert_exit "invalid flag exits non-zero" 1 "$SASH" -Z

# 5. -n 0 rejected
assert_exit "-n 0 rejected" 1 "$SASH" -n 0

# 6. Passthrough: single line
out="$(echo hello | "$SASH")"
assert_eq "passthrough: single line" "hello" "$out"

# 7. Passthrough: multi-line
out="$(printf 'a\nb\nc\n' | "$SASH")"
expected="$(printf 'a\nb\nc')"
assert_eq "passthrough: multi-line" "$expected" "$out"

# 8. Passthrough: line count (100 lines)
count="$(seq 1 100 | "$SASH" | wc -l | tr -d ' ')"
assert_eq "passthrough: 100 lines" "100" "$count"

# 9. Passthrough: large input (10000 lines)
count="$(seq 1 10000 | "$SASH" | wc -l | tr -d ' ')"
assert_eq "passthrough: 10000 lines" "10000" "$count"

# 10. Empty input → no output
out="$(printf "" | "$SASH")"
assert_eq "empty input produces no output" "" "$out"

# 11. -n doesn't affect passthrough
out="$(printf 'a\nb\nc\nd\ne\n' | "$SASH" -n 2)"
expected="$(printf 'a\nb\nc\nd\ne')"
assert_eq "-n does not affect passthrough" "$expected" "$out"

# 12. -w creates file with correct content
f="$TMPDIR/w1.txt"
printf 'hello\nworld\n' | "$SASH" -w "$f" >/dev/null
assert_file_content "-w creates file" "$f" "$(printf 'hello\nworld')"

# 13. -w truncates existing file
f="$TMPDIR/w2.txt"
echo "old content" > "$f"
printf 'new\n' | "$SASH" -w "$f" >/dev/null
assert_file_content "-w truncates existing file" "$f" "new"

# 14. -a appends
f="$TMPDIR/a1.txt"
printf 'first\n' | "$SASH" -w "$f" >/dev/null
printf 'second\n' | "$SASH" -a "$f" >/dev/null
assert_file_content "-a appends" "$f" "$(printf 'first\nsecond')"

# 15. Multiple output files (-w + -w)
f1="$TMPDIR/m1.txt"
f2="$TMPDIR/m2.txt"
printf 'data\n' | "$SASH" -w "$f1" -w "$f2" >/dev/null
assert_file_content "multiple -w: file 1" "$f1" "data"
assert_file_content "multiple -w: file 2" "$f2" "data"

# 16. Mixed -w and -a
f="$TMPDIR/mix.txt"
printf 'base\n' | "$SASH" -w "$f" >/dev/null
printf 'added\n' | "$SASH" -a "$f" >/dev/null
assert_file_content "mixed -w and -a" "$f" "$(printf 'base\nadded')"

# 17. stdout AND file simultaneously
f="$TMPDIR/sim.txt"
out="$(printf 'both\n' | "$SASH" -w "$f")"
assert_eq "stdout and file: stdout" "both" "$out"
assert_file_content "stdout and file: file" "$f" "both"

# 18. -f doesn't break output
f="$TMPDIR/flush.txt"
printf 'flushed\n' | "$SASH" -f -w "$f" >/dev/null
assert_file_content "-f does not break output" "$f" "flushed"

# 19. Command mode basic
out="$("$SASH" echo hello)"
assert_eq "command mode basic" "hello" "$out"

# 20. Command mode with -w
f="$TMPDIR/cmd.txt"
"$SASH" -w "$f" echo hello >/dev/null
assert_file_content "command mode with -w" "$f" "hello"

# 21. Shell features in command mode
out="$("$SASH" 'echo a && echo b')"
expected="$(printf 'a\nb')"
assert_eq "shell features in command mode" "$expected" "$out"

# 22. -x direct exec
out="$("$SASH" -x echo hello)"
assert_eq "-x direct exec" "hello" "$out"

# 23. Exit code 0 (true)
assert_exit "exit code 0 (true)" 0 "$SASH" true

# 24. Exit code 1 (false)
assert_exit "exit code 1 (false)" 1 "$SASH" false

# 25. Exit code propagation (42)
assert_exit "exit code propagation (42)" 42 "$SASH" 'exit 42'

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="

[ "$FAIL" -eq 0 ]
