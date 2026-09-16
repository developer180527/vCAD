# The architecture audit

Structural rules, checked by machine, run on every build and on a schedule on a server that always
has the latest commit.

```bash
python3 tools/audit/audit.py .        # the audit: exit 1 on anything NEW
python3 tools/audit/selftest.py       # proves every rule can fail
ctest --test-dir build -L architecture
```

## Why this exists

`app::Controller` and `abi::Session` each assembled the document-computing stack by hand. Nothing
recorded that there were two, and they drifted in both directions: the shells had no disk cache tier,
and the first shared version would have removed the ABI's memory tier. It was found only because
someone extracted `cad::runtime::DocumentRuntime` by hand.

That fix consolidated two copies. The audit's first run found **four more**: the Python bindings and
three spikes still assemble their own. Code review cannot be relied on to find the copy nobody is
looking at. A rule that is checked every time can.

## The checks

| Check | Rule | The drift it comes from |
|---|---|---|
| `single_assembly` | The document stack (registry, caches, blob store, mesh cache) is built in `runtime/` only | Controller and Session each built their own and diverged |
| `layer_graph` | `cad/` includes follow a declared **allow-list** of layer dependencies; an undeclared directory is a finding | `runtime` could be included from `core`, because the old deny-lists had to be updated in every layer below and weren't |
| `ffi_mirror` | Every hand-written Rust `#[repr(C)]` struct matches its C struct field-for-field, in order (`…Prefix` mirrors match the leading fields) | `CadHost` was reordered in C; Rust called the wrong slot and a SIGSEGV silently killed 34 tests |

These sit beside the existing guards, which keep their own jobs: `tools/check_layering.py`
(third-party headers per layer), `tools/check_abi_firebreak.py`, the ABI golden in the Rust suite,
`docs_claims`, the `two_paths_agree` conformance test, and the shell probe.

## The baseline is a ratchet

`baseline.json` lists known violations, each with a reason.

- A finding **in** the baseline is reported as tracked debt and does not fail.
- A finding **not** in the baseline fails.
- A baseline entry that no longer matches a finding **also fails**. It is stale: the debt was paid,
  so delete the entry.

Debt can be paid down and never quietly added to. Fixing the code is the answer to a new finding.
Adding a baseline entry is a decision that needs a reason a reviewer can disagree with.

## Why the checks cannot pass vacuously

- **Floors.** Each check counts what it examined. If a directory moves or a parser breaks and the
  count collapses, the run fails instead of reading as clean.
- **Self-test.** `selftest.py` builds synthetic trees that violate every rule and asserts each is
  found. It also builds near-misses that must *not* be found (doc comments, references,
  `io::FormatRegistry::builtins()`), because a check that cries wolf gets ignored.
- **Stable keys.** Findings are keyed by file and component, not line number, so an unrelated edit
  does not churn the baseline.

## Adding a check when a new kind of drift turns up

1. Write it as a function in `audit.py` returning a `CheckResult`, and register it in `CHECKS`.
2. Add synthetic violations **and** near-misses for it to `selftest.py`.
3. Break the real tree on purpose and confirm the check fails, then restore it.
4. Run it; put today's genuine violations in the baseline, each with a reason.
5. Set its floor well below today's `examined` count.

## Running it periodically on a home server

`nightly.sh` works in a **dedicated clone**, never a working tree people develop in. It fetches the
branch, runs the audit and its self-test, configures, builds, runs all of ctest (the suite, layering,
firebreak, Rust suite and ABI golden, docs claims, conformance, and the audit again) and the offscreen
shell probe. It writes a report per run and compares it with the previous one. If a step went from ok
to failing, a test newly fails, or tracked debt grew, it notifies you.

### One-time setup (Linux, user systemd)

```bash
# 1. Install the runner OUTSIDE the clone, so a checkout can never rewrite it mid-run
mkdir -p ~/vcad-audit/bin ~/.config/systemd/user
git clone <repo> /tmp/vcad && cp /tmp/vcad/tools/audit/nightly.sh ~/vcad-audit/bin/

# 2. Configure: repo URL, toolchain, notification command
cp /tmp/vcad/tools/audit/vcad-audit.env.example ~/.config/vcad-audit.env
$EDITOR ~/.config/vcad-audit.env

# 3. Install and start the timers
cp /tmp/vcad/tools/audit/systemd/* ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now vcad-audit-nightly.timer vcad-audit-poll.timer
loginctl enable-linger "$USER"      # keep timers running when you are logged out
```

- **Nightly (03:00):** everything, even on an unchanged commit, to catch toolchain or dependency
  drift.
- **Every 30 minutes:** runs only if the branch moved, so drift is reported within half an hour of the
  commit that introduced it.

Reports land in `~/vcad-audit/reports/<time>-<commit>/` (`summary.txt`, `summary.json`, `audit.json`,
`ctest.xml`, logs), with `reports/latest` pointing at the newest. Run one by hand with
`systemctl --user start vcad-audit@nightly` and watch it with `journalctl --user -u vcad-audit@nightly -f`.

If the installed runner falls behind `tools/audit/nightly.sh` in the repo, the report says so. Reinstall
it with step 1.

Without systemd, cron works the same way:

```cron
0 3 * * *    . ~/.config/vcad-audit.env; export $(cut -d= -f1 ~/.config/vcad-audit.env | grep -v '^#'); ~/vcad-audit/bin/nightly.sh nightly
*/30 * * * * . ~/.config/vcad-audit.env; export $(cut -d= -f1 ~/.config/vcad-audit.env | grep -v '^#'); ~/vcad-audit/bin/nightly.sh changed
```

### Build what the product builds

Install Qt and pybind11 on the server, so the shell probe and the Python bindings are **compiled**.
Code that builds nowhere is exactly where drift hides: until the audit read its source, nothing noticed
that the bindings carried their own stack.

The server cannot run the Metal renderer or the native macOS window-chrome probe. Those still need a
Mac.
