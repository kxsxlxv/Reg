#!/usr/bin/env python3
"""Build a self-contained portable Windows bundle and immutable release assets.

Only files listed in the manifest are remotely managed. App configuration,
session logs, recordings and user data are NEVER included.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile

REPO = "kxsxlxv/Reg"
REQUIRED = ("reg_probe.exe", "reg_replay.exe", "reg_updater.exe")
MAX_FILES = 512
MAX_BYTES = 1_500_000_000


def collect(root: Path) -> dict[str, Path]:
    files: dict[str, Path] = {}
    launcher = root / "reg_launcher.exe"
    if not launcher.is_file():
        raise ValueError(f"Missing launcher: {launcher}")
    files["Launcher.exe"] = launcher
    for name in REQUIRED:
        path = root / name
        if not path.is_file():
            raise ValueError(f"Missing executable: {path}")
        files[name] = path
    dlls = sorted(root.glob("*.dll"))
    if not dlls:
        raise ValueError("Windows runtime DLLs have not been deployed")
    for path in dlls:
        files[path.name] = path
    for folder, suffix in (("fonts", ".ttf"), ("shaders", ".spv")):
        directory = root / folder
        assets = sorted(p for p in directory.rglob("*")
                        if p.is_file() and p.suffix.lower() == suffix)
        if not assets:
            raise ValueError(f"No {suffix} files in {directory}")
        for path in assets:
            relative = path.relative_to(root).as_posix()
            files[relative] = path
    if len(files) > MAX_FILES:
        raise ValueError("Too many files")
    return files


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def package(root: Path, output: Path, channel: str, tag: str) -> dict:
    if channel not in {"dev", "stable"}:
        raise ValueError("Invalid channel")
    if not re.fullmatch(r"(dev-[a-f0-9]{40}|v[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?)", tag):
        raise ValueError("Invalid release tag")
    if channel == "dev" and not tag.startswith("dev-"):
        raise ValueError("Dev channel needs a dev tag")
    if channel == "stable" and not tag.startswith("v"):
        raise ValueError("Stable channel needs a version tag")

    inputs = collect(root)
    if sum(x.stat().st_size for x in inputs.values()) > MAX_BYTES:
        raise ValueError("Package exceeds the size limit")

    if output.exists():
        shutil.rmtree(output)
    assets = output / "assets"
    portable = output / "portable"
    assets.mkdir(parents=True)
    portable.mkdir(parents=True)
    records = []
    for index, (relative, source) in enumerate(sorted(inputs.items())):
        checksum = sha256(source)
        filename = f"file-{index:04d}-{checksum[:20]}-{source.name}"
        destination = portable / Path(relative)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        shutil.copy2(source, assets / filename)
        records.append({"path": relative, "asset": filename,
                        "sha256": checksum, "size": source.stat().st_size})

    manifest = {"schema_version": 1, "repository": REPO,
                "platform": "windows-x64", "channel": channel,
                "tag": tag, "files": records}
    manifest_text = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    (output / "manifest.json").write_text(manifest_text, encoding="utf-8")
    (portable / "manifest.json").write_text(manifest_text, encoding="utf-8")
    archive = output / "portable.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=6) as z:
        for file in sorted(portable.rglob("*")):
            if file.is_file():
                z.write(file, arcname=f"VideoConsole/{file.relative_to(portable).as_posix()}")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--channel", choices=("dev", "stable"), required=True)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    manifest = package(args.root.resolve(), args.out.resolve(), args.channel, args.tag)
    print(f"Packed {len(manifest['files'])} files into {args.out}")


if __name__ == "__main__":
    main()
