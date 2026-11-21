"""Consumer-visible deletion safety checks for the owned tmp lifecycle."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import tmp_lifecycle as lifecycle
import tmp_runs
from _tmp_fs import LockedFile, snapshot


@unittest.skipUnless(os.name == "nt", "Windows delete-by-handle contract")
class RemovalSafety(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mh_tmp_safety_")
        self.base = Path(self.temp.name).resolve()
        self.lease_root = mock.patch.object(lifecycle.hostlock, "LOCK_DIR", self.base / "leases")
        self.lease_root.start()
        self.root = self.base / "scratch"
        self.root.mkdir()

    def tearDown(self):
        self.lease_root.stop()
        self.temp.cleanup()

    def file(self, name, content=b"payload"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def test_readonly_file_is_removed_by_verified_handle(self):
        path = self.file("readonly.bin")
        path.chmod(stat.S_IREAD)
        row = snapshot(path, self.root)
        with LockedFile(path, deleting=True) as held:
            held.validate(row)
            held.remove()
        self.assertFalse(path.exists())

    def test_changed_payload_is_rejected_without_removal(self):
        path = self.file("changed.bin")
        before = snapshot(path, self.root)
        path.write_bytes(b"different length")
        with LockedFile(path, deleting=True) as held:
            with self.assertRaisesRegex(RuntimeError, "Changed"):
                held.validate(before)
        self.assertEqual(path.read_bytes(), b"different length")

    def test_hardlinks_require_separate_adjudication(self):
        path = self.file("shared.bin")
        other = self.base / "other.bin"
        os.link(path, other)
        row = snapshot(path, self.root)
        with LockedFile(path, deleting=True) as held:
            with self.assertRaisesRegex(RuntimeError, "Hardlinked"):
                held.validate(row)
        self.assertEqual(other.read_bytes(), b"payload")
        self.assertTrue(path.exists())

    def test_junction_target_survives_and_ancestor_alias_is_rejected(self):
        target = self.base / "outside"
        target.mkdir()
        survivor = target / "keep.bin"
        survivor.write_bytes(b"outside")
        link = self.root / "alias"
        subprocess.run(
            ["cmd", "/d", "/c", "mklink", "/J", str(link), str(target)],
            check=True,
            capture_output=True,
        )
        with self.assertRaisesRegex(RuntimeError, "Reparse ancestor"):
            with LockedFile(link / "keep.bin", deleting=True):
                pass
        row = snapshot(link, self.root)
        with LockedFile(link, deleting=True) as held:
            held.validate(row)
            held.remove()
        self.assertFalse(os.path.lexists(link))
        self.assertEqual(survivor.read_bytes(), b"outside")

    def test_open_writer_prevents_removal(self):
        path = self.file("active.bin")
        with path.open("ab"):
            self.assertEqual(snapshot(path, self.root)["logical"], len(b"payload"))
            with self.assertRaises(OSError), LockedFile(path, deleting=True):
                pass
        self.assertEqual(path.read_bytes(), b"payload")

    def test_preservation_and_keeper_dependencies_precede_removal(self):
        source = self.file("needed.bin", b"needed original")
        duplicate = self.file("duplicate.bin", b"needed original")
        operational = self.file("history.json", b"history")
        required = snapshot(source, self.root)
        required.update(
            disposition="PERSIST_BEFORE_DELETE",
            sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        )
        dup = snapshot(duplicate, self.root)
        dup.update(
            disposition="DELETE_GENERATED",
            sha256=required["sha256"],
            proof={
                "keeper": str(source),
                "sha256": required["sha256"],
                "requires_keeper_persistence": True,
            },
        )
        keep = snapshot(operational, self.root)
        keep.update(
            disposition="KEEP_IN_PLACE", sha256=hashlib.sha256(operational.read_bytes()).hexdigest()
        )
        manifest = self.base / "plan.jsonl"
        manifest.write_text(
            "".join(json.dumps(r) + "\n" for r in (required, dup, keep)), encoding="utf-8"
        )
        receipt = lifecycle.apply_manifest(
            manifest,
            lifecycle._sha(manifest),
            root=self.root,
            receipt_dir=self.base / "receipt",
            approval_note="isolated regression fixture",
        )
        self.assertEqual(receipt["removed_files"], 2)
        self.assertFalse(source.exists())
        self.assertFalse(duplicate.exists())
        self.assertEqual(
            (self.base / "receipt/retained/needed.bin").read_bytes(), b"needed original"
        )
        self.assertEqual(operational.read_bytes(), b"history")

    def test_changed_manifest_cannot_authorize_removal(self):
        path = self.file("protected.bin")
        row = snapshot(path, self.root)
        row["disposition"] = "DELETE_GENERATED"
        manifest = self.base / "plan.jsonl"
        manifest.write_text(json.dumps(row) + "\n", encoding="utf-8")
        approved = lifecycle._sha(manifest)
        manifest.write_text(json.dumps({**row, "logical": 0}) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "Approval digest"):
            lifecycle.apply_manifest(
                manifest,
                approved,
                root=self.root,
                receipt_dir=self.base / "receipt",
                approval_note="isolated regression fixture",
            )
        self.assertEqual(path.read_bytes(), b"payload")


@unittest.skipUnless(os.name == "nt", "Windows identity-bound cleanup contract")
class OwnershipSafety(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mh_tmp_owner_")
        self.base = Path(self.temp.name).resolve()
        self.lease_root = mock.patch.object(lifecycle.hostlock, "LOCK_DIR", self.base / "leases")
        self.lease_root.start()
        self.repo = self.base / "repo"
        (self.repo / "tmp").mkdir(parents=True)
        self.environment = mock.patch.dict(
            os.environ,
            {
                "MH_TOOL_STATE_ROOT": str(self.base / "state"),
                "MH_TOOL_INPUT_ROOT": str(self.base / "inputs"),
                "MH_EVIDENCE_ROOT": str(self.base / "evidence"),
            },
        )
        self.environment.start()
        self.parent = os.environ.pop("MH_TMP_PARENT_ID", None)
        self.roots = [
            mock.patch.object(module, "REPO", self.repo) for module in (tmp_runs, lifecycle)
        ]
        for patch in self.roots:
            patch.start()

    def tearDown(self):
        for patch in reversed(self.roots):
            patch.stop()
        if self.parent is not None:
            os.environ["MH_TMP_PARENT_ID"] = self.parent
        self.environment.stop()
        self.lease_root.stop()
        self.temp.cleanup()

    def completed(self, name="producer", *, evidence=False):
        with tmp_runs.Run(name) as run:
            path = run.path / "scratch" / "provenance.json"
            path.write_bytes(b"original")
            if evidence:
                run.register(path, role="evidence")
            run.finish("PASS")
        return run, path

    def plan(self):
        path = self.base / ("plan_" + __import__("uuid").uuid4().hex + ".jsonl")
        context = tmp_runs.plan_cleanup(path)
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        owners = path.with_suffix(".jsonl.owners.json")
        return context, rows, owners

    def test_successful_unconsumed_output_is_removed(self):
        run, path = self.completed()
        _, _, plan = self.plan()
        result = lifecycle.apply_cleanup(plan, lifecycle._sha(plan))
        self.assertFalse(path.exists())
        self.assertEqual(tmp_runs.load(run.id)["state"], "cleaned")
        self.assertEqual(result["removed_files"], 1)

    def test_failed_interrupted_and_human_parents_protect_passing_children(self):
        for verdict, human in (("FAIL", False), ("INTERRUPTED", False), ("PASS", True)):
            with self.subTest(verdict=verdict, human=human):
                with tmp_runs.Run("parent", human=human) as parent:
                    with tmp_runs.Run("child", adjudicator=True) as child:
                        path = child.path / "scratch" / "result"
                        path.write_bytes(b"must survive")
                        child.finish("PASS")
                    parent.finish(verdict)
                _, rows, _ = self.plan()
                self.assertNotIn(
                    str(path.relative_to(self.repo / "tmp")).replace("\\", "/"),
                    {r["path"] for r in rows},
                )
                self.assertEqual(path.read_bytes(), b"must survive")

    def test_broader_active_input_registration_protects_deeper_generated_output(self):
        _, path = self.completed()
        with tmp_runs.Run("consumer", manual=True) as consumer:
            consumer.register(path.parents[2], role="input")
            _, rows, _ = self.plan()
            self.assertFalse(any(r["kind"] == "file" for r in rows))
            self.assertEqual(path.read_bytes(), b"original")
            consumer.finish("FAIL")

    def test_new_consumer_invalidates_frozen_cleanup_authorization(self):
        _, path = self.completed()
        _, _, plan = self.plan()
        with tmp_runs.Run("consumer", manual=True) as consumer:
            consumer.register(path, role="input")
            with self.assertRaisesRegex(RuntimeError, "Consumers, holds or adjudications"):
                lifecycle.apply_cleanup(plan, lifecycle._sha(plan))
            self.assertEqual(path.read_bytes(), b"original")
            consumer.finish("FAIL")

    def test_same_metadata_replacement_payload_is_not_removed(self):
        _, path = self.completed()
        before = path.stat()
        path.write_bytes(b"replaced")
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
        _, _, plan = self.plan()
        with self.assertRaisesRegex(RuntimeError, "hash|SHA|payload"):
            lifecycle.apply_cleanup(plan, lifecycle._sha(plan))
        self.assertEqual(path.read_bytes(), b"replaced")

    def test_hold_excludes_subtree_and_late_hold_invalidates_plan(self):
        run, path = self.completed()
        _, _, plan = self.plan()
        lifecycle.protect(path, "unresolved incident")
        with self.assertRaisesRegex(RuntimeError, "Consumers, holds or adjudications"):
            lifecycle.apply_cleanup(plan, lifecycle._sha(plan))
        _, rows, _ = self.plan()
        self.assertNotIn(
            str(path.relative_to(self.repo / "tmp")).replace("\\", "/"), {r["path"] for r in rows}
        )
        self.assertEqual(path.read_bytes(), b"original")

    def test_evidence_keeper_survives_generated_original_removal(self):
        run, path = self.completed(evidence=True)
        keeper = Path(tmp_runs.load(run.id)["archives"][str(path)]["keeper"])
        self.assertEqual(keeper.read_bytes(), b"original")
        _, _, plan = self.plan()
        lifecycle.apply_cleanup(plan, lifecycle._sha(plan))
        self.assertFalse(path.exists())
        self.assertEqual(keeper.read_bytes(), b"original")

    def test_changed_adjudication_is_read_before_new_plan(self):
        run, path = self.completed()
        with tmp_runs.RunLock(run.id):
            changed = lifecycle._existing_run(run.id)
            changed.finish("FAIL")
        _, rows, _ = self.plan()
        self.assertFalse(any(r["kind"] == "file" for r in rows))
        self.assertEqual(path.read_bytes(), b"original")

    def test_external_generated_registration_cannot_delete_outside_tmp(self):
        outside = self.base / "external.txt"
        outside.write_bytes(b"outside deletion scope")
        with tmp_runs.Run("external-output") as run:
            inside = run.path / "scratch" / "inside.txt"
            inside.write_bytes(b"generated")
            run.register(outside)
            run.finish("PASS")
        _, _, plan = self.plan()
        lifecycle.apply_cleanup(plan, lifecycle._sha(plan))
        self.assertFalse(inside.exists())
        self.assertEqual(outside.read_bytes(), b"outside deletion scope")

    def test_mutating_held_directory_does_not_block_unrelated_cleanup(self):
        _, generated = self.completed()
        held = self.repo / "tmp" / "investigation"
        held.mkdir()
        lifecycle.protect(held, "open incident")
        needed = held / "later.txt"
        needed.write_bytes(b"new evidence after directory hold")
        _, _, plan = self.plan()
        lifecycle.apply_cleanup(plan, lifecycle._sha(plan))
        lifecycle._check_unmanaged()
        self.assertFalse(generated.exists())
        self.assertEqual(needed.read_bytes(), b"new evidence after directory hold")

    def test_reviewed_disposal_preserves_the_failed_verdict(self):
        with tmp_runs.Run("failed") as run:
            path = run.path / "scratch" / "failure.txt"
            path.write_bytes(b"adjudicated failure")
            run.finish("FAIL")
        with tmp_runs.RunLock(run.id):
            reviewed = lifecycle._existing_run(run.id)
            reviewed.data["disposal_review"] = {
                "reason": "isolated case adjudicated",
                "previous_verdict": "FAIL",
            }
            reviewed.finish("FAIL")
        _, _, plan = self.plan()
        lifecycle.apply_cleanup(plan, lifecycle._sha(plan))
        self.assertFalse(path.exists())
        self.assertEqual(tmp_runs.load(run.id)["verdict"], "FAIL")

    def test_consumed_external_output_keeps_owner_until_dependency_closes(self):
        producer, path = self.completed()
        with tmp_runs.Run("consumer", manual=True) as consumer:
            consumer.register(path, role="input")
            _, _, plan = self.plan()
            lifecycle.apply_cleanup(plan, lifecycle._sha(plan))
            self.assertTrue(producer.path.exists())
            self.assertEqual(path.read_bytes(), b"original")
            consumer.finish("PASS")
        keeper = Path(tmp_runs.load(consumer.id)["archives"][str(path)]["keeper"])
        _, _, plan = self.plan()
        lifecycle.apply_cleanup(plan, lifecycle._sha(plan))
        self.assertFalse(path.exists())
        self.assertEqual(keeper.read_bytes(), b"original")


def main():
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__))
    )
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
