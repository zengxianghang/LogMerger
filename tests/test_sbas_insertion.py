"""Regression tests for SBAS1A–SBAS26A insertion using NovAtel long ASCII headers.

Run after building the executable:
  LOGMERGER_EXE=/path/to/merge_aux_into_input python -m unittest discover -s tests -v
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXE = ROOT / ("merge_aux_into_input.exe" if os.name == "nt" else "merge_aux_into_input")
EXE = Path(os.environ.get("LOGMERGER_EXE", str(DEFAULT_EXE))).resolve()


def crc32_novatel(data):
    """NovAtel long ASCII CRC32 starts at zero (not zlib's standard seed)."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc


def record(name, sow, week=2430, payload="1,2"):
    message = f"{name},COM1,0,0.0,FINE,{week},{sow},0,0,0;{payload}".encode("ascii")
    return b"#" + message + f"*{crc32_novatel(message):08X}".encode("ascii") + b"\r\n"


@unittest.skipUnless(EXE.is_file(), "Build LogMerger first or set LOGMERGER_EXE")
class SbasInsertionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def merge(self, source, auxiliary, tolerance_us=None):
        source_file = self.root / "input.log"
        aux_file = self.root / "aux.log"
        output_file = self.root / "out.log"
        source_file.write_bytes(source)
        aux_file.write_bytes(auxiliary)
        args = [str(EXE), str(source_file), str(aux_file), str(output_file)]
        if tolerance_us is not None:
            args.append(str(tolerance_us))
        proc = subprocess.run(args, capture_output=True, check=False)
        return proc, output_file

    def assert_merged(self, source, auxiliary, expected, tolerance_us=None):
        proc, output_file = self.merge(source, auxiliary, tolerance_us)
        self.assertEqual(proc.returncode, 0, proc.stderr.decode(errors="replace"))
        self.assertEqual(output_file.read_bytes(), expected)
        return proc.stderr.decode(errors="replace")

    def test_all_26_types_are_inserted_before_first_range(self):
        sbas = b"".join(record(f"SBAS{mt}A", "123455.900") for mt in range(1, 27))
        first = record("RANGEA", "123456.000")
        second = record("RANGEA", "123457.000")
        stderr = self.assert_merged(first + second, sbas,
                                    sbas + first + second)
        self.assertIn("SBAS1A-SBAS26A", stderr)
        self.assertIn("26", stderr)

    def test_mixed_eph_inspva_sbas_and_no_duplicate_insertion(self):
        eph = record("GPSEPHEMA", "100.000")
        sbas18 = record("SBAS18A", "100.000")
        sbas26 = record("SBAS26A", "100.500")
        inspva = record("INSPVAA", "101.000")
        sbas1 = record("SBAS1A", "101.000")
        auxiliary = eph + sbas18 + sbas26 + inspva + sbas1
        r100 = record("RANGEA", "100.000")
        r101 = record("RANGEA", "101.000")
        r102 = record("RANGEA", "102.000")
        self.assert_merged(r100 + r101 + r102, auxiliary,
                           eph + sbas18 + r100 + sbas26 + inspva + sbas1 + r101 + r102)

    def test_invalid_names_and_crc_are_ignored(self):
        invalid_names = ("SBAS0A", "SBAS27A", "SBAS01A", "SBAS1",
                         "SBAS1AA", "SBAS18B", "SBAS2Aa", "SBAS100A")
        invalid_crc = record("SBAS18A", "100.000").replace(b";1,2", b";9,2")
        auxiliary = (b"".join(record(name, "100.000") for name in invalid_names)
                     + invalid_crc)
        accepted = record("SBAS18A", "100.000")
        target = record("RANGEA", "100.000")
        stderr = self.assert_merged(target, auxiliary + accepted, accepted + target)
        self.assertIn("ignored other aux lines", stderr)

    def test_first_crc_valid_target_is_required(self):
        valid_target = record("RANGEA", "101.000")
        invalid_target = record("RANGEA", "100.000").replace(b";1,2", b";9,2")
        sbas = record("SBAS25A", "99.000")
        self.assert_merged(invalid_target + valid_target, sbas,
                           invalid_target + sbas + valid_target)

    def test_cross_gps_week_and_unmatched_later_message(self):
        old_sbas = record("SBAS1A", "604799.800", week=2430)
        newer_sbas = record("SBAS26A", "0.050", week=2431)
        after_last = record("SBAS25A", "1.000", week=2431)
        r1 = record("RANGEA", "604799.900", week=2430)
        r2 = record("RANGEA", "0.100", week=2431)
        stderr = self.assert_merged(r1 + r2, old_sbas + newer_sbas + after_last,
                                    old_sbas + r1 + newer_sbas + r2)
        self.assertIn("unmatched EPH/ION/SBAS after", stderr)

    def test_future_within_tolerance_matches_first_target(self):
        sbas = record("SBAS2A", "100.000050")
        r100 = record("RANGEA", "100.000000")
        r101 = record("RANGEA", "101.000000")
        self.assert_merged(r100 + r101, sbas, sbas + r100 + r101, tolerance_us=100)

    def test_insertion_before_non_range_targets_remains_supported(self):
        sbas = record("SBAS26A", "100.000")
        bestpos = record("BESTPOSA", "101.000")
        range_line = record("RANGEA", "102.000")
        self.assert_merged(bestpos + range_line, sbas, sbas + bestpos + range_line)


if __name__ == "__main__":
    unittest.main()
