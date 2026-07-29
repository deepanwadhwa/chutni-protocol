#!/bin/sh
# CLI-level checks: the paths an agent or a person actually drives.
#
# Runs against a throwaway CHUTNI_HOME so a test never touches the real
# registry, and a throwaway work directory so it never touches real files.
set -eu

CLI="${1:?usage: run.sh <path to chutni binary>}"
WORK="$(mktemp -d)"
CHUTNI_HOME="$WORK/home"
export CHUTNI_HOME
unset CHUTNI_STORE 2>/dev/null || true

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

pass=0
fail=0

check() {
    description="$1"
    if shift && "$@" >/dev/null 2>&1; then
        printf '  pass  %s\n' "$description"
        pass=$((pass + 1))
    else
        printf '  FAIL  %s\n' "$description"
        fail=$((fail + 1))
    fi
}

check_output() {
    description="$1"
    needle="$2"
    shift 2
    if "$@" 2>&1 | grep -q -- "$needle"; then
        printf '  pass  %s\n' "$description"
        pass=$((pass + 1))
    else
        printf '  FAIL  %s (expected to see: %s)\n' "$description" "$needle"
        fail=$((fail + 1))
    fi
}

printf 'Chutni CLI checks\n\n'

mkdir -p "$WORK/docs"
printf 'The migration pattern of arctic terns spans pole to pole.\n' > "$WORK/docs/terns.md"
printf 'unrelated notes\n' > "$WORK/docs/other.txt"

check_output "discover reports nothing on a fresh machine" "No Chutni memory" \
    "$CLI" discover

check "init creates a store" "$CLI" init "$WORK/Test.chutni"

check_output "discover finds the new store" "Test.chutni" \
    "$CLI" discover

check_output "discover --json is well-formed" '"count": 1' \
    "$CLI" discover --json

check "add-root authorizes a directory" \
    "$CLI" add-root "$WORK/docs" --store "$WORK/Test.chutni"

check "scan indexes it" "$CLI" scan --store "$WORK/Test.chutni"

check_output "search finds indexed content" "terns.md" \
    "$CLI" search "arctic terns" --store "$WORK/Test.chutni"

check_output "search --json reports a score_type" "score_type" \
    "$CLI" search "arctic" --json --store "$WORK/Test.chutni"

check_output "inspect shows provenance" "produced by" \
    "$CLI" inspect "$WORK/docs/terns.md" --store "$WORK/Test.chutni"

check_output "verify reports all current" "0 stale" \
    "$CLI" verify --store "$WORK/Test.chutni"

# Scanning without an authorized root must refuse rather than wander.
check "empty store refuses to scan without a root" \
    sh -c "$CLI init '$WORK/Empty.chutni' >/dev/null && ! $CLI scan --store '$WORK/Empty.chutni'"

# A changed file must not keep being served as current.
printf 'completely different text about penguins\n' > "$WORK/docs/terns.md"
"$CLI" verify --store "$WORK/Test.chutni" >/dev/null 2>&1 || true
if "$CLI" search "arctic terns" --store "$WORK/Test.chutni" 2>&1 | grep -q "No matches"; then
    printf '  pass  %s\n' "stale content withdrawn after verify"
    pass=$((pass + 1))
else
    printf '  FAIL  %s\n' "stale content withdrawn after verify"
    fail=$((fail + 1))
fi

check_output "rescan picks up new content" "penguins" \
    sh -c "$CLI scan --store '$WORK/Test.chutni' >/dev/null && $CLI search penguins --store '$WORK/Test.chutni'"

# Two stores must not be silently guessed between.
"$CLI" init "$WORK/Second.chutni" >/dev/null 2>&1
check_output "ambiguous store selection refuses to guess" "choose one with --store" \
    sh -c "$CLI info 2>&1 || true"

check_output "unregister removes a store from discovery" "Unregistered" \
    "$CLI" unregister "$WORK/Second.chutni"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
