"""Check native harness selection and isolation without launching processes."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
pil = types.ModuleType("PIL")
pil.Image = Mock()
with patch.dict(sys.modules, {"PIL": pil}):
    spec = importlib.util.spec_from_file_location("round_native", HERE / "round_native.py")
    native = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(native)


class SelectionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="moorhuhn-native-unit-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        for name in ("moorhuhn", "images.pak", "audio.pak", "manifest.txt"):
            (self.root / name).touch()
        self.output = self.root / "output"
        self.arguments = ["--build-dir", str(self.root), "--manifest", str(self.root / "manifest.txt"),
                          "--output-dir", str(self.output)]

    def dispatch(self, scenario="flow", failure=None):
        display = Mock()
        display.process.pid = 12345
        display.__enter__ = Mock(return_value=display)
        display.__exit__ = Mock(return_value=False)
        refusal = Mock()
        refusal.wait.return_value = 2

        def spawn(command, **kwargs):
            self.assertEqual(command[0], sys.executable)
            kwargs["stdout"].write("Private display already exists\n")
            kwargs["stdout"].flush()
            return refusal

        def play(*args):
            self.environment = args[3]
            if failure:
                raise failure
            names = ["title.png"] if scenario == "startup" else ["title.png", "credits.png"]
            if scenario == "flow":
                names.append("round-restart.png")
            for name in names:
                (args[4] / name).write_bytes(b"capture")
            return {"rounds": 0 if scenario == "startup" else 1, "screenshots": names}

        with contextlib.ExitStack() as stack:
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(patch.object(native.os.path, "lexists", return_value=False))
            stack.enter_context(patch.object(native, "OwnedDisplay", return_value=display))
            stack.enter_context(patch.object(native.subprocess, "Popen", side_effect=spawn))
            stop = stack.enter_context(patch.object(native, "stop_group"))
            selected = stack.enter_context(patch.object(native, "play", side_effect=play))
            credits = stack.enter_context(patch.object(native, "validate_credits", return_value={}))
            if failure:
                with self.assertRaises(RuntimeError):
                    native.main(self.arguments + ["--scenario", scenario])
            else:
                native.main(self.arguments + ["--scenario", scenario])
        selected.assert_called_once()
        self.assertEqual(selected.call_args.args[-1], scenario)
        stop.assert_called_once_with(refusal)
        display.__exit__.assert_called_once()
        self.assertFalse(Path(self.environment["XDG_RUNTIME_DIR"]).exists())
        return credits

    def test_each_scenario(self):
        for scenario in ("startup", "input", "flow"):
            with self.subTest(scenario=scenario):
                credits = self.dispatch(scenario)
                summary = json.loads((self.output / "summary.json").read_text())
                self.assertEqual(summary["scenario"], scenario)
                self.assertEqual("round-restart.png" in summary["screenshots"], scenario == "flow")
                self.assertEqual(credits.call_count, int(scenario != "startup"))

    def test_failure_cleans_display_and_temporary_data(self):
        self.dispatch(failure=RuntimeError("scenario failed"))
        self.assertFalse((self.output / "summary.json").exists())

    def test_parser_rejects_unsafe_or_removed_options(self):
        for arguments in (["--display", ":99"], ["--display", ":139.0"], ["--scenario", "canonical"]):
            with (self.subTest(arguments=arguments), patch.object(native, "OwnedDisplay") as display,
                  contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error):
                native.main(arguments)
            self.assertEqual(error.exception.code, 2)
            display.assert_not_called()

    def test_occupied_display_refused_before_setup(self):
        with (patch.object(native.os.path, "lexists", return_value=True),
              patch.object(native, "OwnedDisplay") as display,
              contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error):
            native.main(self.arguments)
        self.assertEqual(error.exception.code, 2)
        display.assert_not_called()



if __name__ == "__main__":
    unittest.main()
