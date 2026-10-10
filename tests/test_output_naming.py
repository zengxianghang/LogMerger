"""CLI regression tests for automatic LogMerger output naming.

Build the executable first, then run:
  LOGMERGER_EXE=/path/to/merge_aux_into_input python -m unittest discover -s tests -v
On Windows, set LOGMERGER_EXE to the corresponding .exe path.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXE = ROOT / ("merge_aux_into_input.exe" if os.name == "nt" else "merge_aux_into_input")
EXE = Path(os.environ.get("LOGMERGER_EXE", str(DEFAULT_EXE))).resolve()


@unittest.skipUnless(EXE.is_file(), "Build LogMerger first or set LOGMERGER_EXE")
class OutputNamingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_merger(self, *args):
        result = subprocess.run([str(EXE), *(str(arg) for arg in args)],
                                capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))

    def test_two_inputs_use_combined_name(self):
        source = self.root / "input.log"
        aux = self.root / "aux.log"
        original = b"non-GNSS input line\r\n"
        source.write_bytes(original)
        aux.write_bytes(b"")
        self.run_merger(source, aux)
        self.assertEqual((self.root / "input-aux.log").read_bytes(), original)
        self.assertEqual(source.read_bytes(), original)

    def test_different_dirs_multi_dot_names_and_input_extension(self):
        input_dir = self.root / "input files"
        aux_dir = self.root / "aux files"
        input_dir.mkdir()
        aux_dir.mkdir()
        source = input_dir / "range.2026.txt"
        aux = aux_dir / "nav.data.log"
        source.write_bytes(b"test\n")
        aux.write_bytes(b"")
        self.run_merger(source, aux)
        self.assertEqual((input_dir / "range.2026-nav.data.txt").read_bytes(),
                         b"test\n")
        self.assertFalse((aux_dir / "range.2026-nav.data.txt").exists())

    def test_explicit_output_still_wins(self):
        source = self.root / "input.log"
        aux = self.root / "aux.log"
        output = self.root / "user-output.log"
        source.write_bytes(b"original\n")
        aux.write_bytes(b"")
        self.run_merger(source, aux, output)
        self.assertEqual(output.read_bytes(), b"original\n")
        self.assertFalse((self.root / "input-aux.log").exists())

    def test_explicit_output_and_tolerance_still_work(self):
        source = self.root / "input.log"
        aux = self.root / "aux.log"
        output = self.root / "custom.log"
        source.write_bytes(b"original\n")
        aux.write_bytes(b"")
        self.run_merger(source, aux, output, 100)
        self.assertEqual(output.read_bytes(), b"original\n")


if __name__ == "__main__":
    unittest.main()
