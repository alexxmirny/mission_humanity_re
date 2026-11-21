"""Persistent tool input/state/evidence locations, distinct from disposable run output."""

from __future__ import annotations

import os
from pathlib import Path

import machine_config as machine
from _tmp_fs import relative_path

REPO = Path(__file__).resolve().parent.parent


def _root(key, fallback, repo):
    configured = os.environ.get("MH_" + key, getattr(machine, key))
    root = Path(configured) if configured else Path(repo) / "workdir" / fallback
    root = Path(os.path.abspath(root))
    temporary = Path(os.path.abspath(Path(repo) / "tmp"))
    if root.is_relative_to(temporary) or root.resolve().is_relative_to(temporary.resolve()):
        raise ValueError(f"{key} must be outside disposable tmp")
    return root


def input_path(relative: str, *, repo=REPO) -> Path:
    return relative_path(_root("TOOL_INPUT_ROOT", "tool_inputs", repo), relative)


def state_path(relative: str, *, repo=REPO) -> Path:
    return relative_path(_root("TOOL_STATE_ROOT", "tool_state", repo), relative)


def evidence_path(relative: str) -> Path:
    return relative_path(_root("EVIDENCE_ROOT", "evidence", REPO), relative)


def resolve_input(spec, *, repo=REPO) -> str:
    value = str(spec)
    if value.startswith("input:"):
        return str(input_path(value[6:], repo=repo))
    if value.startswith("evidence:"):
        return str(evidence_path(value[9:]))
    return os.path.join(str(repo), value)
