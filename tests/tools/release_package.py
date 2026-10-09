#!/usr/bin/env python3
"""Release archive and dependency-resolution regressions without host SDKs."""

from __future__ import annotations

from contextlib import ExitStack
import os
import json
from pathlib import Path
import plistlib
import stat
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.packaging import package_release as release

release_inputs = release._release_inputs


class ReleaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.build = self.root / "build"
        self.build.mkdir()
        self.executable = self.write(self.build / "moorhuhn", b"native game")
        self.executable.chmod(0o755)
        self.write(self.build / "moorhuhn.exe", b"native game")
        self.notice = self.write(self.root / "SDL-LICENSE.txt", b"SDL license notice\n")
        self.context = ExitStack()
        self.addCleanup(self.context.close)
        self.context.enter_context(patch.dict(os.environ, {"PATH": ""}))
        self.context.enter_context(patch.object(release, "ROOT", self.root))
        self.resources = {
            "README.md": self.write(self.root / "README.md", b"Player instructions"),
            "LICENSES.txt": self.notice,
            "images.pak": self.write(self.build / "images.pak", b"packed images"),
            "audio.pak": self.write(self.build / "audio.pak", b"packed audio"),
        }
        self.context.enter_context(patch.object(release, "_release_inputs", return_value=(
            self.resources, {"version": "0.1.0", "source_commit": "a" * 40, "source_dirty": False},
        )))
        self.context.enter_context(patch.object(release.linux_package, "cache_values",
                                               return_value={"CMAKE_CXX_COMPILER": "g++", "CMAKE_STRIP": "strip"}))
        self.context.enter_context(patch.object(release.linux_package, "compiler_runtimes",
                                               return_value=({}, "GCC runtime notices\\n")))
        self.context.enter_context(patch.object(release.linux_package, "run", return_value=""))
        self.context.enter_context(patch.object(release.linux_package, "check_runtime", return_value={"2.39"}))
        self.context.enter_context(patch.object(release.linux_package, "inspect_elf", return_value={"2.39"}))
        self.context.enter_context(patch.object(release.platform, "machine", return_value="AMD64"))

    def write(self, path: Path, data: bytes = b"library") -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def stage(self) -> Path:
        stage = self.root / "stage"
        stage.mkdir()
        return stage

    def test_ldd_parses_full_closure_and_absolute_libraries(self) -> None:
        result = release._parse_ldd(
            "linux-vdso.so.1 (0x1234)\n"
            "libSDL3.so.0 => /sdk/lib/libSDL3.so.0 (0xabcd)\n"
            "libc.so.6 => /lib/libc.so.6 (0x1234)\n"
            "/lib64/ld-linux-x86-64.so.2 (0x1234)\n"
            "/opt/libcustom.so (0x1234)\n"
        )
        self.assertEqual(result["libSDL3.so.0"], Path("/sdk/lib/libSDL3.so.0"))
        self.assertEqual(result["libcustom.so"], Path("/opt/libcustom.so"))
        self.assertIn("ld-linux-x86-64.so.2", result)

    def test_ldd_rejects_missing_conflicting_and_unrecognized_dependencies(self) -> None:
        for output in (
            "libSDL3.so.0 => not found\n",
            "libsame.so => /one/libsame.so (0x1)\nlibsame.so => /two/libsame.so (0x2)\n",
            "unexpected dependency output\n",
        ):
            with self.subTest(output=output), self.assertRaises(release.PackagingError):
                release._parse_ldd(output)

    def test_linux_archives_preserve_sonames_and_executable_permissions(self) -> None:
        library = self.write(self.root / "runtime" / "libstdc++.so.6.0.33")
        deps = {"libstdc++.so.6": library, "libc.so.6": Path("/host/libc.so.6"),
                "libpthread.so.0": Path("/host/libpthread.so.0"),
                "ld-linux-x86-64.so.2": Path("/host/ld-linux-x86-64.so.2")}
        with patch.object(release.platform, "system", return_value="Linux"), \
                patch.object(release, "_run", return_value="mocked ldd") as run, \
                patch.object(release, "_parse_ldd", return_value=deps):
            outputs = release.package(self.build)
        self.assertEqual(run.call_count, 2)
        self.assertTrue(all(call.args[0] == ["ldd", str(self.executable)] for call in run.call_args_list))
        self.assertEqual([path.name for path in outputs], [
            "moorhuhn-Linux-x86_64.zip", "moorhuhn-Linux-x86_64-bundled.zip",
        ])
        with zipfile.ZipFile(outputs[0]) as archive:
            self.assertEqual(set(archive.namelist()), {"moorhuhn", "README.md", "LICENSES.txt", "images.pak", "audio.pak", "release-info.json"})
            self.assertEqual(archive.read("moorhuhn"), b"native game")
        with zipfile.ZipFile(outputs[1]) as archive:
            self.assertEqual(set(archive.namelist()), {
                "moorhuhn", "libstdc++.so.6", "README.md", "LICENSES.txt",
                "images.pak", "audio.pak", "release-info.json",
            })
            self.assertEqual(archive.read("libstdc++.so.6"), b"library")
            self.assertEqual(archive.read("images.pak"), b"packed images")
            self.assertEqual(archive.read("audio.pak"), b"packed audio")
            self.assertIn(b"GCC runtime notices", archive.read("LICENSES.txt"))
            self.assertEqual(json.loads(archive.read("release-info.json"))["source_commit"], "a" * 40)
            if os.name != "nt":
                for name in ("moorhuhn",):
                    self.assertTrue(archive.getinfo(name).external_attr >> 16 & stat.S_IXUSR)

    def test_linux_fails_when_reported_library_file_is_missing(self) -> None:
        with patch.object(release, "_run", return_value="libSDL3.so.0 => /missing/libSDL3.so.0 (0x1)"), \
                self.assertRaisesRegex(release.PackagingError, "missing library"):
            release._linux(self.executable, self.stage(), bundled=True)

    def test_host_glibc_filter_keeps_non_glibc_runtimes(self) -> None:
        for name in ("libc.so.6", "libm.so.6", "libmvec.so.1", "libnss_files.so.2",
                     "libresolv.so.2", "ld-linux-aarch64.so.1", "ld64.so.1"):
            self.assertIsNotNone(release._HOST_GLIBC.fullmatch(name), name)
        for name in ("libstdc++.so.6", "libgcc_s.so.1", "libSDL3.so.0", "libdecor-0.so.0"):
            self.assertIsNone(release._HOST_GLIBC.fullmatch(name), name)

    def test_windows_collects_recursive_imports_and_excludes_system_libraries(self) -> None:
        runtime = self.root / "ucrt64" / "bin"
        self.write(self.build / "SDL3.dll")
        self.write(self.build / "SDL3_mixer.dll")
        gcc = self.write(runtime / "LIBGCC_S_SEH-1.DLL")
        cpp = self.write(runtime / "libstdc++-6.dll")
        threads = self.write(runtime / "libwinpthread-1.dll")
        calls: list[Path] = []
        imports = {
            "moorhuhn.exe": ["KERNEL32.dll", "USER32.dll", "SDL3.dll", "SDL3_mixer.dll", "libstdc++-6.dll", "libgcc_s_seh-1.dll"],
            "sdl3.dll": ["USER32.dll", "api-ms-win-crt-runtime-l1-1-0.dll"],
            "sdl3_mixer.dll": ["SDL3.dll", "libgcc_s_seh-1.dll"],
            cpp.name.lower(): ["libgcc_s_seh-1.dll", "libwinpthread-1.dll", "api-ms-win-crt-runtime-l1-1-0.dll"],
            gcc.name.lower(): ["libwinpthread-1.dll", "ucrtbase.dll"],
            threads.name.lower(): ["libgcc_s_seh-1.dll", "ext-ms-win-ntuser-window-l1-1-0.dll"],
        }

        def objdump(command: list[str]) -> str:
            self.assertEqual(command[:2], ["objdump", "-p"])
            source = Path(command[2])
            calls.append(source)
            return "\n".join(f"    DLL Name: {name}" for name in imports[source.name.lower()])

        with patch.object(release.platform, "system", return_value="Windows"), \
                patch.dict(os.environ, {"PATH": str(runtime)}), \
                patch.object(release, "_run", side_effect=objdump):
            outputs = release.package(self.build)
        self.assertEqual(outputs[0].name, "moorhuhn-Windows-x86_64-bundled.zip")
        self.assertEqual(len(calls), 6)
        with zipfile.ZipFile(outputs[0]) as archive:
            self.assertEqual(set(archive.namelist()), {
                "moorhuhn.exe", "SDL3.dll", "SDL3_mixer.dll", "libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll",
                "README.md", "LICENSES.txt", "images.pak", "audio.pak", "release-info.json",
            })

    def test_windows_missing_transitive_import_is_fatal(self) -> None:
        runtime = self.write(self.build / "libstdc++-6.dll")

        def objdump(command: list[str]) -> str:
            name = "missing-runtime.dll" if Path(command[-1]) == runtime else runtime.name
            return f"DLL Name: {name}\n"

        with patch.object(release, "_run", side_effect=objdump), \
                self.assertRaisesRegex(release.PackagingError, "missing-runtime.dll"):
            release._windows(self.build / "moorhuhn.exe", self.stage())

    def test_windows_recognizes_installed_system_components(self) -> None:
        windows = self.root / "Windows"
        self.write(windows / "System32" / "SystemComponent.dll")
        with patch.dict(os.environ, {"SystemRoot": str(windows)}):
            self.assertTrue(release._windows_system_dll("SYSTEMCOMPONENT.DLL"))
            self.assertFalse(release._windows_system_dll("libstdc++-6.dll"))

    def test_import_parser_rejects_paths(self) -> None:
        for name in ("../bad.dll", "folder\\bad.dll", "", "not-a-dll"):
            with self.subTest(name=name), self.assertRaises(release.PackagingError):
                release._parse_imports(f"DLL Name: {name}\n")

    def test_macos_resolves_recursive_rpaths_and_signs_after_fixups(self) -> None:
        first = self.write(self.root / "dependencies" / "libFirst.dylib")
        second = self.write(first.parent / "libSecond.dylib")
        calls: list[list[str]] = []
        deps = {
            self.executable: ["@rpath/libFirst.dylib", "/usr/lib/libSystem.B.dylib"],
            first: ["@rpath/libFirst.dylib", "@loader_path/libSecond.dylib"],
            second: ["@rpath/libSecond.dylib", "@rpath/libFirst.dylib",
                     "/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa"],
        }

        def run(command: list[str]) -> str:
            calls.append(command)
            if command[0] != "otool":
                return ""
            source = Path(command[-1])
            if command[1] == "-l":
                return ("Load command 0\n cmd LC_RPATH\n cmdsize 64\n"
                        " path @executable_path/../dependencies (offset 12)\n"
                        if source == self.executable else "")
            if command[1] == "-D":
                return f"{source}:\n@rpath/{source.name}\n"
            return f"{source}:\n" + "".join(
                f"\t{name} (compatibility version 1.0.0, current version 1.0.0)\n"
                for name in deps[source]
            )

        with patch.object(release.platform, "system", return_value="Darwin"), \
                patch.object(release, "_run", side_effect=run):
            outputs = release.package(self.build, arch="aarch64")
        self.assertEqual(outputs[0].name, "moorhuhn-macOS-arm64-bundled.zip")
        with zipfile.ZipFile(outputs[0]) as archive:
            prefix = "Moorhuhn.app/Contents/"
            self.assertEqual(archive.read(prefix + "MacOS/Moorhuhn"), b"native game")
            self.assertEqual(archive.read(prefix + "Frameworks/libFirst.dylib"), b"library")
            self.assertEqual(archive.read(prefix + "Frameworks/libSecond.dylib"), b"library")
            info = plistlib.loads(archive.read(prefix + "Info.plist"))
            self.assertEqual(info["CFBundleExecutable"], "Moorhuhn")
            self.assertEqual(info["CFBundleShortVersionString"], "0.1.0")
            self.assertEqual(info["LSMinimumSystemVersion"], "13.0")
            self.assertEqual(archive.read(prefix + "Resources/images.pak"), b"packed images")
            self.assertEqual(archive.read(prefix + "Resources/audio.pak"), b"packed audio")
            self.assertEqual(json.loads(archive.read(prefix + "Resources/release-info.json"))["architecture"], "arm64")
        fixups = [index for index, cmd in enumerate(calls) if cmd[0] == "install_name_tool"]
        signatures = [index for index, cmd in enumerate(calls) if cmd[:2] == ["codesign", "--force"]]
        self.assertEqual(len(signatures), 3)
        self.assertLess(max(fixups), min(signatures))
        self.assertTrue(calls[signatures[-1]][-1].endswith("Moorhuhn.app"))
        self.assertEqual(calls[-1][:4], ["codesign", "--verify", "--deep", "--strict"])
        changes = [cmd[2:4] for cmd in calls if cmd[:2] == ["install_name_tool", "-change"]]
        self.assertIn(["@rpath/libFirst.dylib", "@executable_path/../Frameworks/libFirst.dylib"], changes)
        self.assertIn(["@loader_path/libSecond.dylib", "@loader_path/libSecond.dylib"], changes)
        self.assertIn(["@rpath/libFirst.dylib", "@loader_path/libFirst.dylib"], changes)
        self.assertTrue(any(cmd[:3] == ["install_name_tool", "-delete_rpath", "@executable_path/../dependencies"]
                            for cmd in calls))

    def test_macos_missing_rpath_dependency_is_fatal(self) -> None:
        with self.assertRaisesRegex(release.PackagingError, "cannot resolve macOS library"):
            release._macho_dependency("@rpath/missing.dylib", self.executable,
                                       self.executable, [self.root / "missing"])

    def test_macos_bare_rpath_tokens_resolve_to_their_binary_directories(self) -> None:
        library = self.write(self.root / "libraries" / "libSDL3.dylib")
        self.assertEqual(release._expand_macho_path("@loader_path", library, self.executable),
                         library.parent)
        self.assertEqual(release._expand_macho_path("@executable_path", library, self.executable),
                         self.executable.parent)
        self.assertIsNone(release._expand_macho_path("@loader_path_typo", library, self.executable))

    def test_otool_parser_rejects_unknown_dependency_lines(self) -> None:
        with self.assertRaisesRegex(release.PackagingError, "unrecognized otool"):
            release._parse_otool("program:\n\tunrecognized dependency\n")

    def test_source_guards_reject_a_debug_build_and_uncommitted_changes(self) -> None:
        with self.assertRaisesRegex(release.PackagingError, "Release distribution build"):
            release_inputs(self.build, allow_dirty=False)
        values = {"CMAKE_HOME_DIRECTORY": str(self.root), "CMAKE_BUILD_TYPE": "Release",
                  "MOORHUHN_DISTRIBUTION": "ON"}
        with patch.object(release.linux_package, "cache_values", return_value=values), \
                patch.object(release, "_run", side_effect=["a" * 40, " M src/app/main.cpp"]):
            with self.assertRaisesRegex(release.PackagingError, "source is dirty"):
                release_inputs(self.build, allow_dirty=False)

    def test_notices_and_packed_assets_are_verified_before_packaging(self) -> None:
        self.write(self.root / "CMakeLists.txt", b"project(moorhuhn VERSION 0.1.0 LANGUAGES C CXX)")
        self.write(self.root / "tools/packaging/README.md", b"Player instructions")
        self.write(self.root / "cmake/dependencies.lock.json", b'{"dependencies": {}}')
        self.write(self.build / "LICENSES.txt", b"locked notices")
        values = {"CMAKE_HOME_DIRECTORY": str(self.root), "CMAKE_BUILD_TYPE": "Release",
                  "MOORHUHN_DISTRIBUTION": "ON", "MOORHUHN_DEPENDENCY_CACHE": str(self.root)}
        with patch.object(release.linux_package, "cache_values", return_value=values), \
                patch.object(release, "_run", side_effect=["a" * 40, "", "a" * 40, " M Makefile"]), \
                patch.object(release.linux_package, "dependency_notices", return_value="locked notices"), \
                patch.object(release.linux_package, "checked_packages", return_value={}) as check:
            resources, metadata = release_inputs(self.build, allow_dirty=False)
            self.assertFalse(metadata["source_dirty"])
            self.assertEqual(metadata["version"], "0.1.0")
            self.assertEqual(resources["images.pak"], self.build / "images.pak")
            _, metadata = release_inputs(self.build, allow_dirty=True)
            self.assertTrue(metadata["source_dirty"])
            self.assertEqual(check.call_count, 2)
        with patch.object(release.linux_package, "cache_values", return_value=values), \
                patch.object(release, "_run", side_effect=["a" * 40, ""]), \
                patch.object(release.linux_package, "dependency_notices", return_value="wrong notices"), \
                self.assertRaisesRegex(release.PackagingError, "notices differ"):
            release_inputs(self.build, allow_dirty=False)

    def test_output_directory_override_contains_only_completed_archives(self) -> None:
        destination = self.root / "artifacts"
        with patch.object(release.platform, "system", return_value="Linux"), \
                patch.object(release, "_run", return_value="statically linked\n"):
            outputs = release.package(self.build, destination)
        self.assertEqual(set(os.listdir(destination)), {output.name for output in outputs})
        self.assertTrue(all(output.is_file() and output.parent.samefile(destination)
                            for output in outputs))
        self.assertFalse((self.build / "release").exists())

    def test_failed_zip_write_keeps_the_existing_archive_and_cleans_temporary_file(self) -> None:
        stage = self.stage()
        self.write(stage / "moorhuhn")
        output = self.write(self.root / "release.zip", b"previous archive")
        with patch.object(release.zipfile.ZipFile, "write", side_effect=OSError("disk full")), \
                self.assertRaisesRegex(OSError, "disk full"):
            release._zip_tree(stage, output)
        self.assertEqual(output.read_bytes(), b"previous archive")
        self.assertFalse(output.with_suffix(".zip.tmp").exists())

    def test_architecture_cannot_escape_output_directory(self) -> None:
        with self.assertRaisesRegex(release.PackagingError, "unsupported release architecture"):
            release.package(self.build, arch="../../outside")

    def test_command_failure_is_not_silently_ignored(self) -> None:
        failed = subprocess.CompletedProcess(["codesign"], 1, "", "invalid signature")
        with patch.object(release.subprocess, "run", return_value=failed), \
                self.assertRaisesRegex(release.PackagingError, "invalid signature"):
            release._run(["codesign", "--verify", "app"])


if __name__ == "__main__":
    unittest.main()
