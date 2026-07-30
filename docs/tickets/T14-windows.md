# T14 — Windows port

**Priority:** P2 · **Size:** L · **Depends on:** T15 for CI · **Spec:** §26, §39.2, §12.2; TASKS W1–W3
**Status:** proposed 2026-07-30

## Why

Half the desktop market, and a hard credibility requirement for the word
"protocol" — a cross-app memory format that runs on one OS family is a
macOS/Linux convention. It is P2 rather than P0 only because the beachhead
audience (CLI-agent users, indie LLM builders) skews mac/Linux and every P0
ticket compounds faster; it must not slip past the first external-adopter
conversation with a Windows-based host.

This ticket absorbs TASKS W1–W3 and adds the concrete inventory those items
gesture at.

## Known POSIX dependencies (from source, not from memory — re-inventory at start)

- `/dev/urandom` (uuid7 entropy) → `BCryptGenRandom`;
- `flock` advisory writer lock → `LockFileEx`/`UnlockFileEx` on the lock
  file; T10's holder-metadata write must stay atomic under the new primitive;
- `realpath` → `GetFinalPathNameByHandleW` (long-path aware);
- `lstat`/`st_mtimespec` → `GetFileAttributesExW` + 100ns FILETIME→ns
  conversion (mtime_ns precision differs; freshness comparisons must use the
  stored value's own precision, never re-derive with a different one);
- `opendir`/`readdir` → `FindFirstFileW` family — **listing-hash critical**:
  §13.5 sorts by raw entry-name bytes; on Windows names are UTF-16 and must
  be converted to the stored UTF-8 form *before* sorting so the same
  directory hashes identically cross-platform (this is the subtlest item in
  the port; conformance must prove it);
- fork-based tests in the conformance suite → process-spawn equivalents or
  scoped skips with printed reasons (never silent);
- path separators and `PATH_MAX` throughout.

## Deliverables

1. **W1 — platform shim**: `src/platform_{posix,win32}.c` behind one small
   internal header (entropy, lock, realpath, stat, dirwalk, atomic rename).
   First-party code above the shim stays C99/-Werror; no `#ifdef _WIN32`
   outside the shim files.
2. **W2 — registry location**: `%APPDATA%\chutni\registry.json` (roaming, as
   §39.2 permits), `CHUTNI_HOME` still overriding; documented in §39.2 with
   the exact path.
3. **W3 — path storage**: display_path stays UTF-8 with the drive-absolute
   form; `native_path_b64` (§12.2) populated for lossless UTF-16 round-trip;
   `file_identity_json` uses volume serial + file index (the dev/ino
   analogue), with §12.3's non-portability caveat unchanged.
4. **Build**: MSVC or clang-cl via a `Makefile.win` / CMake decision made at
   implementation (bias: smallest thing that keeps "no build system beyond
   make" morally true); vendored SQLite/BLAKE3 compile as shipped, as always.
5. **Conformance on Windows**: full suite runs; any scenario that cannot run
   (fork-based writer-exclusion checks) prints `GAP` with the reason — the
   suite's honesty rules apply to platforms too.
6. **Cross-platform store interchange test**: store written on macOS, packed
   (T08), unpacked + remapped on Windows, verified current, and vice versa —
   listing hashes byte-identical both ways. This is the test that makes the
   port *mean* something for the protocol.

## Acceptance criteria

- `make test`-equivalent green on Windows 11 x64 (named machine, compiler,
  versions) with GAPs enumerated, not hidden.
- The cross-platform interchange transcript exists both directions.
- macOS/Linux behavior byte-unchanged (existing suite green, listing hashes
  for the fixture trees identical before/after the shim refactor).
- CI matrix (after T15) covers all three OS families per commit.

## Evidence required

Windows suite transcript + interchange transcripts under `docs/evidence/`,
with the usual machine/compiler naming.

## Non-goals

- No Windows-native installer/packaging; build-from-source parity only.
- No long-path opt-out heroics — require the manifest long-path setting and
  document it.
- No ARM64-Windows claims until it is actually run there.
