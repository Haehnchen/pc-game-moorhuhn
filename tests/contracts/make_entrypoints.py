"""Check phony entry points and the failed-build launch guard."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    make = shutil.which("make")
    if make is None:
        raise RuntimeError("make is required for this entry-point check")
    repository = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="moorhuhn-make-") as temporary:
        root = Path(temporary)
        shutil.copy2(repository / "Makefile", root / "Makefile")
        (root / "build").touch()
        (root / "run").touch()
        cmake = root / "cmake-check"
        cmake.write_text(
            "#!/bin/sh\n"
            'printf "%s\\n" "$*" >> "$MOORHUHN_MAKE_LOG"\n'
            'if [ "$1" = "--build" ] && [ "$MOORHUHN_FAIL_BUILD" = "1" ]; then exit 11; fi\n'
        )
        cmake.chmod(0o755)
        log = root / "calls.log"
        environment = dict(os.environ, MOORHUHN_MAKE_LOG=str(log), MOORHUHN_FAIL_BUILD="0")

        def invoke(target, success):
            result = subprocess.run(
                [make, target, f"CMAKE={cmake}", "BUILD_DIR=output"],
                cwd=root,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )
            if (result.returncode == 0) != success:
                raise RuntimeError(result.stdout + result.stderr)

        invoke("build", True)
        invoke("build", True)
        calls = log.read_text().splitlines()
        if len(calls) != 4 or sum(call.startswith("-S ") for call in calls) != 2:
            raise RuntimeError("A named build file suppressed configure or build")
        invoke("run", True)
        calls = log.read_text().splitlines()
        if len(calls) != 7 or "--target run" not in calls[-1]:
            raise RuntimeError("A named run file suppressed build or launch")
        environment["MOORHUHN_FAIL_BUILD"] = "1"
        invoke("run", False)
        calls = log.read_text().splitlines()
        if len(calls) != 9 or "--target run" in calls[-1]:
            raise RuntimeError("Launch was attempted after a failed build")
    print("Phony build/run and failed-build launch guard passed")


if __name__ == "__main__":
    main()
