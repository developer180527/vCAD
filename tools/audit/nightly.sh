#!/usr/bin/env bash
#
# The periodic run: fetch the latest commit into a DEDICATED clone, audit it, build it, test it, and
# say what got worse since the last run.
#
#     tools/audit/nightly.sh nightly     # always run everything
#     tools/audit/nightly.sh changed     # run only if the branch moved since the last run
#
# Configured by environment (see tools/audit/README.md):
#
#   VCAD_REPO_URL      where to clone from                                  (required)
#   VCAD_AUDIT_HOME    working area: src/, reports/, state                  (default ~/vcad-audit)
#   VCAD_BRANCH        branch to follow                                     (default main)
#   VCAD_CMAKE_ARGS    extra configure args, e.g. the vcpkg toolchain       (default none)
#   VCAD_NOTIFY_CMD    shell command; the summary arrives on its stdin     (default none)
#   VCAD_KEEP_DAYS     prune reports older than this                        (default 30)
#   VCAD_SKIP_BUILD=1  audit only, no configure/build/ctest -- for testing THIS script
#
# # Why a dedicated clone
#
# Never the working tree people develop in. Several sessions share that one, with each other's
# half-written files in it; an audit of it would report their work in progress as drift, and a
# `git checkout` here would destroy it. This clone belongs to the job and nothing else.
#
# # Why it keeps going after a failure
#
# One report with every step's result, rather than the first failure and silence. "The build broke"
# and "the build broke AND the audit found a new copy of the stack" are different mornings.

set -uo pipefail

MODE="${1:-nightly}"
case "$MODE" in nightly|changed) ;; *) echo "usage: $0 nightly|changed" >&2; exit 2 ;; esac

: "${VCAD_REPO_URL:?set VCAD_REPO_URL to the repository to audit}"
HOME_DIR="${VCAD_AUDIT_HOME:-$HOME/vcad-audit}"
BRANCH="${VCAD_BRANCH:-main}"
KEEP_DAYS="${VCAD_KEEP_DAYS:-30}"
SRC="$HOME_DIR/src"
REPORTS="$HOME_DIR/reports"
mkdir -p "$REPORTS"

# One run at a time. A slow nightly overlapping the half-hourly poll would build into the same tree.
# `mkdir` is atomic everywhere, where flock(1) is Linux-only; a lock left by a killed run is taken
# over once it is older than the longest a run may take.
LOCK="$HOME_DIR/.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
    if [[ -n "$(find "$LOCK" -maxdepth 0 -mmin +420 2>/dev/null)" ]]; then
        echo "taking over a stale lock" >&2
    else
        echo "another audit run is in progress; skipping" >&2
        exit 0
    fi
fi
trap 'rmdir "$LOCK" 2>/dev/null' EXIT

if [[ ! -d "$SRC/.git" ]]; then
    git clone --quiet "$VCAD_REPO_URL" "$SRC" || { echo "clone failed" >&2; exit 1; }
fi
git -C "$SRC" fetch --quiet origin "$BRANCH" || { echo "fetch failed" >&2; exit 1; }
COMMIT="$(git -C "$SRC" rev-parse "origin/$BRANCH")"
LAST_FILE="$HOME_DIR/last-commit"

if [[ "$MODE" == "changed" && -f "$LAST_FILE" && "$(cat "$LAST_FILE")" == "$COMMIT" ]]; then
    exit 0   # nothing new; the nightly run still re-checks, which catches toolchain drift
fi

git -C "$SRC" checkout --quiet --detach "$COMMIT"
# Everything untracked goes, EXCEPT the build directory: an incremental build is the difference
# between minutes and hours, and a clean one happens whenever CMake decides it must.
git -C "$SRC" clean -ffdxq -e /build

# The runner itself can drift: this script is installed OUTSIDE the clone (so a checkout cannot
# rewrite it mid-run), which means an improvement committed to the repo never reaches it by itself.
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
RUNNER_NOTE=""
if [[ -f "$SRC/tools/audit/nightly.sh" ]] && ! cmp -s "$SELF" "$SRC/tools/audit/nightly.sh"; then
    RUNNER_NOTE="the installed runner differs from tools/audit/nightly.sh at this commit -- reinstall it"
fi

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$REPORTS/$STAMP-${COMMIT:0:10}"
mkdir -p "$OUT"

# Step results in a file, not an associative array: bash 3.2 (macOS) has none, and a runner that
# only works on one kind of machine is one more thing that drifts.
STEPS="$OUT/steps.txt"
: >"$STEPS"
set_status() { echo "$1=$2" >>"$STEPS"; }
status_of() { grep "^$1=" "$STEPS" | tail -1 | cut -d= -f2; }
step() {   # step NAME LOGFILE COMMAND...
    local name="$1" log="$2"; shift 2
    if "$@" >"$OUT/$log" 2>&1; then set_status "$name" ok; else set_status "$name" FAIL; fi
}
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

cd "$SRC" || exit 1
step audit          audit.txt     python3 tools/audit/audit.py . --json "$OUT/audit.json"
step audit_selftest selftest.txt  python3 tools/audit/selftest.py

if [[ "${VCAD_SKIP_BUILD:-0}" != 1 ]]; then
    # shellcheck disable=SC2086 -- VCAD_CMAKE_ARGS is deliberately word-split
    step configure configure.log cmake -S . -B build ${VCAD_CMAKE_ARGS:-}
    if [[ "$(status_of configure)" == ok ]]; then
        step build build.log cmake --build build -j "$JOBS"
    else
        set_status build skipped
    fi
    if [[ "$(status_of build)" == ok ]]; then
        # ctest carries everything else: the suite, layering, the ABI firebreak and golden (via the
        # Rust suite), docs claims, the two-paths conformance test, and arch_audit again.
        step ctest ctest.log ctest --test-dir build --output-on-failure \
            --output-junit "$OUT/ctest.xml" -j "$JOBS"
        if [[ -x build/shell_qt/vcad_probe ]]; then
            step shell_probe probe.log env QT_QPA_PLATFORM=offscreen build/shell_qt/vcad_probe
        fi
    else
        set_status ctest skipped
    fi
fi

# ── summary, and what got worse since the previous run ─────────────────────────────────────
PREV="$(ls -1d "$REPORTS"/*/ 2>/dev/null | grep -v "/$STAMP-" | grep -v "/latest/" | sort | tail -1)"

python3 - "$OUT" "${PREV%/}" "$COMMIT" "$RUNNER_NOTE" <<'PY'
import json, sys, xml.etree.ElementTree as ET
from pathlib import Path

out, prev, commit, runner_note = sys.argv[1:]
out = Path(out)
steps = {}
for line in (out / "steps.txt").read_text().splitlines():
    if "=" in line:
        name, state = line.split("=", 1)
        steps[name] = state
summary = {"commit": commit, "report": str(out), "steps": steps,
           "failed_tests": [], "audit": {}, "regressions": [], "notes": [runner_note] if runner_note else []}

audit = out / "audit.json"
if audit.is_file():
    a = json.loads(audit.read_text())
    summary["audit"] = {"new": [f"{i['where']}: {i['message']}" for i in a["new"]],
                        "stale": [f"{i['check']}: {i['key']}" for i in a["stale"]],
                        "problems": a["problems"], "debt": len(a["debt"])}

junit = out / "ctest.xml"
if junit.is_file():
    for case in ET.parse(junit).getroot().iter("testcase"):
        if case.find("failure") is not None or case.get("status") == "fail":
            summary["failed_tests"].append(case.get("name"))

if prev and (Path(prev) / "summary.json").is_file():
    before = json.loads((Path(prev) / "summary.json").read_text())
    for step, state in summary["steps"].items():
        if state != "ok" and before.get("steps", {}).get(step) == "ok":
            summary["regressions"].append(f"step {step}: ok -> {state}")
    for name in sorted(set(summary["failed_tests"]) - set(before.get("failed_tests", []))):
        summary["regressions"].append(f"test newly failing: {name}")
    if summary["audit"].get("debt", 0) > before.get("audit", {}).get("debt", 0):
        summary["regressions"].append("architectural debt grew: " +
                                      f"{before['audit'].get('debt')} -> {summary['audit']['debt']}")

summary["ok"] = all(v in ("ok", "skipped") for v in summary["steps"].values())
(out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

lines = [f"vCAD audit {'OK' if summary['ok'] else 'FAILED'} at {commit[:10]}"]
lines += [f"  {k}: {v}" for k, v in sorted(summary["steps"].items())]
for title, items in (("REGRESSIONS since the last run", summary["regressions"]),
                     ("new architecture findings", summary["audit"].get("new", [])),
                     ("stale baseline entries", summary["audit"].get("stale", [])),
                     ("audit problems", summary["audit"].get("problems", [])),
                     ("failing tests", summary["failed_tests"]),
                     ("notes", summary["notes"])):
    if items:
        lines.append(f"{title}:")
        lines += [f"  {i}" for i in items]
lines.append(f"tracked architectural debt: {summary['audit'].get('debt', '?')}")
lines.append(f"report: {out}")
(out / "summary.txt").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
PY

echo "$COMMIT" >"$LAST_FILE"
ln -sfn "$OUT" "$REPORTS/latest"
find "$REPORTS" -mindepth 1 -maxdepth 1 -type d -mtime +"$KEEP_DAYS" -exec rm -rf {} + 2>/dev/null

OK="$(python3 -c 'import json,sys; s=json.load(open(sys.argv[1])); print(int(s["ok"] and not s["regressions"]))' "$OUT/summary.json")"
if [[ -n "${VCAD_NOTIFY_CMD:-}" && ( "$OK" != 1 || -n "$RUNNER_NOTE" ) ]]; then
    sh -c "$VCAD_NOTIFY_CMD" <"$OUT/summary.txt" || echo "notify command failed" >&2
fi
[[ "$OK" == 1 ]]
