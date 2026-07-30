#!/bin/sh
# CLI-level checks: the paths an agent or a person actually drives.
#
# Runs against a throwaway CHUTNI_HOME so a test never touches the real
# registry, and a throwaway work directory so it never touches real files.
#
# HOME is redirected too. CHUTNI_HOME only moves the registry, while §39
# discovery also sweeps the conventional locations under $HOME — so on a
# machine that has real stores in ~/ or ~/Documents, "discover finds nothing on
# a fresh machine" failed against the developer's own memory rather than
# against anything the suite created.
set -eu

CLI="${1:?usage: run.sh <path to chutni binary>}"
WORK="$(mktemp -d)"
CHUTNI_HOME="$WORK/home"
HOME="$WORK/home"
export CHUTNI_HOME HOME
mkdir -p "$HOME"
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

# The window between a file changing and the next verify. The lexical index
# still holds the old text, so the hit still comes back; what must not happen is
# search calling it "current" on the strength of two catalog columns agreeing
# with each other while the disk says otherwise.
printf 'completely different text about penguins\n' > "$WORK/docs/terns.md"
if "$CLI" search "arctic terns" --store "$WORK/Test.chutni" --json 2>&1 \
        | grep -q '"freshness": *"unverified"'; then
    printf '  pass  %s\n' "edited file is not called current before verify"
    pass=$((pass + 1))
else
    printf '  FAIL  %s\n' "edited file is not called current before verify"
    fail=$((fail + 1))
fi

# A changed file must not keep being served as current.
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

# ------------------------------------------------------- bounded coverage
#
# The paths an agent drives when it is deciding how much of a folder to open.

mkdir -p "$WORK/tree/Alpha/Deep" "$WORK/tree/Alpha/Other" "$WORK/tree/Beta"
printf 'top level notes\n'          > "$WORK/tree/top.md"
printf 'alpha notes on terns\n'     > "$WORK/tree/Alpha/a.md"
printf 'deep notes on octopus\n'    > "$WORK/tree/Alpha/Deep/d.md"
printf 'other notes on pangolin\n'  > "$WORK/tree/Alpha/Other/o.md"
printf 'beta notes on wombats\n'    > "$WORK/tree/Beta/b.md"

BOUNDED="$WORK/Bounded.chutni"
"$CLI" init "$BOUNDED" >/dev/null 2>&1

check_output "add-root records a depth bound" "max_depth 1" \
    "$CLI" add-root "$WORK/tree" --max-depth 1 --goal define \
        --definition-mode adaptive --store "$BOUNDED"

check_output "roots report their depth bound" "max_depth 1" \
    "$CLI" roots --store "$BOUNDED"

check_output "a bounded scan says what it did not open" "recorded but not opened" \
    "$CLI" scan --store "$BOUNDED"

# The point of the whole feature: a bounded scan must not read as an
# exhaustive one, and the tool must not let it.
check_output "a bounded scan refuses to imply completeness" \
    "not a complete reading of the subtree" \
    "$CLI" scan --store "$BOUNDED"

check_output "grandchildren past the bound are not indexed" "octopus" \
    sh -c "! $CLI search octopus --store '$BOUNDED' | grep -q d.md && echo octopus-absent"

check_output "children lists a directory's immediate entries" "Alpha" \
    "$CLI" children "$WORK/tree" --store "$BOUNDED"

check_output "an unopened directory is reported opaque" "opaque" \
    "$CLI" children "$WORK/tree/Alpha" --store "$BOUNDED"

check_output "coverage reports the requested depth" "max_depth        1" \
    "$CLI" coverage --store "$BOUNDED"

check_output "coverage separates policy completeness from reading it all" \
    "does not mean the whole subtree was read" \
    "$CLI" coverage --store "$BOUNDED"

check_output "observe opens exactly one directory" "directories enumerated  1" \
    "$CLI" observe "$WORK/tree/Alpha/Deep" --store "$BOUNDED"

check_output "an observed directory yields its contents" "octopus" \
    sh -c "$CLI search octopus --store '$BOUNDED'"

# Observing Deep must not have expanded anything else on the way. Other sits
# beside it at the same depth and was equally unopened; it must stay that way.
check_output "observing one directory does not expand its siblings" "pangolin" \
    sh -c "! $CLI search pangolin --store '$BOUNDED' | grep -q o.md && echo pangolin-absent"

check_output "an unobserved sibling is still opaque" "opaque" \
    sh -c "$CLI children '$WORK/tree/Alpha' --store '$BOUNDED' | grep Other"

# A shallow refresh knows nothing about regions it never entered.
rm "$WORK/tree/Beta/b.md"
check_output "a shallow refresh marks nothing missing outside its region" \
    '"sources_marked_missing": 0' \
    "$CLI" scan --max-depth 0 --json --store "$BOUNDED"

check_output "search results carry the region's coverage" "coverage_manifest_id" \
    "$CLI" search "top level" --json --store "$BOUNDED"

check_output "info separates files from directories" "directories" \
    "$CLI" info --store "$BOUNDED"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
