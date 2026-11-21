"""No-follow, identity-bound Windows file operations for tmp_lifecycle.

Destructive operations require Windows delete-by-handle support. There is deliberately
no path-based unlink fallback: an identity check followed by unlink(path) has a race.
"""

from __future__ import annotations

import ctypes
import hashlib
import os
from ctypes import wintypes as W
from pathlib import Path

REPARSE = 0x400
DIRECTORY = 0x10


def relative_path(root: Path, relative: str) -> Path:
    if not relative or "\\" in relative or ":" in relative:
        raise ValueError(f"Invalid relative path: {relative!r}")
    parts = relative.split("/")
    if any(part in ("", ".", "..") for part in parts) or Path(relative).is_absolute():
        raise ValueError(f"Out-of-scope path: {relative!r}")
    return root / relative


def snapshot(path: Path, root: Path) -> dict:
    if os.name == "nt":
        # Python 3.11 lstat uses a 32-bit volume serial; FileIdInfo and Python 3.12
        # use the full 64-bit value. Plan and validate with the same native identity.
        # Planning must still observe active producers; only deletion takes the
        # restrictive LockedFile share mode below.
        handle = _k.CreateFileW(str(path), 0x80, 7, None, 3, 0x02200000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            return {"path": path.relative_to(root).as_posix(), **_native_info(handle)}
        finally:
            if not _k.CloseHandle(handle):
                raise ctypes.WinError(ctypes.get_last_error())
    s = path.lstat()  # DirEntry.stat on Windows does not supply reliable file identities.
    if not s.st_ino:
        raise RuntimeError(f"Missing file identity: {path}")
    attrs = s.st_file_attributes if os.name == "nt" else 0
    kind = (
        "reparse_dir"
        if attrs & REPARSE and attrs & DIRECTORY
        else "reparse_file"
        if attrs & REPARSE
        else "dir"
        if path.is_dir()
        else "file"
    )
    return {
        "path": path.relative_to(root).as_posix(),
        "kind": kind,
        "dev": str(s.st_dev),
        "file_id": str(s.st_ino),
        "logical": s.st_size if kind == "file" else 0,
        "mtime_ns": s.st_mtime_ns,
        "attributes": attrs,
        "nlink": s.st_nlink,
    }


def metadata_matches(row: dict, actual: dict, *, directory: bool = False) -> bool:
    keys = (
        ("kind", "dev", "file_id", "attributes")
        if directory
        else ("kind", "dev", "file_id", "logical", "mtime_ns", "attributes", "nlink")
    )
    return all(str(row[key]) == str(actual[key]) for key in keys)


if os.name == "nt":
    _k = ctypes.WinDLL("kernel32", use_last_error=True)
    _k.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, W.LPVOID, W.DWORD, W.DWORD, W.HANDLE]
    _k.CreateFileW.restype = W.HANDLE
    _k.CloseHandle.argtypes = [W.HANDLE]
    _k.CloseHandle.restype = W.BOOL
    _k.GetFinalPathNameByHandleW.argtypes = [W.HANDLE, W.LPWSTR, W.DWORD, W.DWORD]
    _k.GetFinalPathNameByHandleW.restype = W.DWORD
    _k.GetFileInformationByHandleEx.argtypes = [W.HANDLE, ctypes.c_int, W.LPVOID, W.DWORD]
    _k.GetFileInformationByHandleEx.restype = W.BOOL
    _k.ReadFile.argtypes = [W.HANDLE, W.LPVOID, W.DWORD, ctypes.POINTER(W.DWORD), W.LPVOID]
    _k.ReadFile.restype = W.BOOL
    _k.SetFilePointerEx.argtypes = [W.HANDLE, ctypes.c_longlong, W.LPVOID, W.DWORD]
    _k.SetFilePointerEx.restype = W.BOOL
    _k.SetFileInformationByHandle.argtypes = [W.HANDLE, ctypes.c_int, W.LPVOID, W.DWORD]
    _k.SetFileInformationByHandle.restype = W.BOOL

    class _Id(ctypes.Structure):
        _fields_ = [("volume", ctypes.c_ulonglong), ("identifier", ctypes.c_ubyte * 16)]

    class _Basic(ctypes.Structure):
        _fields_ = [
            ("created", ctypes.c_longlong),
            ("accessed", ctypes.c_longlong),
            ("modified", ctypes.c_longlong),
            ("changed", ctypes.c_longlong),
            ("attributes", W.DWORD),
        ]

    class _Standard(ctypes.Structure):
        _fields_ = [
            ("allocation", ctypes.c_longlong),
            ("size", ctypes.c_longlong),
            ("links", W.DWORD),
            ("pending", ctypes.c_ubyte),
            ("directory", ctypes.c_ubyte),
        ]


def _native_info(handle) -> dict:
    ident, basic, standard = _Id(), _Basic(), _Standard()
    for which, result in ((18, ident), (0, basic), (1, standard)):
        if not _k.GetFileInformationByHandleEx(
            handle, which, ctypes.byref(result), ctypes.sizeof(result)
        ):
            raise ctypes.WinError(ctypes.get_last_error())
    attrs = basic.attributes
    kind = (
        "reparse_dir"
        if attrs & REPARSE and attrs & DIRECTORY
        else "reparse_file"
        if attrs & REPARSE
        else "dir"
        if attrs & DIRECTORY
        else "file"
    )
    return {
        "kind": kind,
        "dev": str(ident.volume),
        "file_id": str(int.from_bytes(bytes(ident.identifier), "little")),
        "logical": standard.size if kind == "file" else 0,
        "mtime_ns": (basic.modified - 116444736000000000) * 100,
        "attributes": attrs,
        "nlink": standard.links,
    }


class LockedFile:
    """Pin the exact object, denying writes/renames/deletion until the handle closes."""

    def __init__(self, path: Path, *, deleting: bool = False):
        if os.name != "nt":
            raise OSError("Identity-bound cleanup requires Windows delete-by-handle support")
        self.path = Path(os.path.abspath(path))
        self.handle = None
        handle = _k.CreateFileW(
            str(self.path), 0x80000000 | (0x10000 if deleting else 0), 1, None, 3, 0x02200000, None
        )
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        self.handle = handle
        try:
            self._check_final_path()
        except BaseException:
            self.close()
            raise

    def _check_final_path(self):
        text = ctypes.create_unicode_buffer(32768)
        n = _k.GetFinalPathNameByHandleW(self.handle, text, len(text), 0)
        if not n or n >= len(text):
            raise ctypes.WinError(ctypes.get_last_error())
        final = text.value.removeprefix("\\\\?\\")
        if os.path.normcase(final) != os.path.normcase(str(self.path)):
            raise RuntimeError(f"Reparse ancestor or alias rejected: {self.path}")

    def info(self) -> dict:
        return _native_info(self.handle)

    def validate(self, row: dict, *, directory: bool = False):
        actual = self.info()
        if not metadata_matches(row, actual, directory=directory):
            raise RuntimeError(f"Changed file identity/metadata: {row['path']}")
        if actual["kind"] == "file" and actual["nlink"] != 1:
            raise RuntimeError(f"Hardlinked file requires separate adjudication: {row['path']}")
        return actual

    def chunks(self):
        if self.info()["kind"] != "file":
            raise RuntimeError("Refuse to read a directory or reparse point")
        if not _k.SetFilePointerEx(self.handle, 0, None, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        buffer = (ctypes.c_ubyte * (1024 * 1024))()
        view = memoryview(buffer).cast("B")
        count = W.DWORD()
        while True:
            if not _k.ReadFile(self.handle, buffer, len(buffer), ctypes.byref(count), None):
                raise ctypes.WinError(ctypes.get_last_error())
            if not count.value:
                break
            yield view[: count.value]

    def digest(self):
        value = hashlib.sha256()
        for block in self.chunks():
            value.update(block)
        return value.hexdigest()

    def remove(self):
        self._check_final_path()
        # DELETE | IGNORE_READONLY_ATTRIBUTE. Never opt into POSIX pathname semantics.
        flags = W.DWORD(0x11)
        if not _k.SetFileInformationByHandle(
            self.handle, 21, ctypes.byref(flags), ctypes.sizeof(flags)
        ):
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        if self.handle is not None:
            handle, self.handle = self.handle, None
            if not _k.CloseHandle(handle):
                raise ctypes.WinError(ctypes.get_last_error())

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def process_identity(pid: int):
    """Creation identity, not PID alone; None means dead/inaccessible, never a live proof."""
    if os.name != "nt":
        try:
            text = Path(f"/proc/{pid}/stat").read_text()
            return text[text.rfind(")") + 2 :].split()[19]
        except (OSError, IndexError):
            return None
    _k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    _k.OpenProcess.restype = W.HANDLE
    _k.GetProcessTimes.argtypes = [
        W.HANDLE,
        ctypes.POINTER(W.FILETIME),
        ctypes.POINTER(W.FILETIME),
        ctypes.POINTER(W.FILETIME),
        ctypes.POINTER(W.FILETIME),
    ]
    _k.GetProcessTimes.restype = W.BOOL
    handle = _k.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        created, exited, kernel, user = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
        if not _k.GetProcessTimes(
            handle,
            ctypes.byref(created),
            ctypes.byref(exited),
            ctypes.byref(kernel),
            ctypes.byref(user),
        ):
            return None
        return str((created.dwHighDateTime << 32) | created.dwLowDateTime)
    finally:
        _k.CloseHandle(handle)
