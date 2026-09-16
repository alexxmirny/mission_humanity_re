"""Subprocess helpers that cannot leave a stray process tree behind (Windows).

WHY THIS EXISTS. On 2026-08-02 an unattended loop accumulated several orphaned build/selftest
process trees, each pinning a core. The chain, in order, because every link is needed to reproduce
it and each one is individually reasonable:

1. `build_selftest.bat` ran MSBuild with `/m` and DEFAULT NODE REUSE. MSBuild's worker nodes outlive
   the build by ~15 minutes and inherit whatever stdout handle the build was started with.
2. The callers used `subprocess.run(..., capture_output=True)` with NO TIMEOUT. Capturing reads the
   pipe until EOF, and EOF cannot arrive while a surviving node still holds the write end. The call
   blocks forever at 0% CPU with an empty log -- indistinguishable from a slow build.
3. The `net_selftest.exe` call on the next line also had no timeout, and the code under test was a
   deliberately MUTATED translation. A mutation that swallows a loop's advance is an infinite loop,
   which is the half that burned CPU rather than idling.
4. Killing the tracked Python did not take the tree with it: `cmd` -> MSBuild -> nodes ->
   `net_selftest.exe` share no job object, so each retry stacked another tree.

`/nodeReuse:false` (applied in the bat and in the README recipe) removes cause 1. This module
removes 2, 3 and 4: every call has a mandatory timeout, and a timeout kills the whole TREE rather
than the direct child.

RULE OF THUMB: if a command builds, links, or runs a test binary, call it through `run_capture`
here rather than `subprocess.run` -- especially from a script that might be launched without a
console. The unattended loop driver keeps its own `run()` (it needs the UTF-8 forcing and its own error
contract); the tree-kill is shared via `kill_tree`.
"""

from __future__ import annotations

import os
import subprocess

# Image names this repo's own tooling spawns and may orphan. NEVER add `java.exe` (that is Ghidra),
# and never widen this to a pattern -- the existing `kill_stray_game` comment in the unattended loop driver explains
# why an image-name kill has to be an explicit allowlist on this box.
STRAY_IMAGES = ("net_selftest.exe", "MSBuild.exe", "VBCSCompiler.exe")


def kill_tree(pid: int) -> None:
    """Kill a process and every descendant. Never raises.

    `taskkill /T` is the only reliable way to do this here: the children were started by `cmd` and a
    plain `p.kill()` reaches the shell only. Exit 128 means "no such process", which is the normal
    outcome when the child already exited -- not an error.
    """
    try:
        subprocess.run(
            ["taskkill", "/T", "/F", "/PID", str(pid)],
            capture_output=True,
            text=True,
            timeout=60,
        )
    except Exception:  # a failure to clean up must never mask the original failure
        pass


def run_capture(cmd, *, timeout, cwd=None, env=None, text=True):
    """`subprocess.run(capture_output=True)` with a MANDATORY timeout and a tree-kill on expiry.

    `timeout` is keyword-only and has no default on purpose: the bug this module exists for was a
    missing timeout, and a default would just move the mistake somewhere less visible. Pick one from
    the work -- a build is minutes, a selftest is tens of seconds.

    Raises `subprocess.TimeoutExpired` exactly as `subprocess.run` would, but only AFTER the tree is
    gone, so a caller that retries does not stack orphans.
    """
    full_env = {**os.environ, "PYTHONIOENCODING": "utf-8", **(env or {})}
    with subprocess.Popen(
        cmd,
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=text,
        encoding="utf-8" if text else None,
        errors="replace" if text else None,
        env=full_env,
    ) as p:
        try:
            out, err = p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            kill_tree(p.pid)
            p.kill()
            p.communicate()
            raise
    return subprocess.CompletedProcess(cmd, p.returncode, out, err)


def reap_strays(images=STRAY_IMAGES) -> list[str]:
    """Kill leftover build/test processes by image name. Returns the ones that were actually there.

    Safe to call unconditionally between sessions: `taskkill` exits 128 when nothing matches. The
    allowlist is the point -- see STRAY_IMAGES.
    """
    killed = []
    for image in images:
        try:
            p = subprocess.run(
                ["taskkill", "/IM", image, "/F", "/T"],
                capture_output=True,
                text=True,
                timeout=60,
            )
        except Exception:
            continue
        if p.returncode == 0:
            killed.append(image)
    return killed


if __name__ == "__main__":
    got = reap_strays()
    print("reaped: " + (", ".join(got) if got else "nothing"))
