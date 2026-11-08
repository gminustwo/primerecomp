#!/usr/bin/env python3
"""Manifest contracts and optional runtime smoke tests; never require game content or real keys."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("recomplauncher", ROOT / "tools/recomplauncher.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
SCHEMA = EXE = PARSER = None


class ManifestTests(unittest.TestCase):
    def test_both_platform_manifests(self):
        for platform, program in [("windows", "primerecomp.exe"), ("linux", "Renamed game.AppImage")]:
            with self.subTest(platform=platform):
                m = generator.manifest(platform, program)
                self.assertEqual(m["repo"], "gminustwo/primerecomp")
                self.assertEqual(m["start"]["program"], [program])
                self.assertEqual(m["prepare"]["program"], [program])
                self.assertIn("--keys={input:keys}", m["prepare"]["args"])
                self.assertGreaterEqual(m["prepare"]["timeout"], 1800)
                if SCHEMA:
                    import jsonschema
                    jsonschema.Draft7Validator(json.loads(SCHEMA.read_text())).validate(m)

    def test_keys_are_only_required_for_packages(self):
        m = generator.manifest("windows", "primerecomp.exe")
        base, keys = m["inputs"]
        self.assertNotIn("requires", base["formats"][0])
        self.assertEqual(base["formats"][1]["requires"], [keys["id"]])
        self.assertEqual(base["formats"][1]["extensions"], ["nsp", "xci"])
        self.assertEqual(keys["shared"], "switch.prod-keys")
        self.assertNotIn("destination", keys)
        self.assertFalse(any(i.get("shared") == "switch.title-keys" for i in m["inputs"]))

    def test_saves_never_cover_keys_or_decryption_metadata(self):
        for platform in ("windows", "linux"):
            m = generator.manifest(platform, "game.AppImage")
            root = "{install}" if platform == "windows" else "{local}/primerecomp"
            self.assertEqual(m["saves"], [root + "/save"])
            self.assertEqual(m["config"][0]["file"][0]["path"], root + "/primerecomp.ini")
            self.assertIn("--data-root=" + root, m["prepare"]["args"])
            self.assertIn("--data-root=" + root, m["start"]["args"])

    def test_options_match_real_ini_keys(self):
        text = (ROOT / "runtime/settings.c").read_text()
        runtime_keys = set(re.findall(r'put\(o, "([a-z_][a-z_0-9]*) = ', text))
        # Some are written in the same format string after a newline.
        runtime_keys.update(re.findall(r'\\n([a-z_][a-z_0-9]*) = ', text))
        m = generator.manifest("windows", "primerecomp.exe")
        ids = [s["id"] for s in m["settings"]]
        self.assertEqual(len(ids), len(set(ids)))
        for s in m["settings"]:
            if s.get("in") == "args": continue
            self.assertIn(s["id"], runtime_keys)
            if "slider" in s:
                self.assertLess(s["slider"]["min"], s["slider"]["max"])
                self.assertGreater(s["slider"]["step"], 0)
        self.assertNotIn("romfs.src", json.dumps(m))

    def test_generator_writes_utf8_json(self):
        with tempfile.TemporaryDirectory(prefix="prime manifest ") as d:
            output = Path(d) / "recomplauncher.json"
            subprocess.run([os.sys.executable, str(ROOT / "tools/recomplauncher.py"), "--platform", "linux",
                            "--program", "PrimeRecomp-test.AppImage", "--output", str(output)], check=True)
            self.assertEqual(json.loads(output.read_text())["format"], 1)


@unittest.skipUnless(EXE, "pass --exe to run native headless smoke tests")
class RuntimeTests(unittest.TestCase):
    def run_cli(self, *args):
        with tempfile.TemporaryDirectory(prefix="prime setup α ") as d:
            root = Path(d)
            data = root / "data"
            data.mkdir()
            save = data / "save/save_0"
            save.mkdir(parents=True)
            sentinel = save / "keep.bin"
            sentinel.write_bytes(b"existing synthetic save")
            env = os.environ.copy()
            env.update(LARECOMP_DATA_ROOT=str(data), LARECOMP_SAVE_DIR=str(data / "save"),
                       APPIMAGE_EXTRACT_AND_RUN="1", SDL_VIDEODRIVER="invalid-driver", SDL_AUDIODRIVER="invalid-driver")
            for key in ("DISPLAY", "WAYLAND_DISPLAY", "LARECOMP_LAUNCHER_SETUP", "LARECOMP_LAUNCHER_EXTRACT"):
                env.pop(key, None)
            source = root / "game files β"
            source.mkdir()
            synthetic_keys = None
            if "bad-package" in args:
                source = root / "synthetic.nsp"
                source.write_bytes(bytes(512))
                synthetic_keys = root / "synthetic.keys"
                synthetic_keys.write_text("not-a-key-value\n")
                args = ("--keys=" + str(synthetic_keys),)
            if "wrong-version" in args:
                (source / "exefs").mkdir()
                (source / "romfs/Worlds").mkdir(parents=True)
                (source / "exefs/main").write_bytes(bytes(128))
                (source / "exefs/main.npdm").write_bytes(b"fixture")
                args = ()
            r = subprocess.run([str(EXE), "--prepare", "--game=" + str(source), *args], cwd=root,
                               env=env, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(sentinel.read_bytes(), b"existing synthetic save")
            self.assertNotIn("video", r.stderr.lower())  # setup never attempts a window
            progress = [line for line in r.stdout.splitlines() if line.startswith("progress ")]
            self.assertTrue(progress, r.stderr)
            values = [float(line.split(" ", 2)[1]) for line in progress]
            self.assertEqual(values, sorted(values))
            self.assertTrue(all(0 <= x < 1 for x in values))
            log = data / "primerecomp-setup.log"
            self.assertTrue(log.is_file())
            self.assertFalse((data / ".primerecomp-setup-source").exists())
            if synthetic_keys:
                self.assertEqual(synthetic_keys.read_text(), "not-a-key-value\n")
                self.assertEqual(source.read_bytes(), bytes(512))
                self.assertNotIn("not-a-key-value", log.read_text() + r.stdout)
            return log.read_text()

    def test_empty_folder_fails_without_window_or_save_changes(self):
        self.assertIn("No game files", self.run_cli())

    def test_wrong_game_version_fails_without_window(self):
        self.assertIn("Wrong version", self.run_cli("wrong-version"))

    def test_bad_package_and_explicit_keys_preserve_originals(self):
        self.assertIn("keys", self.run_cli("bad-package").lower())

    def test_help_without_display(self):
        r = subprocess.run([str(EXE), "--help"], capture_output=True, text=True, timeout=15)
        self.assertEqual(r.returncode, 0)
        self.assertIn("--title-keys", r.stdout)


class ParserTests(unittest.TestCase):
    def test_native_parser(self):
        if not PARSER: self.skipTest("pass --parser to run compiled parser tests")
        with tempfile.TemporaryDirectory(prefix="prime args α ") as d:
            subprocess.run([str(PARSER), d], check=True)


if __name__ == "__main__":
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("--schema", type=Path)
    args.add_argument("--exe", type=lambda s: Path(s).resolve())
    args.add_argument("--parser", type=lambda s: Path(s).resolve())
    o, remaining = args.parse_known_args()
    SCHEMA, EXE, PARSER = o.schema, o.exe, o.parser
    # unittest decorators are evaluated before CLI arguments are parsed.
    if EXE:
        RuntimeTests.__unittest_skip__ = False
    unittest.main(argv=[os.sys.argv[0], *remaining])
