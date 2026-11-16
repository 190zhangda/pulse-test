#!/usr/bin/env python3
"""Stage the installed Pulse runtime as a portable folder and ZIP archive.

The portable edition is the same verified payload the setup installs, without
the setup bootstrapper: the root holds only the stable launcher and
dialog-current.json, and every application/dialog component plus the licenses
live under DialogBridge/<manifest sha256>/ exactly as an installation does.
Settings and caches keep using the user application-data directory, so the
folder can be moved or deleted at any time.

Only the Python standard library is used.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pack_installer


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_archive(folder: Path, archive: Path) -> None:
    # Deterministic entries: sorted paths, fixed timestamps, stored checksums.
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as package:
        for path in sorted(p for p in folder.rglob("*") if p.is_file()):
            relative = path.relative_to(folder).as_posix()
            info = zipfile.ZipInfo(relative, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with path.open("rb") as stream:
                package.writestr(info, stream.read())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=".", help="repository root")
    ap.add_argument("--build", default="build", help="build directory (relative to root or absolute)")
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True, help="new portable folder (must not exist)")
    ap.add_argument("--zip", dest="archive", help="optional ZIP archive (must not exist)")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    build = Path(args.build)
    actual_build = build if build.is_absolute() else root / build
    out = Path(args.out)
    if not out.is_absolute():
        out = (root / out).resolve()
    if out.exists():
        raise SystemExit(f"portable folder already exists: {out}")
    if args.archive and Path(args.archive).exists():
        raise SystemExit(f"archive already exists: {args.archive}")

    items = pack_installer.collect(root, actual_build)
    sys.path.insert(0, str(root / "tools" / "pack_dialog_experiment"))
    import pack as dialog_pack

    machine = dialog_pack.PE(actual_build / "pulse.exe").machine
    arch = next((name for name, value in dialog_pack.MACHINES.items() if value == machine), None)
    if arch is None:
        raise SystemExit("unsupported Pulse architecture for the dialog bundle")

    out.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".pulse-portable-", dir=out.parent))
    try:
        folder = staging / out.name
        dialog_pack.package_installed(actual_build, folder, args.version, arch, root, items)
        dialog_pack.verify_installed(folder)
        for name in ("pulse.exe", "dialog-current.json", "DialogBridge"):
            if not (folder / name).exists():
                raise SystemExit(f"portable output is missing {name}")
        os.replace(folder, out)
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    files = sorted(p for p in out.rglob("*") if p.is_file())
    total = sum(p.stat().st_size for p in files)
    print(f"portable folder: {out} ({len(files)} files, {total} bytes)")
    for path in files:
        print(f"  {sha256_file(path)}  {path.relative_to(out).as_posix()}")
    if args.archive:
        archive = Path(args.archive)
        if not archive.is_absolute():
            archive = (root / archive).resolve()
        archive.parent.mkdir(parents=True, exist_ok=True)
        write_archive(out, archive)
        print(f"archive: {archive} ({archive.stat().st_size} bytes, sha256 {sha256_file(archive)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
