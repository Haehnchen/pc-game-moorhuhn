#!/usr/bin/env python3
"""Stage one local Linux runtime archive. Never build, launch, or upload."""

import argparse
import datetime
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import runpy
import subprocess
import sys
import tarfile
import tempfile


REPOSITORY = Path(__file__).resolve().parents[2]
REPORTS = REPOSITORY / "build/package-reports"
COMPONENT = "MoorhuhnRuntime"
SYSTEM_LIBRARIES = {"libc.so.6", "libm.so.6", "ld-linux-x86-64.so.2"}
BUNDLED_LIBRARIES = {"libSDL3.so.0", "libSDL3_mixer.so.0", "libstdc++.so.6", "libgcc_s.so.1"}


def run(arguments, cwd=None):
    environment = os.environ.copy()
    for key in tuple(environment):
        if key.startswith("LD_"):
            del environment[key]
    environment["LC_ALL"] = "C"
    return subprocess.check_output(arguments, cwd=cwd, env=environment, text=True).strip()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def cache_values(build):
    values = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        match = re.match(r"([^/#][^:]*):[^=]+=(.*)", line)
        if match:
            values[match[1]] = match[2]
    return values


def dependencies(executable, stage):
    output = run(["ldd", str(executable)])
    require("not found" not in output, "Unresolved ELF dependency")
    result = {}
    for line in output.splitlines():
        match = re.match(r"\s*(\S+) => (\/.+?) \(", line)
        if match:
            result[match[1]] = Path(match[2]).resolve()
        else:
            match = re.match(r"\s*(\/.+?) \(", line)
            if match:
                result[Path(match[1]).name] = Path(match[1]).resolve()
    for name, path in result.items():
        if name in BUNDLED_LIBRARIES:
            require(path == stage / name, f"Library does not resolve beside the executable: {name}")
        else:
            require(name in SYSTEM_LIBRARIES, f"Unexpected system runtime: {name}")
    return result


def check_runtime(stage, forbidden_paths, strip):
    glibc_versions = set()
    for name in ["moorhuhn", *sorted(BUNDLED_LIBRARIES)]:
        path = stage / name
        require(path.is_file() and not path.is_symlink(), f"Missing runtime file: {name}")
        if name != "moorhuhn":
            run([strip, "--strip-unneeded", str(path)])
        dynamic = run(["readelf", "-d", str(path)])
        search_paths = re.findall(r"\((?:RUNPATH|RPATH)\).*?\[(.*?)\]", dynamic)
        if name in {"libstdc++.so.6", "libgcc_s.so.1"}:
            require(not search_paths, f"Unexpected compiler runtime search path: {name}")
        else:
            require(search_paths == ["$ORIGIN"], f"Runtime must search its own folder: {name}")
        needed = set(re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic))
        require(needed <= SYSTEM_LIBRARIES | BUNDLED_LIBRARIES, f"Unexpected ELF dependencies: {name}")
        require(".symtab" not in run(["readelf", "-S", str(path)]), f"Unstripped runtime: {name}")
        if name not in {"libstdc++.so.6", "libgcc_s.so.1"}:
            dependencies(path, stage)
        glibc_versions.update(inspect_elf(path, forbidden_paths))
    return glibc_versions


def inspect_elf(path, forbidden_paths):
    data = path.read_bytes()
    require(data.startswith(b"\x7fELF"), f"Not an ELF file: {path.name}")
    header = run(["readelf", "-h", str(path)])
    require(
        "Advanced Micro Devices X86-64" in header and "ELF64" in header, f"Unsupported ELF architecture: {path.name}"
    )
    for value in forbidden_paths:
        require(value.encode() not in data, f"Embedded developer path: {path.name}")
    require(not re.search(rb"/(?:home|root|Users)/[^\x00\n\r]+", data), f"Embedded home path: {path.name}")
    require(
        not re.search(rb"(?:^|\x00)/[^\x00\n\r]+\.(?:c|cc|cpp|h|hpp)\x00", data),
        f"Embedded absolute source path: {path.name}",
    )
    versions = run(["readelf", "--version-info", str(path)])
    return set(re.findall(r"\bGLIBC_(\d+(?:\.\d+)+)\b", versions))


def payload_files(stage):
    result = {}
    for path in sorted(stage.rglob("*")):
        require(not path.is_symlink(), f"Package symlink: {path.relative_to(stage)}")
        if path.is_dir():
            continue
        require(path.is_file(), f"Non-file payload: {path.relative_to(stage)}")
        relative = path.relative_to(stage).as_posix()
        require(not any(character in relative for character in "\n\r\\"), "Unsafe filename")
        require(
            path.suffix.lower() not in {".exe", ".dat", ".cpp", ".hpp", ".cmake", ".py"},
            f"Forbidden package file: {relative}",
        )
        result[relative] = {"sha256": digest(path), "bytes": path.stat().st_size}
    return result


def archive_tree(stage, destination, epoch):
    with destination.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for path in [stage, *sorted(stage.rglob("*"))]:
                    info = archive.gettarinfo(str(path), arcname=path.relative_to(stage.parent).as_posix())
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mtime = epoch
                    info.mode = 0o755 if path.is_dir() or path.name == "moorhuhn" else 0o644
                    if path.is_file():
                        with path.open("rb") as stream:
                            archive.addfile(info, stream)
                    else:
                        archive.addfile(info)


def checked_packages(stage, root):
    packer = runpy.run_path(str(REPOSITORY / "tools/pack_assets.py"))
    groups = packer["collect_files"](root / "manifest.txt")
    expected_paths = {path for files in groups.values() for path, _ in files}
    # The Windows icon is embedded in the executable rather than a game asset package.
    source_paths = payload_files(root).keys() - {"moorhuhn.ico"}
    require(source_paths == expected_paths, "Source asset tree contains missing or unlisted files")
    result = {}
    for name, files in groups.items():
        filename = f"{name}.pak"
        data = packer["pack_files"](files)
        require((stage / filename).read_bytes() == data, f"Installed asset package differs: {filename}")
        result[filename] = {"files": len(files), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    return result


def dependency_notices(lock, cache):
    result = ""
    for name, record in sorted(lock.items()):
        source = cache / "sources" / f"{name}-{record['sha256']}" / record["archive_root"]
        notice = record["license_file"]
        require(digest(source / notice) == record["license_sha256"], f"License hash mismatch: {name}")
        content = (source / notice).read_text(encoding="utf-8")
        result += f"=== {name} {record['version']} / {notice} ===\n{content}\n"
    return result


def compiler_runtimes(compiler):
    metadata = {}
    notices = {}
    for name in ("libstdc++.so.6", "libgcc_s.so.1"):
        archive = Path(run([compiler, f"-print-file-name={name}"])).resolve()
        require(archive.is_file(), f"Missing compiler runtime: {name}")
        owners = run(["dpkg-query", "-S", str(archive)]).splitlines()
        owners = [line.removesuffix(f": {archive}") for line in owners if line.endswith(f": {archive}")]
        require(len(owners) == 1, f"Cannot identify compiler runtime owner: {name}")
        owner = owners[0]
        version = run(["dpkg-query", "-W", "-f=${Version}", owner])
        source = run(["dpkg-query", "-W", "-f=${source:Package} ${source:Version}", owner])
        metadata[name] = {
            "system_package": owner,
            "version": version,
            "source_package": source,
            "file_sha256": digest(archive),
        }
        if owner not in notices:
            notice = Path("/usr/share/doc") / owner.split(":", 1)[0] / "copyright"
            content = notice.read_bytes().decode("utf-8")
            require("GCC RUNTIME LIBRARY EXCEPTION" in content, "Missing GCC runtime exception notice")
            notices[owner] = f"=== {owner} {version} / copyright ===\n{content}\n"
    gpl = Path("/usr/share/common-licenses/GPL-3").read_text(encoding="utf-8")
    return metadata, "".join(notices.values()) + f"=== GPL-3 ===\n{gpl}\n"


def package(arguments):
    require(sys.platform == "linux" and platform.machine() == "x86_64", "This tool packages Linux x86_64 only")
    build = arguments.build_dir.resolve()
    output = arguments.output_dir.resolve()
    require(
        build != output and build not in output.parents and output not in build.parents,
        "Build and output directories must be separate",
    )
    source_commit = run(["git", "rev-parse", "HEAD"], REPOSITORY)
    requested_commit = run(["git", "rev-parse", f"{arguments.source_commit}^{{commit}}"], REPOSITORY)
    require(source_commit == requested_commit, "Source commit must be the current checkout")
    dirty = bool(run(["git", "status", "--porcelain", "--untracked-files=all"], REPOSITORY))
    require(not dirty or arguments.allow_dirty, "Source is dirty; commit first or use --allow-dirty for development")
    values = cache_values(build)
    require(values.get("CMAKE_HOME_DIRECTORY") == str(REPOSITORY), "Build belongs to another checkout")
    require(values.get("CMAKE_BUILD_TYPE") == "Release", "Package requires a Release build")
    require(values.get("MOORHUHN_DISTRIBUTION") == "ON", "Package requires MOORHUHN_DISTRIBUTION=ON")
    version_match = re.search(r"project\(moorhuhn VERSION (\d+\.\d+\.\d+)", (REPOSITORY / "CMakeLists.txt").read_text())
    require(version_match is not None, "Cannot read application version")
    version = version_match[1]
    epoch = int(run(["git", "show", "-s", "--format=%ct", source_commit], REPOSITORY))
    package_name = f"moorhuhn-{version}-linux-x86_64-{source_commit[:12]}" + ("-dev" if dirty else "")
    destination = output / f"{package_name}.tar.gz"
    report_path = REPORTS / f"{destination.name}.json"
    notices_path = REPORTS / f"{destination.name}.LICENSES.txt"
    asset_root = REPOSITORY / "assets"
    lock = json.loads((REPOSITORY / "cmake/dependencies.lock.json").read_text())["dependencies"]
    forbidden_paths = {str(REPOSITORY), str(build), str(Path.home())}
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".moorhuhn-stage-", dir=output) as temporary:
        stage = Path(temporary) / package_name
        run(["cmake", "--install", str(build), "--strip", "--prefix", str(stage), "--component", COMPONENT])
        glibc_versions = check_runtime(stage, forbidden_paths, values["CMAKE_STRIP"])
        asset_packages = checked_packages(stage, asset_root)
        licenses = stage / "LICENSES.txt"
        dependency_text = licenses.read_bytes().decode("utf-8")
        require(
            dependency_text == dependency_notices(lock, Path(values["MOORHUHN_DEPENDENCY_CACHE"])),
            "Installed dependency notices differ from locked sources",
        )
        runtime_metadata, runtime_notices = compiler_runtimes(values["CMAKE_CXX_COMPILER"])
        notices_text = dependency_text + runtime_notices
        licenses.unlink()
        files = payload_files(stage)
        expected_files = {"moorhuhn", "README.md", "images.pak", "audio.pak"} | BUNDLED_LIBRARIES
        require(files.keys() == expected_files, "Unexpected installed payload")
        for path in stage.rglob("*"):
            if path.is_file():
                data = path.read_bytes()
                for value in forbidden_paths:
                    require(value.encode() not in data, f"Embedded developer path: {path.relative_to(stage)}")
        require(glibc_versions, "No glibc requirement found")
        glibc_minimum = max(glibc_versions, key=lambda value: tuple(map(int, value.split("."))))
        metadata = {
            "schema": 4,
            "name": "moorhuhn",
            "version": version,
            "source_commit": source_commit,
            "source_dirty": dirty,
            "source_date_epoch": epoch,
            "source_date_utc": datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc).isoformat(),
            "platform": "linux",
            "architecture": "x86_64",
            "asset_manifest_sha256": digest(asset_root / "manifest.txt"),
            "asset_packages": asset_packages,
            "host": {
                "system": platform.system(),
                "release": platform.release(),
                "glibc": run(["getconf", "GNU_LIBC_VERSION"]),
                "compiler": run([values["CMAKE_CXX_COMPILER"], "--version"]).splitlines()[0],
            },
            "runtime": {
                "bundled": {name: files[name] for name in sorted(BUNDLED_LIBRARIES)},
                "static": [],
                "compiler": runtime_metadata,
                "header_only": [],
                "glibc_minimum": glibc_minimum,
                "system_libraries": sorted(SYSTEM_LIBRARIES),
                "native_backends": ["X11", "Wayland", "ALSA", "PulseAudio", "PipeWire"],
            },
            "dependencies": {
                name: {key: record[key] for key in ("name", "version", "revision", "license", "license_sha256")}
                for name, record in lock.items()
            },
            "files": files,
        }
        archive = Path(temporary) / destination.name
        archive_tree(stage, archive, epoch)
        archive_hash = digest(archive)
        metadata["archive"] = {"name": destination.name, "sha256": archive_hash, "bytes": archive.stat().st_size}
        REPORTS.mkdir(parents=True, exist_ok=True)
        notices_path.write_text(notices_text, encoding="utf-8", newline="\n")
        metadata["notices"] = {
            "name": notices_path.name,
            "sha256": digest(notices_path),
            "bytes": notices_path.stat().st_size,
        }
        report_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
        archive.replace(destination)
    print(
        json.dumps(
            {
                "archive": str(destination),
                "sha256": archive_hash,
                "metadata": str(report_path),
                "notices": str(notices_path),
                "source_commit": source_commit,
                "source_dirty": dirty,
                "files": len(files),
                "bytes": destination.stat().st_size,
                "glibc_minimum": glibc_minimum,
            },
            indent=2,
        )
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source-commit", required=True, help="Exact current source revision")
    parser.add_argument("--output-dir", type=Path, default=REPOSITORY / "dist")
    parser.add_argument("--allow-dirty", action="store_true", help="Mark an uncommitted development package")
    arguments = parser.parse_args()
    try:
        package(arguments)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Package failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
