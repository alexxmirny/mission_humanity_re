"""Producer-owned scratch, immutable completion snapshots and explicit adjudication.

This module never deletes data. tmp_lifecycle applies its digest-bound plans using
native identity checks. Parent locks cover consumers after a child process exits.
"""

from __future__ import annotations

import contextvars
import datetime
import errno
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time
import uuid
from contextlib import contextmanager
from pathlib import Path

import tool_paths
from _tmp_fs import LockedFile, metadata_matches, process_identity, relative_path, snapshot

REPO = Path(__file__).resolve().parent.parent
_CURRENT = contextvars.ContextVar("tmp_run", default=None)
_PARENT = "MH_TMP_PARENT_ID"


def _json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    with temporary.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def _meta(run_id):
    if not re.fullmatch(r"[A-Za-z0-9_-]+", run_id):
        raise ValueError("Invalid lifecycle run ID")
    return tool_paths.state_path("tmp_runs/" + run_id + ".json")


def load(run_id):
    return json.loads(_meta(run_id).read_text(encoding="utf-8"))


def _sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class RunLock:
    def __init__(self, run_id):
        self.path = _meta(run_id).with_suffix(".lock")
        self.file = None

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        stream = self.path.open("a+b")
        try:
            if stream.tell() == 0:
                stream.write(b" ")
                stream.flush()
            stream.seek(0)
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BaseException:
            stream.close()
            raise
        self.file = stream
        return self

    def __exit__(self, *_):
        if self.file is not None:
            self.file.close()
            self.file = None


_REGISTRY_THREADS = threading.RLock()
_REGISTRY_DEPTH = threading.local()


@contextmanager
def registry_lock():
    """Serialize authorization with registrations, holds and adjudication changes."""
    with _REGISTRY_THREADS:
        if getattr(_REGISTRY_DEPTH, "value", 0):
            yield
            return
        lock = RunLock("__registry__")
        deadline = time.monotonic() + 300
        while True:
            try:
                lock.__enter__()
                break
            except OSError as exc:
                if (
                    exc.errno not in (errno.EACCES, errno.EAGAIN, errno.EDEADLK)
                    or time.monotonic() >= deadline
                ):
                    raise
                time.sleep(0.05)
        _REGISTRY_DEPTH.value = 1
        try:
            yield
        finally:
            _REGISTRY_DEPTH.value = 0
            lock.__exit__()


def _slug(value):
    result = re.sub(r"[^A-Za-z0-9_-]+", "-", str(value)).strip("-")[:80]
    if not result:
        raise ValueError("Run purpose/task must have an identifiable name")
    return result


def _children(run_id):
    folder = tool_paths.state_path("tmp_children/" + run_id)
    return [p.stem for p in folder.glob("*.json")] if folder.exists() else []


def _walk(path, root):
    pending = [path]
    while pending:
        current = pending.pop()
        row = snapshot(current, root)
        yield row
        if row["kind"] == "dir":
            with os.scandir(current) as entries:
                pending.extend(Path(entry.path) for entry in entries)


def _source_info(parent):
    if parent:
        return parent.get("source", {})
    env = dict(os.environ, GIT_OPTIONAL_LOCKS="0")
    commit = subprocess.run(
        ["git", "-C", str(REPO), "rev-parse", "HEAD"],
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        env=env,
        check=False,
    )
    status = subprocess.run(
        ["git", "-C", str(REPO), "status", "--porcelain", "--untracked-files=normal"],
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        env=env,
        check=False,
    )
    return {
        "commit": commit.stdout.strip() if commit.returncode == 0 else None,
        "dirty": bool(status.stdout) if status.returncode == 0 else None,
    }


class Run:
    def __init__(self, purpose, task=None, adjudicator=False, *, manual=False, human=False):
        self.id = (
            datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S")
            + "_"
            + uuid.uuid4().hex[:12]
        )
        parent_id = os.environ.get(_PARENT)
        current = _CURRENT.get()
        if current is not None:
            parent_id = current.id
        parent = load(parent_id) if parent_id else None
        if parent:
            owner = parent["owner"]
            if parent["state"] != "active" or (
                not parent.get("manual") and process_identity(owner["pid"]) != owner["created"]
            ):
                raise RuntimeError(
                    "Lifecycle parent is no longer an active, identity-verified owner"
                )
        self.path = REPO / "tmp" / "runs" / _slug(purpose) / _slug(task or "routine") / self.id
        self.data = {
            "schema": "tmp-run-v1",
            "id": self.id,
            "parent": parent_id,
            "purpose": purpose,
            "task": task,
            "path": str(self.path),
            "owner": {"pid": os.getpid(), "created": process_identity(os.getpid())},
            "manual": manual,
            "human": human,
            "adjudicator": adjudicator,
            "source": _source_info(parent),
            "state": "active",
            "items": [],
            "archives": {},
            "events": [],
        }
        self.lock = None
        self.token = None
        self.finished = False

    def __enter__(self):
        self.lock = RunLock(self.id)
        self.lock.__enter__()
        try:
            with registry_lock():
                if self.data["parent"]:
                    parent = load(self.data["parent"])
                    owner = parent["owner"]
                    if parent["state"] != "active" or (
                        not parent.get("manual")
                        and process_identity(owner["pid"]) != owner["created"]
                    ):
                        raise RuntimeError("Lifecycle parent finished before child registration")
                self.path.mkdir(parents=True, exist_ok=False)
                (self.path / "scratch").mkdir()
                self.register(self.path)
                self.token = _CURRENT.set(self)
                if self.data["parent"]:
                    _json(
                        tool_paths.state_path(
                            "tmp_children/" + self.data["parent"] + "/" + self.id + ".json"
                        ),
                        {"id": self.id},
                    )
                self.save()
            return self
        except BaseException:
            if self.token is not None:
                _CURRENT.reset(self.token)
            self.lock.__exit__()
            raise

    def save(self):
        with registry_lock():
            _json(_meta(self.id), self.data)

    def register(self, path, role="generated", source=None):
        with registry_lock():
            path = Path(os.path.abspath(path))
            if role not in ("generated", "input", "source", "evidence"):
                raise ValueError("Unknown output role")
            temporary = REPO / "tmp"
            existing = next((r for r in self.data["items"] if r["path"] == str(path)), None)
            identity = (
                snapshot(path, temporary)
                if path.is_relative_to(temporary) and os.path.lexists(path)
                else None
            )
            value = {
                "path": str(path),
                "role": role,
                "source": str(source) if source else None,
                "identity": identity,
            }
            if existing:
                existing.update(value)
            else:
                self.data["items"].append(value)
            self.save()

    def child_env(self, base=None):
        env = dict(os.environ if base is None else base)
        env[_PARENT] = self.id
        env["MH_TOOL_STATE_ROOT"] = str(tool_paths.state_path("tmp_runs").parent)
        env["MH_TOOL_INPUT_ROOT"] = str(tool_paths.input_path("state_matrix.json").parent)
        env["MH_EVIDENCE_ROOT"] = str(tool_paths.evidence_path("runs").parent)
        env["MH_TMP_WORK_DIR"] = str(self.path / "scratch")
        return env

    def _persist(self, path):
        path = Path(path)
        if not os.path.lexists(path):
            raise FileNotFoundError(path)
        destination = tool_paths.evidence_path(
            "runs/" + self.id + "/" + hashlib.sha256(str(path).encode()).hexdigest()[:12]
        )
        for row in _walk(path, path.parent):
            source = path.parent / row["path"]
            if row["kind"].startswith("reparse_"):
                raise RuntimeError("Evidence contains an unadjudicated reparse point")
            if row["kind"] != "file":
                continue
            target = destination / "payload" / row["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            with LockedFile(source) as held:
                held.validate(row)
                previous = self.data["archives"].get(str(source))
                if (
                    previous
                    and held.digest() == previous["sha256"]
                    and _sha(previous["keeper"]) == previous["sha256"]
                ):
                    continue
                digest = hashlib.sha256()
                if target.exists():
                    expected = held.digest()
                else:
                    with target.open("xb") as stream:
                        for block in held.chunks():
                            digest.update(block)
                            stream.write(block)
                        stream.flush()
                        os.fsync(stream.fileno())
                    expected = digest.hexdigest()
                if _sha(target) != expected:
                    raise RuntimeError("Persisted evidence hash mismatch")
            self.data["archives"][str(source)] = {"keeper": str(target), "sha256": expected}
        _json(
            destination / "provenance.json",
            {"source": str(path), "run": self.id, "files": self.data["archives"]},
        )

    def _persist_failed_sources(self):
        import _rundir

        pending = [self.id]
        gaps = []
        while pending:
            run_id = pending.pop()
            record = self.data if run_id == self.id else load(run_id)
            if run_id != self.id and record.get("verdict") == "PASS" and record.get("adjudicator"):
                continue
            self.data["archives"].update(record.get("archives", {}))
            pending.extend(_children(run_id))
            for item in record["items"]:
                if item["role"] != "source":
                    continue
                target = Path(item["path"])
                if not target.is_dir():
                    gaps.append({"source": str(target), "error": "source directory unavailable"})
                    continue
                for member in _rundir.process_and_sessions(str(target)):
                    try:
                        self._persist(Path(member))
                    except (OSError, RuntimeError, ValueError) as exc:
                        gaps.append({"source": str(member), "error": repr(exc)})
        if gaps:
            self.data["evidence_gaps"] = gaps
            raise RuntimeError("Some failed-run sources could not be preserved; see evidence_gaps")

    def finish(self, verdict):
        if self.finished:
            raise RuntimeError("Lifecycle run already finished")
        verdict = str(verdict).upper()
        self.data["verdict"] = verdict
        self.data["events"].append(
            {"verdict": verdict, "epoch": datetime.datetime.now(datetime.timezone.utc).timestamp()}
        )
        try:
            for item in self.data["items"]:
                path = Path(item["path"])
                if item["role"] in ("evidence", "input") and path.is_relative_to(REPO / "tmp"):
                    self._persist(path)
            if verdict != "PASS":
                self._persist_failed_sources()
            rows = {}
            for item in self.data["items"]:
                path = Path(item["path"])
                if item["role"] == "source" or not path.is_relative_to(REPO / "tmp"):
                    continue
                if item["identity"]:
                    actual = snapshot(path, REPO / "tmp")
                    if not metadata_matches(
                        item["identity"], actual, directory=actual["kind"] == "dir"
                    ):
                        raise RuntimeError("Registered output root was replaced")
                for row in _walk(path, REPO / "tmp"):
                    if row["kind"] == "file":
                        with LockedFile(REPO / "tmp" / row["path"]) as held:
                            held.validate(row)
                            row["sha256"] = held.digest()
                    rows[row["path"]] = row
            frozen = _meta(self.id).with_name(self.id + ".snapshot_" + uuid.uuid4().hex + ".jsonl")
            with frozen.open("x", encoding="utf-8") as stream:
                for row in sorted(rows.values(), key=lambda r: r["path"]):
                    stream.write(json.dumps(row) + "\n")
                stream.flush()
                os.fsync(stream.fileno())
            self.data["snapshot"] = str(frozen)
            self.data["snapshot_sha256"] = _sha(frozen)
            reviewed = bool(self.data.get("disposal_review"))
            self.data["state"] = (
                "disposable"
                if reviewed
                else "retained"
                if verdict != "PASS" or self.data["human"]
                else "disposable"
                if self.data["adjudicator"] or not self.data["parent"]
                else "awaiting_parent"
            )
            self.finished = True
            self.save()
            if self.data["state"] == "disposable":
                _adjudicate_children(self.id)
        except BaseException as exc:
            self.data.update(state="retained", archive_error=repr(exc))
            self.finished = True
            self.save()
            if verdict == "PASS":
                raise
            print("[tmp] evidence retention error: " + repr(exc), file=sys.stderr)

    def __exit__(self, kind, *_):
        try:
            if not self.finished:
                self.finish("INTERRUPTED" if kind else "UNADJUDICATED")
        finally:
            if self.token is not None:
                _CURRENT.reset(self.token)
            if self.lock is not None:
                self.lock.__exit__()


def _adjudicate_children(parent):
    for child_id in _children(parent):
        with RunLock(child_id), registry_lock():
            data = load(child_id)
            if data["state"] == "awaiting_parent" and data.get("verdict") == "PASS":
                data["state"] = "disposable"
                data["adjudicated_by"] = parent
                _json(_meta(child_id), data)
                _adjudicate_children(child_id)


@contextmanager
def run_scope(purpose, task=None, adjudicator=False):
    with Run(purpose, task, adjudicator) as run:
        yield run


def register(path, role="generated", source=None):
    current = _CURRENT.get()
    if current is not None:
        current.register(path, role, source)


def child_env(base=None):
    current = _CURRENT.get()
    if current:
        return current.child_env(base)
    env = dict(os.environ if base is None else base)
    for key in (
        _PARENT,
        "MH_TOOL_STATE_ROOT",
        "MH_TOOL_INPUT_ROOT",
        "MH_EVIDENCE_ROOT",
        "MH_TMP_WORK_DIR",
    ):
        if key in os.environ:
            env.setdefault(key, os.environ[key])
    return env


def current_output(subdir, *, run_id=None):
    current = _CURRENT.get()
    if run_id is None and current is not None:
        data = current.data
    else:
        owner_id = run_id or os.environ.get(_PARENT)
        if not owner_id:
            raise RuntimeError("Temporary output requires an owned lifecycle run")
        data = load(owner_id)
    owner = data["owner"]
    if data["state"] != "active" or (
        not data.get("manual") and process_identity(owner["pid"]) != owner["created"]
    ):
        raise RuntimeError("Temporary output owner is not active and identity-verified")
    path = relative_path(Path(data["path"]) / "scratch", subdir)
    path.mkdir(parents=True, exist_ok=True)
    return str(path)


def scratch_dir(default):
    explicit = os.environ.get("MH_RIG_SCRATCH")
    if explicit:
        return explicit
    if _CURRENT.get() is not None or os.environ.get(_PARENT):
        return current_output("ui")
    return str(default)


def run_cli(main, purpose, adjudicator=False, human_flags=(), offline_flags=None):
    offline = (
        {
            "--help",
            "-h",
            "--plan",
            "--list",
            "--selftest",
            "--classify-selftest",
            "--solo-steps-selftest",
            "--suite-config-selftest",
            "--check-budgets",
        }
        if offline_flags is None
        else set(offline_flags)
    )
    if any(arg in offline for arg in sys.argv[1:]):
        return main()
    human = any(arg in human_flags for arg in sys.argv[1:])
    with Run(purpose, adjudicator=adjudicator, human=human) as run:
        saved = {
            key: os.environ.get(key)
            for key in (
                _PARENT,
                "MH_TOOL_STATE_ROOT",
                "MH_TOOL_INPUT_ROOT",
                "MH_EVIDENCE_ROOT",
                "MH_TMP_WORK_DIR",
            )
        }
        os.environ.update({key: value for key, value in run.child_env({}).items() if key in saved})
        try:
            result = main()
            run.finish("PASS" if result == 0 else "FAIL")
            return result
        finally:
            for key, value in saved.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value


def records():
    folder = tool_paths.state_path("tmp_runs")
    return (
        {p.stem: json.loads(p.read_text(encoding="utf-8")) for p in folder.glob("*.json")}
        if folder.exists()
        else {}
    )


def _closed_ancestors(record, all_records):
    seen = set()
    parent = record.get("parent")
    while parent:
        if parent in seen or parent not in all_records:
            return False
        seen.add(parent)
        data = all_records[parent]
        if data["state"] not in ("disposable", "cleaned") or not (
            data.get("verdict") == "PASS" or data.get("disposal_review")
        ):
            return False
        try:
            with RunLock(parent):
                pass
        except OSError:
            return False
        parent = data.get("parent")
    return True


def _registry_snapshot():
    folder = tool_paths.state_path("tmp_runs")
    data, hashes = {}, {}
    for path in folder.glob("*.json"):
        raw = path.read_bytes()
        data[path.stem] = json.loads(raw)
        hashes[path.stem] = hashlib.sha256(raw).hexdigest()
    hold_path = tool_paths.state_path("tmp_holds.json")
    raw = hold_path.read_bytes() if hold_path.exists() else b"{}"
    holds = json.loads(raw)
    for name, hold in holds.items():
        path = relative_path(REPO / "tmp", name)
        actual = snapshot(path, REPO / "tmp")
        if not metadata_matches(hold["identity"], actual, directory=actual["kind"] == "dir"):
            raise RuntimeError("Protected object changed; re-adjudicate hold: " + name)
    digest = hashlib.sha256(
        json.dumps(
            {"runs": hashes, "holds": hashlib.sha256(raw).hexdigest()}, sort_keys=True
        ).encode()
    ).hexdigest()
    return data, hashes, holds, digest


def plan_cleanup(destination):
    with registry_lock():
        all_records, hashes, holds, registry_digest = _registry_snapshot()
        eligible, roots, frozen = {}, [], {}
        for run_id, record in all_records.items():
            if record["state"] == "cleaned":
                continue
            for item in record["items"]:
                path = Path(item["path"])
                if path.is_relative_to(REPO / "tmp"):
                    roots.append((path, run_id, item["role"]))
            if record["state"] != "disposable" or not _closed_ancestors(record, all_records):
                continue
            try:
                with RunLock(run_id):
                    raw = _meta(run_id).read_bytes()
                    if hashlib.sha256(raw).hexdigest() != hashes[run_id]:
                        raise RuntimeError("Run metadata changed outside the authorization lock")
                    record = json.loads(raw)
                    raw_snapshot = Path(record["snapshot"]).read_bytes()
                    if hashlib.sha256(raw_snapshot).hexdigest() != record["snapshot_sha256"]:
                        raise RuntimeError("Completion snapshot was modified")
                    frozen[run_id] = [json.loads(line) for line in raw_snapshot.splitlines()]
                    eligible[run_id] = record
            except OSError:
                continue
        held_paths = [relative_path(REPO / "tmp", name) for name in holds]

        def disposable(row):
            absolute = relative_path(REPO / "tmp", row["path"])
            if not os.path.lexists(absolute):
                return True
            if any(absolute == path or absolute.is_relative_to(path) for path in held_paths):
                return False
            matches = [r for r in roots if absolute == r[0] or absolute.is_relative_to(r[0])]
            if not matches or any(owner not in eligible for _, owner, _ in matches):
                return False
            if row["kind"] != "dir" and any(
                role != "generated" and not eligible[owner].get("archives", {}).get(str(absolute))
                for _, owner, role in matches
            ):
                return False
            return not row["kind"].startswith("reparse_") and (
                row["kind"] != "file" or row["nlink"] == 1
            )

        # Dispose whole runs, not their empty owner directory while a consumer holds
        # an externally registered output. Dependency exclusions propagate to shared owners.
        while True:
            blocked = [
                run_id for run_id in eligible if any(not disposable(row) for row in frozen[run_id])
            ]
            if not blocked:
                break
            for run_id in blocked:
                del eligible[run_id]

        rows = {}
        for run_id, record in eligible.items():
            for row in frozen[run_id]:
                absolute = relative_path(REPO / "tmp", row["path"])
                matches = [r for r in roots if absolute == r[0] or absolute.is_relative_to(r[0])]
                proofs = [
                    eligible[owner].get("archives", {}).get(str(absolute))
                    for _, owner, role in matches
                    if role != "generated"
                ]
                try:
                    actual = snapshot(absolute, REPO / "tmp")
                except FileNotFoundError:
                    continue
                if not metadata_matches(row, actual):
                    raise RuntimeError(
                        "Finished output changed; refusing cleanup plan: " + row["path"]
                    )
                row["disposition"] = (
                    "PRUNE_EMPTY_DIR" if row["kind"] == "dir" else "DELETE_GENERATED"
                )
                if row["kind"] == "file":
                    if not row.get("sha256"):
                        raise RuntimeError("Completion snapshot lacks a payload hash")
                    proof = next((p for p in proofs if p), None)
                    if proof:
                        if proof["sha256"] != row["sha256"]:
                            raise RuntimeError("Archive and completion payload differ")
                        row["proof"] = {"kind": "verified_run_archive", **proof}
                rows[row["path"]] = row
        for record in eligible.values():
            owned = Path(record["path"])
            for parent in (owned.parent, owned.parent.parent):
                if parent.is_relative_to(REPO / "tmp" / "runs") and os.path.lexists(parent):
                    row = snapshot(parent, REPO / "tmp")
                    if row["kind"] == "dir":
                        row["disposition"] = "PRUNE_EMPTY_DIR"
                        rows.setdefault(row["path"], row)
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("x", encoding="utf-8") as stream:
            for row in sorted(rows.values(), key=lambda r: r["path"]):
                stream.write(json.dumps(row) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        context = {
            "root": str(REPO / "tmp"),
            "manifest": str(destination),
            "registry_sha256": registry_digest,
            "runs": {
                run_id: {"sha256": hashes[run_id], "path": data["path"]}
                for run_id, data in eligible.items()
            },
            "manifest_sha256": _sha(destination),
        }
        for data in eligible.values():
            parent = data.get("parent")
            while parent:
                context["runs"].setdefault(
                    parent,
                    {
                        "sha256": hashes[parent],
                        "path": all_records[parent]["path"],
                        "parent_only": True,
                    },
                )
                parent = all_records[parent].get("parent")
        _json(destination.with_suffix(destination.suffix + ".owners.json"), context)
        return context


@contextmanager
def locked_plan(context):
    with registry_lock():
        _, _, _, digest = _registry_snapshot()
        if digest != context["registry_sha256"]:
            raise RuntimeError("Consumers, holds or adjudications changed after planning")
        held = []
        try:
            for run_id, approved in sorted(context["runs"].items()):
                lock = RunLock(run_id)
                lock.__enter__()
                held.append(lock)
                if _sha(_meta(run_id)) != approved["sha256"]:
                    raise RuntimeError("Run adjudication changed after planning")
            yield
        finally:
            for lock in reversed(held):
                lock.__exit__()


def mark_cleaned(context):
    for run_id, value in context["runs"].items():
        if value.get("parent_only"):
            continue
        data = load(run_id)
        data["state"] = "cleaned" if not Path(data["path"]).exists() else "retained"
        data["cleanup_complete"] = True
        _json(_meta(run_id), data)
