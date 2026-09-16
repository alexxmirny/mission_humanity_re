"""Order-normalized semantic diff of two parsed configs.

Two configs are *canon-equal* when they would produce the same runtime
``dynamic::data``. Normalization (see :func:`canonicalize`):

- Objects within a class stay in file order (index-semantic — ``ReadObjects``
  fills arrays in file order and saves persist those indices), so lists are
  compared element-wise, not reordered.
- Field order inside a record is irrelevant (records are dicts); slotted arrays
  (int-keyed dicts) and planet-bank sets are normalized to sorted lists.
- Floats are already float32-quantized at parse time (``double(float32(text))``),
  so scalar equality is exact.
"""
from __future__ import annotations


def _norm(x):
    if isinstance(x, dict):
        if x and all(isinstance(k, int) for k in x):        # slotted array
            return [[k, _norm(x[k])] for k in sorted(x)]
        if not x:                                            # empty slotted array
            return []
        return {k: _norm(v) for k, v in x.items()}          # record / mapping
    if isinstance(x, set):
        return sorted(x)
    if isinstance(x, (list, tuple)):
        return [_norm(e) for e in x]
    return x


def canonicalize(model) -> dict:
    # The flat sections (DEFINE/DEF_BANK/ICON/TEXT) are name-keyed lookups — the
    # game resolves them by name, never by file position — so they compare
    # order-INsensitively (sorted). This lets a source file co-locate an icon/
    # define with the object that uses it without a spurious reordering diff.
    # Objects within a class stay order-sensitive (index = save-persisted).
    return {
        "defines": sorted(_norm(model.defines)),
        "def_banks": sorted(_norm(model.def_banks)),
        "icons": sorted(_norm(model.icons)),
        "texts": sorted(_norm(model.texts)),
        "banks": _norm(model.banks),
        "globals": _norm(model.globals),
        "classes": {cls: _norm(recs) for cls, recs in model.classes.items()},
    }


def _diff(path, a, b, out):
    if type(a) is not type(b):
        out.append(f"{path}: type {type(a).__name__} != {type(b).__name__}")
        return
    if isinstance(a, dict):
        for k in sorted(set(a) | set(b), key=str):
            if k not in a:
                out.append(f"{path}/{k}: missing on left")
            elif k not in b:
                out.append(f"{path}/{k}: missing on right")
            else:
                _diff(f"{path}/{k}", a[k], b[k], out)
    elif isinstance(a, list):
        if len(a) != len(b):
            out.append(f"{path}: length {len(a)} != {len(b)}")
        for idx in range(min(len(a), len(b))):
            _diff(f"{path}[{idx}]", a[idx], b[idx], out)
    elif a != b:
        out.append(f"{path}: {a!r} != {b!r}")


def canon_diff(model_a, model_b):
    """Return a list of difference strings (empty iff canon-equal)."""
    out = []
    _diff("", canonicalize(model_a), canonicalize(model_b), out)
    return out
