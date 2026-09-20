#!/usr/bin/env python3
"""lint_rust.py -- the Rust half of the repo gate: `cargo fmt --check` and `cargo clippy -D warnings`
over the whole workspace (dist DS1, plan decision D20).

Two `tools/lint_repo.py` rows call this, one per mode. It exists as its own file rather than as two
inline `check(...)` rows for ONE reason, and it is the reason the whole item exists:

  CARGO IS OPTIONAL ON A MACHINE AND THE GATE MUST SAY SO OUT LOUD.

The C++ and Python halves of this repo's lint can assume their formatter: clang-format ships with
the Build Tools the DLL already requires, and ruff is in tools/requirements.txt. Rust is different
-- it arrives via `rustup`, which nothing else in the tree needs, so a checkout on a machine without
it is a NORMAL state, not a broken one. That leaves exactly two wrong answers and one right one:

  WRONG   fail the gate       -- every session on a machine that does not build the launcher is red,
                                 and a red that is always red is a red nobody reads.
  WRONG   pass the gate       -- `cargo fmt --check` that never ran is indistinguishable in the log
                                 from `cargo fmt --check` that found nothing. This is the vacuous
                                 pass the repo's own --ci table calls out: "a vacuous pass is not a
                                 gate". Two unformatted crates would ship looking checked.
  RIGHT   SKIP, BY NAME, WITH THE REMEDY  -- exit 3. lint_repo prints `[SKIP] ... -- cargo not
                                 found ...` and keeps the gate green, so the log states which rows
                                 did not run and how to make them run.

So the exit codes are a three-valued verdict, not a boolean:

  0  the check ran and passed
  1  the check ran and FAILED (or cargo itself errored -- a missing component, a broken manifest)
  3  the check DID NOT RUN: no cargo on PATH. The reason line names the remedy.

WHERE CARGO COMES FROM. `MH_CARGO` (an explicit path, the same MH_-prefixed override shape
tools/machine_config.py uses) and otherwise PATH, and NOTHING ELSE -- in particular NOT a guess at
`~/.cargo/bin`, even though that is where rustup puts it. A fallback to the default install location
would make the skip arm unreachable on any machine that has ever run rustup, which is to say
unprovable exactly where it matters; and it would paper over the real post-install state, where the
toolchain is installed but the CURRENT shell's PATH predates it. `tools/bootstrap.py` reports that
state as its own distinct red ("installed at ... but not on this shell's PATH"), which is the
actionable message; this file just does not run.

WHY --all-targets ON CLIPPY. Without it clippy checks the default target set only, so a warning in
a `#[test]` or a `[[bin]]` that is not the default one is not seen. The tracker clause names the
flag; it is also what makes the row keep meaning something once the crates grow tests.

usage:
  python tools/lint_rust.py --fmt        # cargo fmt --all --check
  python tools/lint_rust.py --clippy     # cargo clippy --workspace --all-targets -- -D warnings
  python tools/lint_rust.py --selftest   # planted negatives for the three-valued verdict itself
"""

import argparse
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

OK, FAILED, SKIPPED = 0, 1, 3

# The remedy printed on the SKIP line. Deliberately the full two-step: rustup-init also has to put
# %USERPROFILE%\.cargo\bin on PATH, and a shell opened before that happened is the common case.
SKIP_REASON = (
    "cargo not found (no MH_CARGO, none on PATH) -- install rustup "
    "(https://win.rustup.rs/x86_64, `rustup-init -y --profile minimal --default-toolchain stable`) "
    "and open a new shell so %USERPROFILE%\\.cargo\\bin is on PATH; "
    "rust-toolchain.toml then pins the channel + rustfmt/clippy"
)

MODES = {
    # mode -> (argv after cargo, human label)
    "fmt": (["fmt", "--all", "--check"], "cargo fmt --all --check"),
    "clippy": (
        ["clippy", "--workspace", "--all-targets", "--", "-D", "warnings"],
        "cargo clippy --workspace --all-targets -- -D warnings",
    ),
}


def resolve_cargo(env=None):
    """The cargo to use, or None. MH_CARGO (explicit path) wins over PATH; nothing else is tried."""
    env = os.environ if env is None else env
    explicit = env.get("MH_CARGO")
    if explicit:
        return explicit
    return shutil.which("cargo", path=env.get("PATH"))


def run_mode(mode, cargo=None, runner=None, cwd=REPO):
    """Run one mode. Returns (exit_code, text). Pure of argparse/printing so --selftest can drive it.

    `cargo` None means "resolve it"; `runner` is the subprocess.run seam the selftest replaces."""
    argv, label = MODES[mode]
    if cargo is None:
        cargo = resolve_cargo()
    if not cargo:
        return SKIPPED, "%s -- %s" % (label, SKIP_REASON)
    runner = runner or subprocess.run
    r = runner([cargo] + argv, cwd=cwd, capture_output=True, text=True)
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0:
        return OK, label
    # A missing component reads like a formatting failure unless the remedy is spelled out; rustup
    # only installs rustfmt/clippy for the `minimal` profile when something asks for them.
    hint = ""
    if "not installed" in out or "no such command" in out:
        hint = (
            "\n      (the toolchain lacks the component -- `rustup component add rustfmt clippy`, "
            "or let rust-toolchain.toml's `components` line install it on the next cargo run)"
        )
    return FAILED, "%s FAILED%s\n%s" % (label, hint, out)


def _fake_runner(code, text=""):
    class _R:
        returncode = code
        stdout = text
        stderr = ""

    def run(*_a, **_k):
        return _R()

    return run


def selftest():
    """Planted negatives for the three-valued verdict. Costs no cargo run -- the subprocess is a
    seam, so every arm here is hermetic and instant.

    The arms are the three ways this row can lie: report OK when it never ran, report FAILED when
    cargo is merely absent, and report OK when cargo said no."""
    fails = []

    def arm(ok, label, detail=""):
        print("  [%s] %s%s" % ("ok" if ok else "FAIL", label, ("  -- " + detail) if detail else ""))
        if not ok:
            fails.append(label)

    for mode in sorted(MODES):
        code, text = run_mode(mode, cargo="", runner=_fake_runner(0))
        arm(code == SKIPPED, "%s: absent cargo SKIPS (3), never passes" % mode, "got %d" % code)
        arm(
            "rustup" in text and "PATH" in text,
            "%s: the skip line carries the remedy" % mode,
            text[:60],
        )
        code, _ = run_mode(mode, cargo="cargo", runner=_fake_runner(0))
        arm(code == OK, "%s: a clean cargo run is OK (0)" % mode, "got %d" % code)
        code, text = run_mode(mode, cargo="cargo", runner=_fake_runner(1, "planted diagnostic"))
        arm(code == FAILED, "%s: a nonzero cargo run FAILS (1)" % mode, "got %d" % code)
        arm("planted diagnostic" in text, "%s: the failure text carries cargo's output" % mode)
        code, text = run_mode(mode, cargo="cargo", runner=_fake_runner(1, "not installed for the"))
        arm("rustup component add" in text, "%s: a missing component names its own remedy" % mode)

    # The resolution order, which is what makes the skip arm reachable at all.
    # A BARE NAME, not an absolute path: tools/lint_machine_paths.py reds a drive-absolute path in
    # any published file, and a planted value is still a value in the tree.
    planted = "planted-cargo-that-does-not-exist"
    arm(
        resolve_cargo({"MH_CARGO": planted, "PATH": ""}) == planted,
        "MH_CARGO wins over PATH",
    )
    arm(resolve_cargo({"PATH": ""}) is None, "an empty PATH resolves no cargo (no ~/.cargo guess)")

    # The workspace this lints must actually be the one the tracker describes -- a row pointed at a
    # manifest with no members would pass every arm above and check nothing.
    manifest = os.path.join(REPO, "Cargo.toml")
    text = ""
    if os.path.isfile(manifest):
        with open(manifest, encoding="utf-8") as fh:
            text = fh.read()
    arm("src/launcher" in text and "src/relay" in text, "the workspace names both member crates")

    print("lint_rust --selftest:", "PASS" if not fails else "FAIL (%d)" % len(fails))
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--fmt", action="store_true", help="cargo fmt --all --check")
    g.add_argument(
        "--clippy", action="store_true", help="cargo clippy --all-targets -- -D warnings"
    )
    g.add_argument("--selftest", action="store_true", help="planted negatives for the verdict")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    code, text = run_mode("fmt" if args.fmt else "clippy")
    print(text)
    return code


if __name__ == "__main__":
    sys.exit(main())
