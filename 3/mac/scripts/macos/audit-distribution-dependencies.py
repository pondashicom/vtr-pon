#!/usr/bin/env python3
"""Inventory a macOS development bundle and runtime candidates without changing it.

This records load commands, installed metadata and notice evidence. It neither
selects a release payload nor verifies that distribution conditions are met.
"""
from __future__ import annotations

import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
MAGICS = {bytes.fromhex(x) for x in (
    "cffaedfe", "cefaedfe", "feedface", "feedfacf", "cafebabe", "bebafeca",
    "cafebabf", "bfbafeca")}
SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/", "/Library/Apple/System/Library/")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def macho(path: Path) -> bool:
    try:
        with path.open("rb") as stream:
            return stream.read(4) in MAGICS
    except (OSError, IsADirectoryError):
        return False


def run(*args: str) -> str:
    result = subprocess.run(args, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed for {args[-1]}: {result.stderr.strip()}")
    return result.stdout


def metadata(path: Path) -> dict:
    commands = run("/usr/bin/otool", "-arch", "arm64", "-l", str(path))
    links = run("/usr/bin/otool", "-arch", "arm64", "-L", str(path))
    rpaths, identity, minimum_os = [], None, None
    for block in re.split(r"Load command \d+\n", commands)[1:]:
        if re.search(r"\bcmd LC_RPATH\b", block):
            value = re.search(r"\n\s*path (.+) \(offset", block)
            if value:
                rpaths.append(value[1])
        if re.search(r"\bcmd LC_ID_DYLIB\b", block):
            value = re.search(r"\n\s*name (.+) \(offset", block)
            if value:
                identity = value[1]
        if re.search(r"\bcmd LC_BUILD_VERSION\b", block):
            value = re.search(r"\n\s*minos ([\d.]+)", block)
            if value:
                minimum_os = value[1]
        if re.search(r"\bcmd LC_VERSION_MIN_MACOSX\b", block):
            value = re.search(r"\n\s*version ([\d.]+)", block)
            if value:
                minimum_os = value[1]
    dependencies = []
    for line in links.splitlines():
        found = re.match(r"\s+(.+) \(compatibility version ([^,]+), current version ([^)]+)\)", line)
        if found and found[1] != identity:
            dependencies.append({"install_name": found[1], "current_version": found[3]})
    return {"rpaths": rpaths, "install_id": identity, "minimum_macos": minimum_os,
            "architectures": run("/usr/bin/lipo", "-archs", str(path)).strip().split(),
            "dependencies": dependencies, "raw_load_commands": commands, "raw_links": links}


def recipe_for(path: Path, prefix: Path) -> tuple[str, Path] | None:
    try:
        parts = path.resolve().relative_to(prefix / "Cellar").parts
        return parts[0], prefix / "Cellar" / parts[0] / parts[1]
    except ValueError:
        return None


def package_metadata(name: str, cellar: Path, output: Path) -> dict:
    recipe = cellar / ".brew" / f"{name}.rb"
    receipt = cellar / "INSTALL_RECEIPT.json"
    result = {"name": name, "cellar": str(cellar), "version": cellar.name,
              "license_selection_verified": False}
    if recipe.is_file():
        text = recipe.read_text()
        lines = text.splitlines()
        license_lines = []
        for i, line in enumerate(lines):
            if re.match(r"\s*license\b", line):
                license_lines.append(line.strip())
                balance = sum(line.count(c) for c in "[{(") - sum(line.count(c) for c in "]})")
                j = i + 1
                while balance > 0 and j < len(lines):
                    line = lines[j]
                    license_lines.append(line.strip())
                    balance += sum(line.count(c) for c in "[{(") - sum(line.count(c) for c in "]})")
                    j += 1
                break
        downloads = []
        for i, line in enumerate(lines):
            match = re.match(r'\s*url "(https?://[^\"]+)"', line)
            if match:
                digest = None
                for following in lines[i + 1:i + 5]:
                    found = re.match(r'\s*sha256 "([0-9a-f]{64})"', following)
                    if found:
                        digest = found[1]
                        break
                    if re.match(r"\s*(url|resource|livecheck|bottle|head)\b", following):
                        break
                downloads.append({"url": match[1], "declared_sha256": digest,
                                  "download_verified": False})
        result.update({"formula_sha256": sha256(recipe),
                       "formula_license_expression_raw": " ".join(license_lines),
                       "recipe_downloads": downloads})
        destination = output / "formulae" / (name + ".rb")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(text)
    if receipt.is_file():
        data = json.loads(receipt.read_text())
        result.update({"receipt_sha256": sha256(receipt), "used_options": data.get("used_options"),
                       "built_as_bottle": data.get("built_as_bottle"),
                       "poured_from_bottle": data.get("poured_from_bottle"),
                       "source_versions": data.get("source", {}).get("versions"),
                       "runtime_dependencies": data.get("runtime_dependencies", [])})
    return result


def expand(value: str, loader: Path, executable: Path) -> Path | None:
    if value.startswith("@loader_path"):
        return Path(str(loader.parent) + value[len("@loader_path"):])
    if value.startswith("@executable_path"):
        return Path(str(executable.parent) + value[len("@executable_path"):])
    return Path(value) if value.startswith("/") else None


def resolve(name: str, loader: Path, executable: Path, rpaths: tuple[str, ...]) -> Path | None:
    if name.startswith("@rpath/"):
        for rpath in rpaths:
            base = expand(rpath, loader, executable)
            if base is not None and (base / name[7:]).is_file():
                return (base / name[7:]).resolve()
        return None
    path = expand(name, loader, executable)
    return path.resolve() if path is not None and path.is_file() else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=ROOT / "build/macos-debug/vtr-pon.app")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--homebrew-prefix", type=Path, default=Path("/opt/homebrew"))
    parser.add_argument("--addon", type=Path, action="append", default=[], help="Additional addon Mach-O or .app, inspected separately")
    parser.add_argument("--gstreamer-metadata", type=Path, help="Optional plugin registry metadata captured separately; not release selection")
    options = parser.parse_args()
    app, output, prefix = options.app.resolve(), options.output.resolve(), options.homebrew_prefix.resolve()
    if not app.is_dir() or not (app / "Contents/MacOS/vtr-pon").is_file():
        parser.error("--app must be an existing VTR-PON .app")
    # Output must never mutate an input bundle or a Homebrew installation.
    if output == app or app in output.parents or output == prefix or prefix in output.parents:
        parser.error("--output must be outside the inspected app and Homebrew prefix")
    for addon in options.addon:
        value = addon.resolve()
        if output == value or value in output.parents:
            parser.error("--output must be outside inspected addons")
    output.mkdir(parents=True, exist_ok=True)
    raw_dir = output / "load-commands"
    raw_dir.mkdir(exist_ok=True)
    roots = []
    for path in sorted(app.rglob("*")):
        if path.is_file() and not path.is_symlink() and macho(path):
            roots.append(("app", path.resolve()))
    for category in ("platforms", "imageformats", "iconengines", "tls"):
        for path in sorted((prefix / "share/qt/plugins" / category).glob("*.dylib")):
            roots.append(("qt-runtime-candidate", path.resolve()))
    gst_dir = prefix / "lib/gstreamer-1.0"
    for path in sorted(gst_dir.rglob("*.dylib")):
        roots.append(("gst-installed-candidate", path.resolve()))
    scanners = sorted((prefix / "opt/gstreamer/libexec").rglob("gst-plugin-scanner"))
    roots.extend(("gst-scanner-candidate", path.resolve()) for path in scanners)
    for addon in options.addon:
        paths = addon.rglob("*") if addon.is_dir() else [addon]
        found = [p.resolve() for p in paths if p.is_file() and not p.is_symlink() and macho(p)]
        if not found:
            parser.error(f"no Mach-O found in --addon {addon}")
        roots.extend(("optional-addon:" + addon.name, path) for path in found)
    cache, nodes, packages, edges, errors = {}, {}, {}, [], []
    main_executable = app / "Contents/MacOS/vtr-pon"
    root_metadata = metadata(main_executable)
    queue = deque((path, group, path if group == "app" or group.startswith("optional-addon:") else main_executable,
                   tuple(root_metadata["rpaths"]) if group not in ("app",) else ()) for group, path in roots)
    missing_candidates = []
    visited = set()
    while queue:
        path, group, executable, inherited = queue.popleft()
        key = str(path)
        context = (key, group, str(executable))
        if context in visited:
            continue
        visited.add(context)
        if not path.is_file() and group.endswith("candidate"):
            missing_candidates.append({"path": key, "group": group})
            continue
        try:
            if key not in cache:
                cache[key] = metadata(path)
            info = cache[key]
            if key not in nodes:
                raw_key = hashlib.sha256(key.encode()).hexdigest()[:20]
                (raw_dir / (raw_key + ".txt")).write_text(info["raw_links"] + "\n" + info["raw_load_commands"])
                package = recipe_for(path, prefix)
                package_name = package[0] if package else None
                if package and package_name not in packages:
                    packages[package_name] = package_metadata(*package, output)
                nodes[key] = {"path": key, "sha256": sha256(path), "size": path.stat().st_size,
                              "architectures": info["architectures"], "minimum_macos": info["minimum_macos"],
                              "package": package_name, "groups": [], "evidence": "load-commands/" + raw_key + ".txt"}
            if group not in nodes[key]["groups"]:
                nodes[key]["groups"].append(group)
            # Convert loader-relative rpaths at the image where they are defined.
            local = tuple(str(expand(value, path, executable) or value) for value in info["rpaths"])
            rpaths = local + inherited
            for dependency in info["dependencies"]:
                name = dependency["install_name"]
                edge = {"from": key, "group": group, **dependency}
                if name.startswith(SYSTEM_PREFIXES):
                    edge["kind"] = "apple-system"
                else:
                    target = resolve(name, path, executable, rpaths)
                    edge.update({"kind": "resolved" if target else "unresolved", "to": str(target) if target else None})
                    if target:
                        queue.append((target, group, executable, rpaths))
                edges.append(edge)
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            errors.append({"path": key, "group": group, "error": str(error)})
    sboms = []
    for name, package in sorted(packages.items()):
        cellar = Path(package["cellar"])
        for path in sorted((cellar / "share/qt/sbom").glob("*.spdx")):
            text = path.read_text()
            package_rows = []
            for block in re.split(r"(?m)^PackageName: ", text)[1:]:
                fields = {}
                for label in ("PackageVersion", "PackageLicenseDeclared", "PackageLicenseConcluded", "PackageDownloadLocation"):
                    match = re.search(r"(?m)^" + label + r": (.+)", block)
                    fields[label] = match[1] if match else None
                package_rows.append({"name": block.splitlines()[0], **fields})
            dest = output / "sbom" / path.name
            dest.parent.mkdir(exist_ok=True)
            dest.write_text(text)
            sboms.append({"path": str(path), "sha256": sha256(path), "packages": package_rows,
                          "complete_third_party_coverage_verified": False})
    notice_root = app / "Contents/Resources/LICENSE"
    notices = [{"path": str(p.relative_to(notice_root)), "sha256": sha256(p), "size": p.stat().st_size}
               for p in sorted(notice_root.rglob("*")) if p.is_file()]
    plugin_data = json.loads(options.gstreamer_metadata.read_text()) if options.gstreamer_metadata else None
    unresolved = [edge for edge in edges if edge["kind"] == "unresolved"]
    summary = {"app_macho_roots": sum(g == "app" for g, _ in roots),
               "qt_plugin_candidates": sum(g == "qt-runtime-candidate" for g, _ in roots),
               "gst_plugin_candidates": sum(g == "gst-installed-candidate" for g, _ in roots),
               "scanner_candidates": sum(g == "gst-scanner-candidate" for g, _ in roots),
               "optional_addon_roots": sum(g.startswith("optional-addon:") for g, _ in roots),
               "unique_macho_files": len(nodes), "homebrew_packages": len(packages),
               "app_link_closure": sum("app" in x["groups"] for x in nodes.values()),
               "apple_system_install_names": len({e["install_name"] for e in edges if e["kind"] == "apple-system"}),
               "unresolved_edges": len(unresolved), "inspection_errors": len(errors), "missing_candidate_roots": len(missing_candidates),
               "bundled_notice_files": len(notices)}
    report = {"schema_version": 1, "purpose": "development dependency inventory; not a release manifest or license approval",
              "base_commit": run("git", "-C", str(ROOT), "rev-parse", "HEAD").strip(),
              "app": str(app), "summary": summary, "roots": [{"group": g, "path": str(p)} for g, p in roots],
              "binaries": sorted(nodes.values(), key=lambda x: x["path"]), "edges": edges,
              "packages": list(packages.values()), "qt_sboms": sboms, "bundled_notices": notices,
              "gstreamer_runtime_metadata": plugin_data, "unresolved": unresolved, "errors": errors, "missing_candidates": missing_candidates,
              "limitations": ["dlopen and static embedded code are not enumerated by Mach-O links alone",
                              "installed plugins and transitive packages are candidates, not release selections",
                              "system dependencies use the macOS dyld cache; they are not copied or recursively inspected",
                              "formula declarations and SBOMs do not replace per-file license/source review"]}
    (output / "inventory.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(summary, indent=2))
    return 1 if errors or unresolved else 0


if __name__ == "__main__":
    sys.exit(main())
