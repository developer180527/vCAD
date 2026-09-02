#!/usr/bin/env python3
"""Compare two import-probe snapshots and say what got worse.

    tools/snapshot_diff.py baseline.json current.json

Exit status is 0 when nothing regressed and 1 when something did, so this is usable as a gate.

# Why a snapshot diff at all

"It still opens" is the weakest claim a CAD application can make about a file, and the easiest one
to keep making while the answer quietly degrades underneath. A release can go on reading every
fixture in the corpus while naming fewer of their faces, losing a reference that used to survive a
fillet, or starting to need healing on geometry that used to arrive sound. None of that shows up in
a pass/fail suite. It shows up here.

# The policy, stated rather than assumed

Not every change is a regression, and treating them alike produces a gate people learn to ignore.
Three classes:

  REGRESSION   something we could do, we can no longer do. Fails the run.
  DRIFT        something measurable moved in the worse direction, but nothing broke. Reported.
  IMPROVEMENT  the inverse. Reported, because a silent improvement is usually somebody's bug fix
               and worth knowing about -- and occasionally it is a measurement that stopped being
               honest, which is worth knowing about rather more.

Geometry counts are REGRESSIONS when they change at all. The same bytes must produce the same
solids, faces, edges and vertices; if they do not, the reader changed its mind about what the file
says, and that is exactly the silent kind of failure this exists to catch.

# Fixtures are matched by CHECKSUM, not by filename

A corpus that lives outside the repository gets re-downloaded, re-exported and replaced. Comparing
a snapshot against a different file that happens to share a name is worse than not comparing at
all: it invents differences that no code change caused. A name whose checksum moved is reported as
CHANGED and excluded from the comparison.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

# Boolean capabilities. Losing one is a regression; gaining one is an improvement.
CAPABILITIES = [
    ("read", "reads at all"),
    ("naming.named", "names its elements"),
    ("naming.stable", "names agree across two reads"),
    ("naming.modelable", "a reference survives a feature"),
    ("validity.healedValid", "valid after healing"),
]

# Counts that must not move at all for identical bytes.
EXACT_COUNTS = ["counts.solids", "counts.faces", "counts.edges", "counts.vertices"]

# Counts where up is worse, but not fatal.
DRIFT_UP_IS_WORSE = [
    "naming.unnamedFaces",
    "naming.unnamedEdges",
    "naming.unnamedVertices",
    "validity.invalidFaces",
    "validity.invalidEdges",
]

# The one that is never acceptable above zero, whatever it was before.
ZERO_TOLERANCE = ["naming.collisions"]


def dotted(record: dict, path: str):
    """Value at 'a.b', or None if any level is missing."""
    node = record
    for part in path.split("."):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def load(path: Path) -> dict[str, dict]:
    """Fixtures keyed by checksum. A snapshot with no checksums cannot be compared safely."""
    data = json.loads(path.read_text())
    fixtures = data.get("fixtures", [])
    missing = [f.get("file", "?") for f in fixtures if not f.get("checksum")]
    if missing:
        sys.exit(f"{path}: {len(missing)} fixture(s) have no checksum; cannot compare safely")
    return {f["checksum"]: f for f in fixtures}


def compare(before: dict, after: dict) -> tuple[list[str], list[str], list[str]]:
    regressions: list[str] = []
    drift: list[str] = []
    improvements: list[str] = []
    name = after.get("file", "?")

    for key, described in CAPABILITIES:
        was, now = dotted(before, key), dotted(after, key)
        # null means "not checked", which is not the same as false and must not read as a loss.
        if was is None or now is None or was == now:
            continue
        if was and not now:
            regressions.append(f"{name}: no longer {described}")
        else:
            improvements.append(f"{name}: now {described}")

    for key in EXACT_COUNTS:
        was, now = dotted(before, key), dotted(after, key)
        if was is None or now is None or was == now:
            continue
        regressions.append(f"{name}: {key} changed {was} -> {now} for identical bytes")

    for key in ZERO_TOLERANCE:
        now = dotted(after, key)
        if now:
            regressions.append(f"{name}: {key} is {now}; it must be zero")

    for key in DRIFT_UP_IS_WORSE:
        was, now = dotted(before, key), dotted(after, key)
        if was is None or now is None or was == now:
            continue
        line = f"{name}: {key} {was} -> {now}"
        (drift if now > was else improvements).append(line)

    # Needing healing where none was needed before means the READER changed, not the file.
    was_healed = dotted(before, "validity.healingChanged")
    now_healed = dotted(after, "validity.healingChanged")
    if was_healed is False and now_healed is True:
        drift.append(f"{name}: now needs healing where it did not before")

    return regressions, drift, improvements


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    baseline_path, current_path = Path(sys.argv[1]), Path(sys.argv[2])
    baseline, current = load(baseline_path), load(current_path)

    regressions: list[str] = []
    drift: list[str] = []
    improvements: list[str] = []

    for checksum, after in current.items():
        before = baseline.get(checksum)
        if before is not None:
            r, d, i = compare(before, after)
            regressions += r
            drift += d
            improvements += i

    # Corpus bookkeeping, kept apart from code regressions. A fixture that vanished is a fact about
    # the corpus; reporting it as a failure would train people to ignore the failures that matter.
    added = [f["file"] for c, f in current.items() if c not in baseline]
    removed = [f["file"] for c, f in baseline.items() if c not in current]

    # Same name, different bytes: the file itself was replaced, so any difference is meaningless.
    by_name_before = {f["file"]: c for c, f in baseline.items()}
    replaced = [f["file"] for c, f in current.items()
                if c not in baseline and f["file"] in by_name_before]

    def report(title: str, lines: list[str]) -> None:
        if not lines:
            return
        print(f"\n{title} ({len(lines)})")
        for line in sorted(lines):
            print(f"  {line}")

    print(f"{len(baseline)} fixtures in baseline, {len(current)} in current, "
          f"{len(set(baseline) & set(current))} compared")
    report("REGRESSIONS", regressions)
    report("drift", drift)
    report("improvements", improvements)
    if replaced:
        report("CHANGED FILES — same name, different bytes; not compared", replaced)
    if added:
        report("new fixtures", [f for f in added if f not in replaced])
    # A replaced file is already reported as CHANGED; listing it again as "no longer present" is
    # the same fact twice, and the second telling reads as a second problem.
    if [f for f in removed if f not in replaced]:
        report("fixtures no longer present", [f for f in removed if f not in replaced])

    if not regressions:
        print("\nno regressions")
    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main())
