"""Owned temporary data, evidence persistence and digest-approved cleanup.

Destructive operations are Windows identity-bound and never follow reparse targets.
Raw manifests and evidence belong outside the source tree; this tool commits none.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import time
import uuid
import zipfile
from contextlib import ExitStack, contextmanager, nullcontext
from pathlib import Path

import hostlock
import machine_config as machine
import tmp_runs
import tool_paths
from _tmp_fs import LockedFile, metadata_matches, relative_path, snapshot

REPO = Path(__file__).resolve().parent.parent


def _json(path: Path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    with temporary.open("x", encoding="utf-8") as stream:
        json.dump(data, stream, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def _sha(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _active_tmp_processes(roots):
    if os.name != "nt":
        raise OSError("Destructive tmp cleanup requires Windows")
    query = "Get-CimInstance Win32_Process | Select-Object ProcessId,Name,ExecutablePath,CommandLine | ConvertTo-Json -Compress"
    result = subprocess.run(
        ["powershell", "-NoProfile", "-Command", query], capture_output=True, text=True, check=True
    )
    rows = json.loads(result.stdout)
    roots = [roots] if isinstance(roots, (str, Path)) else roots
    prefixes = [str(root).replace("\\", "/").casefold().rstrip("/") + "/" for root in roots]
    active = []
    for row in rows:
        if row["ProcessId"] == os.getpid():
            continue
        command = (row.get("CommandLine") or "").replace("\\", "/").casefold()
        executable = (row.get("ExecutablePath") or "").replace("\\", "/").casefold()
        if any(prefix in command or executable.startswith(prefix) for prefix in prefixes):
            active.append({"pid": row["ProcessId"], "name": row["Name"]})
    return active


def _load_manifest(path: Path, approved_sha: str):
    digest = hashlib.sha256()
    rows = []
    with path.open("rb") as stream:
        for line in stream:
            digest.update(line)
            if line.strip():
                rows.append(json.loads(line))
    if len(approved_sha) != 64 or digest.hexdigest() != approved_sha:
        raise RuntimeError("Approval digest does not match the exact parsed manifest")
    folded = set()
    allowed = {
        "KEEP_IN_PLACE",
        "PERSIST_BEFORE_DELETE",
        "DELETE_GENERATED",
        "DELETE_REVIEW",
        "UNLINK_REPARSE",
        "PRUNE_EMPTY_DIR",
    }
    for row in rows:
        relative_path(Path("root"), row["path"])
        key = row["path"].casefold()
        if key in folded or row["disposition"] not in allowed:
            raise ValueError("Duplicate path or unknown manifest disposition")
        folded.add(key)
    return rows


def _verify_zip_member(path, proof):
    with zipfile.ZipFile(path) as archive, archive.open(proof["member"]) as member:
        if hashlib.file_digest(member, "sha256").hexdigest() != proof["sha256"]:
            raise RuntimeError("Preserved ZIP member differs from the approved payload")


@contextmanager
def cleanup_leases():
    """Take host resources before registry authorization; finishing producers need that registry."""
    with (
        hostlock.lease("tmp-cleanup", "approved-manifest"),
        hostlock.lease("rig", "tmp-cleanup"),
        hostlock.lease("selftest", "tmp-cleanup"),
    ):
        yield


def apply_manifest(
    manifest: Path,
    approved_sha: str,
    *,
    root: Path,
    receipt_dir: Path,
    include_review: bool = False,
    approval_note: str = "",
    consumer_roots=None,
    _leases_held=False,
):
    """Apply exactly the approved snapshot, preserving every required source before removal."""
    root = Path(os.path.abspath(root))
    receipt_dir = Path(os.path.abspath(receipt_dir))
    if receipt_dir.is_relative_to(root) or receipt_dir.resolve().is_relative_to(root.resolve()):
        raise ValueError("Cleanup evidence must be outside the deletion tree")
    rows = _load_manifest(manifest, approved_sha)
    if any(r["disposition"] == "DELETE_REVIEW" for r in rows) and not include_review:
        raise ValueError("Loss-aware review entries require explicit --include-review approval")
    receipt_dir.mkdir(parents=True, exist_ok=False)
    receipt = {
        "schema": "tmp-cleanup-receipt-v1",
        "phase": "preflight",
        "root": str(root),
        "manifest": str(manifest),
        "manifest_sha256": approved_sha,
        "approval_note": approval_note,
        "include_review": include_review,
        "started_epoch": time.time(),
        "free_bytes_before": shutil.disk_usage(root).free,
        "entries": len(rows),
        "preserved_files": 0,
        "preserved_bytes": 0,
        "removed_files": 0,
        "removed_payload_bytes": 0,
        "unlinked_reparse": 0,
        "pruned_directories": 0,
        "nonempty_directories": 0,
        "compressed_files": 0,
        "source_data_moved": 0,
    }
    receipt_path = receipt_dir / "receipt.json"
    journal_path = receipt_dir / "deletions.jsonl"
    preserved = {}
    preserved_guards = {}
    try:
        with (
            nullcontext() if _leases_held else cleanup_leases(),
            ExitStack() as guards,
        ):
            frozen = receipt_dir / "approved_manifest.jsonl"
            shutil.copyfile(manifest, frozen)
            if _sha(frozen) != approved_sha:
                raise RuntimeError("Approval manifest changed while freezing execution evidence")
            active = _active_tmp_processes(consumer_roots if consumer_roots is not None else root)
            if active:
                raise RuntimeError(f"Active tmp consumers: {active}")
            changed = []
            for row in rows:
                path = relative_path(root, row["path"])
                try:
                    actual = snapshot(path, root)
                    if not metadata_matches(row, actual):
                        changed.append(row["path"])
                except OSError:
                    changed.append(row["path"])
            if changed:
                _json(receipt_dir / "changed_entries.json", changed)
                raise RuntimeError(
                    f"Approved snapshot changed: {len(changed)} entries; no deletion performed"
                )
            receipt["preflight_entries"] = len(rows)
            _json(receipt_path, receipt)
            needed = sum(r["logical"] for r in rows if r["disposition"] == "PERSIST_BEFORE_DELETE")
            if shutil.disk_usage(receipt_dir).free < needed:
                raise RuntimeError("Insufficient evidence-volume capacity; no deletion performed")
            # Keep operational objects pinned against replacement throughout the cleanup.
            for row in rows:
                if row["disposition"] == "KEEP_IN_PLACE":
                    guard = guards.enter_context(LockedFile(relative_path(root, row["path"])))
                    guard.validate(row)
                    if row.get("sha256") and guard.digest() != row["sha256"]:
                        raise RuntimeError(f"Operational input changed: {row['path']}")
            receipt["phase"] = "preserving"
            _json(receipt_path, receipt)
            for row in rows:
                if row["disposition"] != "PERSIST_BEFORE_DELETE":
                    continue
                if row["kind"] != "file" or not row.get("sha256"):
                    raise RuntimeError(
                        "Required preservation needs a regular file and payload digest"
                    )
                target = relative_path(receipt_dir / "retained", row["path"])
                target.parent.mkdir(parents=True, exist_ok=True)
                with (
                    LockedFile(relative_path(root, row["path"])) as source,
                    target.open("xb") as dest,
                ):
                    source.validate(row)
                    digest = hashlib.sha256()
                    for block in source.chunks():
                        digest.update(block)
                        dest.write(block)
                    dest.flush()
                    os.fsync(dest.fileno())
                    if digest.hexdigest() != row["sha256"]:
                        raise RuntimeError(f"Preservation source digest changed: {row['path']}")
                copy_guard = guards.enter_context(LockedFile(target))
                if copy_guard.digest() != row["sha256"]:
                    raise RuntimeError(f"Preserved copy verification failed: {row['path']}")
                preserved_guards[str(target)] = copy_guard
                preserved[row["path"]] = {
                    "copy": str(target),
                    "sha256": row["sha256"],
                    "bytes": row["logical"],
                }
                receipt["preserved_files"] += 1
                receipt["preserved_bytes"] += row["logical"]
            _json(receipt_dir / "preservation.json", preserved)
            # Resolve and pin all keeper objects, including newly archived original ZIPs.
            keeper_guards = dict(preserved_guards)
            for row in rows:
                proof = row.get("proof")
                if not proof:
                    continue
                keeper = Path(proof["keeper"])
                try:
                    rel = keeper.relative_to(root).as_posix()
                except ValueError:
                    rel = None
                if rel is not None:
                    if rel not in preserved or not proof.get("requires_keeper_persistence"):
                        raise RuntimeError(f"Keeper is not independently preserved: {row['path']}")
                    keeper = Path(preserved[rel]["copy"])
                    expected = preserved[rel]["sha256"]
                else:
                    expected = proof["sha256"]
                if str(keeper) not in keeper_guards:
                    guard = guards.enter_context(LockedFile(keeper))
                    if guard.digest() != expected:
                        raise RuntimeError(f"Keeper digest mismatch: {keeper}")
                    keeper_guards[str(keeper)] = guard
                if proof.get("member"):
                    _verify_zip_member(keeper, proof)
            receipt["phase"] = "removing"
            _json(receipt_path, receipt)
            removable = [
                r for r in rows if r["disposition"] not in ("KEEP_IN_PLACE", "PRUNE_EMPTY_DIR")
            ]
            with journal_path.open("x", encoding="utf-8", buffering=1) as journal:
                for index, row in enumerate(removable):
                    path = relative_path(root, row["path"])
                    with LockedFile(path, deleting=True) as obj:
                        obj.validate(row)
                        if row.get("sha256") and obj.digest() != row["sha256"]:
                            raise RuntimeError(f"Removal payload changed: {row['path']}")
                        journal.write(
                            json.dumps(
                                {
                                    "event": "intent",
                                    "path": row["path"],
                                    "file_id": row["file_id"],
                                    "kind": row["kind"],
                                }
                            )
                            + "\n"
                        )
                        obj.remove()
                    journal.write(
                        json.dumps(
                            {
                                "event": "removed",
                                "path": row["path"],
                                "bytes": row["logical"],
                                "kind": row["kind"],
                            }
                        )
                        + "\n"
                    )
                    if row["kind"].startswith("reparse_"):
                        receipt["unlinked_reparse"] += 1
                    else:
                        receipt["removed_files"] += 1
                        receipt["removed_payload_bytes"] += row["logical"]
                    if index % 256 == 0:
                        journal.flush()
                        os.fsync(journal.fileno())
                        _json(receipt_path, receipt)
                journal.flush()
                os.fsync(journal.fileno())
            # Directory mtimes necessarily change when children are removed. Identity remains bound;
            # delete-by-handle refuses nonempty directories, including unlisted newly created files.
            for row in sorted(
                (r for r in rows if r["disposition"] == "PRUNE_EMPTY_DIR"),
                key=lambda r: -r["path"].count("/"),
            ):
                try:
                    with LockedFile(relative_path(root, row["path"]), deleting=True) as obj:
                        obj.validate(row, directory=True)
                        obj.remove()
                    receipt["pruned_directories"] += 1
                except OSError as exc:
                    if getattr(exc, "winerror", None) not in (145,):
                        raise
                    receipt["nonempty_directories"] += 1
            remaining = [
                r["path"] for r in removable if os.path.lexists(relative_path(root, r["path"]))
            ]
            if remaining:
                raise RuntimeError(f"Approved removal paths remain: {len(remaining)}")
            for row in rows:
                if row["disposition"] == "KEEP_IN_PLACE":
                    if not metadata_matches(row, snapshot(relative_path(root, row["path"]), root)):
                        raise RuntimeError(f"Retained input changed: {row['path']}")
            receipt.update(
                phase="complete",
                remaining_approved_paths=0,
                kept_entries=sum(r["disposition"] == "KEEP_IN_PLACE" for r in rows),
                finished_epoch=time.time(),
                free_bytes_after=shutil.disk_usage(root).free,
            )
            receipt["observed_free_space_gain"] = (
                receipt["free_bytes_after"] - receipt["free_bytes_before"]
            )
            _json(receipt_path, receipt)
            return receipt
    except BaseException as exc:
        receipt.update(phase="aborted", error=repr(exc), finished_epoch=time.time())
        _json(receipt_path, receipt)
        raise


def apply_cleanup(plan: Path, approved_sha: str, *, _leases_held=False):
    raw = plan.read_bytes()
    if hashlib.sha256(raw).hexdigest() != approved_sha:
        raise RuntimeError("Cleanup plan digest changed")
    context = json.loads(raw)
    if Path(context["root"]) != REPO / "tmp":
        raise ValueError("Cleanup plan belongs to another repository")
    manifest = Path(context["manifest"])
    if _sha(manifest) != context["manifest_sha256"]:
        raise RuntimeError("Cleanup manifest changed")
    receipt_dir = tool_paths.evidence_path("cleanup/" + uuid.uuid4().hex)
    with nullcontext() if _leases_held else cleanup_leases(), tmp_runs.locked_plan(context):
        result = apply_manifest(
            manifest,
            context["manifest_sha256"],
            root=REPO / "tmp",
            receipt_dir=receipt_dir,
            approval_note="Approved producer-owned disposable outputs",
            consumer_roots=[
                Path(r["path"]) for r in context["runs"].values() if not r.get("parent_only")
            ],
            _leases_held=True,
        )
        tmp_runs.mark_cleaned(context)
    return result


def protect(path: Path, reason: str):
    root = REPO / "tmp"
    path = Path(os.path.abspath(path))
    if not reason.strip():
        raise ValueError("A continuing retention reason is required")
    with tmp_runs.registry_lock():
        row = snapshot(path, root)
        holds = tool_paths.state_path("tmp_holds.json")
        records = json.loads(holds.read_text()) if holds.exists() else {}
        records[row["path"]] = {"reason": reason, "identity": row}
        _json(holds, records)


def release_hold(path: Path, reason: str):
    """Release a reviewed hold, retaining its original reason and the new decision."""
    if not reason.strip():
        raise ValueError("A hold release needs an explicit adjudication reason")
    relative = Path(os.path.abspath(path)).relative_to(REPO / "tmp").as_posix()
    with tmp_runs.registry_lock():
        location = tool_paths.state_path("tmp_holds.json")
        holds = json.loads(location.read_text()) if location.exists() else {}
        previous = holds.pop(relative)
        _json(
            tool_paths.state_path("tmp_hold_events/" + uuid.uuid4().hex + ".json"),
            {
                "path": relative,
                "previous_hold": previous,
                "release_reason": reason,
                "epoch": time.time(),
            },
        )
        _json(location, holds)


def _check_unmanaged():
    root = REPO / "tmp"
    root.mkdir(exist_ok=True)
    hold_path = tool_paths.state_path("tmp_holds.json")
    holds = json.loads(hold_path.read_text()) if hold_path.exists() else {}
    unmanaged = []
    for path in root.iterdir():
        actual = snapshot(path, root)
        if path.name == "runs" and actual["kind"] == "dir":
            continue
        hold = holds.get(path.name)
        if hold and metadata_matches(hold["identity"], actual, directory=actual["kind"] == "dir"):
            continue
        unmanaged.append(path.name)
    if unmanaged:
        raise RuntimeError(
            "Unowned tmp entries need an explicit owner/retention decision: "
            + ", ".join(unmanaged[:20])
        )
    registered = {str(Path(data["path"])) for data in tmp_runs.records().values()}
    runs = root / "runs"
    if runs.exists():
        for purpose in runs.iterdir():
            if snapshot(purpose, root)["kind"] != "dir":
                raise RuntimeError("Unowned object in run hierarchy")
            for task in purpose.iterdir():
                if snapshot(task, root)["kind"] != "dir":
                    raise RuntimeError("Unowned object in task hierarchy")
                for owned in task.iterdir():
                    if str(owned) not in registered:
                        raise RuntimeError("Unregistered run directory: " + str(owned))


def pre_gate_cleanup(profile):
    if profile != "full":
        return {"profile": profile, "cleanup": "not-requested"}
    with cleanup_leases(), tmp_runs.registry_lock():
        plan = tool_paths.evidence_path("cleanup_plans/" + uuid.uuid4().hex + ".jsonl")
        context = tmp_runs.plan_cleanup(plan)
        plan_file = plan.with_suffix(plan.suffix + ".owners.json")
        if context["runs"]:
            apply_cleanup(plan_file, _sha(plan_file), _leases_held=True)
        _check_unmanaged()
    free = shutil.disk_usage(REPO).free
    minimum = int(machine.TMP_FULL_HEADROOM_BYTES)
    if free < minimum:
        raise RuntimeError(
            f"Insufficient full-gate headroom: {free} bytes free, {minimum} required"
        )
    print(f"[tmp] disposable runs cleaned; {free} bytes free (minimum {minimum})")
    return {"profile": profile, "free_bytes": free, "minimum_bytes": minimum}


def _existing_run(run_id):
    run = object.__new__(tmp_runs.Run)
    run.id = run_id
    run.data = tmp_runs.load(run_id)
    run.path = Path(run.data["path"])
    run.finished = False
    run.lock = None
    run.token = None
    return run


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("selftest", help="Exercise deletion and ownership boundaries")
    audit = sub.add_parser("audit-apply", help="Apply an explicitly approved legacy audit manifest")
    audit.add_argument("--manifest", type=Path, required=True)
    audit.add_argument("--approved-sha", required=True)
    audit.add_argument("--include-review", action="store_true")
    audit.add_argument("--approval-note", required=True)
    audit.add_argument("--receipt-dir", type=Path, required=True)
    new = sub.add_parser(
        "new", help="Open a manually owned task; it stays protected until explicit finish"
    )
    new.add_argument("--purpose", required=True)
    new.add_argument("--task", required=True)
    run_cmd = sub.add_parser("run", help="Own one command and mark its completed generated outputs")
    run_cmd.add_argument("--purpose", required=True)
    run_cmd.add_argument("--task", required=True)
    run_cmd.add_argument("cmd", nargs=argparse.REMAINDER)
    for name in ("register", "finish"):
        operation = sub.add_parser(name)
        operation.add_argument("--id", required=True)
        operation.add_argument("--evidence", action="append", default=[])
        if name == "register":
            operation.add_argument("--path", type=Path, required=True)
            operation.add_argument(
                "--role", choices=("generated", "input", "source", "evidence"), required=True
            )
        else:
            operation.add_argument("--verdict", required=True)
            operation.add_argument("--reviewed", action="store_true")
            operation.add_argument(
                "--reason",
                help="Required explanation for reviewed disposal; never changes the original gate verdict",
            )
    plan_cmd = sub.add_parser("plan", help="Freeze only finalized, unconsumed generated outputs")
    plan_cmd.add_argument("--out", type=Path, required=True)
    apply_cmd = sub.add_parser("apply")
    apply_cmd.add_argument("--plan", type=Path, required=True)
    apply_cmd.add_argument("--approved-sha", required=True)
    for operation in ("protect", "release"):
        hold = sub.add_parser(operation)
        hold.add_argument("--path", type=Path, required=True)
        hold.add_argument("--reason", required=True)
    sub.add_parser("status")
    args = parser.parse_args(argv)
    if args.command == "selftest":
        import tmp_lifecycle_test

        return tmp_lifecycle_test.main()
    if args.command == "audit-apply":
        result = apply_manifest(
            args.manifest,
            args.approved_sha,
            root=REPO / "tmp",
            receipt_dir=args.receipt_dir,
            include_review=args.include_review,
            approval_note=args.approval_note,
        )
    elif args.command == "new":
        run = tmp_runs.Run(args.purpose, args.task, adjudicator=True, manual=True)
        run.__enter__()
        try:
            result = {
                "id": run.id,
                "directory": str(run.path / "scratch"),
                "environment": run.child_env({}),
            }
        finally:
            tmp_runs._CURRENT.reset(run.token)
            run.lock.__exit__()
    elif args.command == "run":
        command = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd
        if not command:
            parser.error("run needs a command after --")
        with tmp_runs.Run(args.purpose, args.task, adjudicator=True) as run:
            code = subprocess.run(command, cwd=REPO, env=run.child_env(), check=False).returncode
            run.finish("PASS" if code == 0 else "FAIL")
        print(json.dumps({"id": run.id, "state": run.data["state"], "directory": str(run.path)}))
        return code
    elif args.command in ("register", "finish"):
        with tmp_runs.RunLock(args.id):
            run = _existing_run(args.id)
            if args.command == "finish" and run.data["state"] != "active" and not args.reviewed:
                parser.error(
                    "A closed or retained run needs explicit --reviewed --reason adjudication"
                )
            for path in args.evidence:
                run.register(Path(path), role="evidence")
            if args.command == "register":
                run.register(args.path, args.role)
            else:
                if args.reviewed:
                    if not args.reason or not args.reason.strip():
                        parser.error("--reviewed needs --reason")
                    if run.data.get("verdict") != args.verdict.upper():
                        parser.error("Reviewed disposal must preserve the original verdict")
                    run.data["disposal_review"] = {
                        "reason": args.reason,
                        "previous_verdict": run.data.get("verdict"),
                        "epoch": time.time(),
                    }
                    run.data["human"] = False
                    run.data["adjudicator"] = True
                run.finish(args.verdict)
            result = {"id": run.id, "state": run.data["state"]}
    elif args.command == "plan":
        result = tmp_runs.plan_cleanup(args.out)
        plan_file = args.out.with_suffix(args.out.suffix + ".owners.json")
        result.update(plan=str(plan_file), approved_sha=_sha(plan_file))
    elif args.command == "apply":
        result = apply_cleanup(args.plan, args.approved_sha)
    elif args.command in ("protect", "release"):
        (protect if args.command == "protect" else release_hold)(args.path, args.reason)
        result = {"operation": args.command, "path": str(args.path), "reason": args.reason}
    else:
        result = tmp_runs.records()
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
