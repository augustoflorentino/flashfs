"""pytest plugin: record what fwtriage's JFFS2, cramfs and UBI readers make of every image its
tests feed them. Each input region becomes <sha>.img, and what Python built from it <sha>.txt,
the oracle test_parity.c holds flashfs to (M1).

    cd ~/opensources/fwtriage
    FLASHFS_PARITY_OUT=<dir> PYTHONPATH=<this dir> python -m pytest -q -p flashfs_capture \\
        tests/unit/formats tests/corpus
"""

import hashlib
import os
import pathlib

import pytest

from fwtriage.formats import cramfs, jffs2, ubi
from fwtriage.model import EntryKind

OUT = pathlib.Path(os.environ["FLASHFS_PARITY_OUT"])
KINDS = {
    EntryKind.FILE: "file",
    EntryKind.DIRECTORY: "dir",
    EntryKind.LINK: "link",
    EntryKind.DEVICE: "device",
    EntryKind.OTHER: "other",
}

def _region_bytes(view, region) -> bytes:
    end = len(view) if region.size is None else min(len(view), region.offset + region.size)
    return bytes(view[region.offset : end])

def _filesystem_lines(filesystem) -> list[str]:
    lines = []
    for path in sorted(filesystem.entries):
        entry = filesystem.entries[path]
        line = f"{KINDS[entry.kind]} {entry.mode:o} {entry.uid} {entry.gid} {path.encode().hex()}"
        if entry.kind is EntryKind.FILE:
            line += f" {len(entry.data)} {hashlib.sha256(entry.data).hexdigest()}"
        elif entry.kind is EntryKind.LINK:
            line += f" {entry.target.encode().hex()}"
        lines.append(line)
    return lines

def _stream_lines(streams) -> list[str]:
    return [f"volume {s.name.encode().hex()} {len(s.data)} {hashlib.sha256(s.data).hexdigest()}" for s in streams]

def _limits(budget) -> str:
    """What was left of the budget when the reader started: the parity test hands flashfs the same."""
    limits = budget.limits
    left = (limits.total_bytes - budget.spent_bytes, limits.entry_bytes, limits.entries - budget.spent_entries)
    return "limits {} {} {}".format(*left)

def _record(fmt: str, data: bytes, limits: str, lines: list[str]) -> None:
    name = hashlib.sha256(data).hexdigest()[:16]
    (OUT / f"{name}.img").write_bytes(data)
    (OUT / f"{name}.txt").write_text("\n".join([f"format {fmt}", limits, *lines]) + "\n")

def _wrap(cls, fmt: str) -> None:
    original = cls.unpack

    def unpack(self, view, region, budget):
        data = _region_bytes(view, region)
        limits = _limits(budget)
        try:
            result = original(self, view, region, budget)
        except Exception as error:
            _record(fmt, data, limits, [f"error {type(error).__name__}"])
            raise
        if result.filesystem is not None:
            _record(fmt, data, limits, _filesystem_lines(result.filesystem))
        else:
            _record(fmt, data, limits, _stream_lines(result.streams))
        return result

    cls.unpack = unpack

@pytest.hookimpl(tryfirst=True)
def pytest_configure(config) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    _wrap(jffs2.Jffs2, "jffs2")
    _wrap(cramfs.Cramfs, "cramfs")
    _wrap(ubi.Ubi, "ubi")
