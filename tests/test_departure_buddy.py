"""Unit tests for the parts three implementations have to agree on.

The firmware, the setup page and the Windows installer each carry the same
rules: how a TfL platform name becomes a stored direction token, how that token
is shortened for the board's 52px route column, which destination a prediction
shows, and which settings keys exist in which order. Those are the places a
change in one copy silently diverges from the others, and every bug this suite
covers is one that actually happened.

    python tests/test_departure_buddy.py          # everything
    python tests/test_departure_buddy.py -v       # per-test names

Needs no hardware and no network: the TfL-facing functions are exercised
against recorded shapes, not live calls. Node is used, when present, to run the
page's JavaScript and compare it with the installer's Python.
"""
import json
import os
import re
import subprocess
import sys
import unittest
import importlib.util

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_installer():
    """Import installer.py without running its wizard."""
    spec = importlib.util.spec_from_file_location(
        "installer_under_test", os.path.join(ROOT, "installer", "installer.py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules["installer_under_test"] = module
    try:
        spec.loader.exec_module(module)
    except SystemExit:          # argparse in __main__ guards, but be safe
        pass
    return module


inst = load_installer()


class DirectionLabels(unittest.TestCase):
    """dirTag(): the direction shrunk to fit the row's 52px route column.

    Truncating to three characters used to mangle four of TfL's real names, so
    each case here is a name the network actually uses.
    """

    CASES = {
        "Northbound": "N/B", "Southbound": "S/B",
        "Eastbound": "E/B", "Westbound": "W/B",
        "EastBound": "E/B", "WestBound": "W/B",     # TfL's capital B, Piccadilly north
        "northbound": "N/B",                        # and its lower-case spelling
        "Inner Rail": "IN", "Outer Rail": "OUT",    # Hainault loop
        "Northbound Fast": "NBF",                   # Harrow-on-the-Hill, distinct platform
        "Southbound Fast": "SBF",
        "North / South": "N/S",                     # Chesham, one platform both ways
        "Westbound Platform 26": "W/B",             # Waterloo & City
        "Platform 2": "P2", "Platform 3": "P3", "Platform 2/3": "P2/3",
        "Some New Name": "Som",                     # unforeseen: first three characters
    }

    def test_every_real_name(self):
        for name, want in self.CASES.items():
            with self.subTest(name=name):
                self.assertEqual(inst.tube_dir_tag(name), want)

    def test_fast_platform_differs_from_stopping(self):
        # The bug: both rendered "Nor", on the one line where they are
        # genuinely different platforms.
        self.assertNotEqual(inst.tube_dir_tag("Northbound"),
                            inst.tube_dir_tag("Northbound Fast"))

    def test_labels_fit_the_column(self):
        # The column is 52px and is not clipped, so an over-long label would
        # run into the destination. Four characters is the practical ceiling.
        for name in self.CASES:
            self.assertLessEqual(len(inst.tube_dir_tag(name)), 4, name)


class PlatformTokens(unittest.TestCase):
    """platformToken(): what gets stored, and what the firmware matches on."""

    def test_compass_and_platform_shapes(self):
        self.assertEqual(inst.tube_platform_token("Northbound - Platform 3"), "Northbound")
        self.assertEqual(inst.tube_platform_token("Platform 2"), "Platform 2")
        self.assertEqual(inst.tube_platform_token("  Eastbound - Platform 1  "), "Eastbound")

    def test_empty_is_empty(self):
        self.assertEqual(inst.tube_platform_token(None), "")
        self.assertEqual(inst.tube_platform_token(""), "")


class Destinations(unittest.TestCase):
    """The destination shown for a prediction.

    TfL sometimes gives a placeholder instead of a place, and at Edgware Road
    those records carry no destinationName at all, which used to leave the row
    blank.
    """

    def test_towards_preferred(self):
        self.assertEqual(inst.tube_destination({"towards": "Brixton"}), "Brixton")

    def test_station_suffix_stripped(self):
        self.assertEqual(
            inst.tube_destination({"towards": "", "destinationName": "Seven Sisters Underground Station"}),
            "Seven Sisters")

    def test_placeholder_falls_back_to_destination_name(self):
        self.assertEqual(
            inst.tube_destination({"towards": "Check Front of Train",
                                   "destinationName": "Barking Underground Station"}),
            "Barking")

    def test_placeholder_with_nothing_else_still_says_something(self):
        # The Edgware Road case: a blank column told the reader less than this.
        self.assertEqual(inst.tube_destination({"towards": "Check Front of Train"}),
                         "Check front")


class LineLabels(unittest.TestCase):
    """The header's line name, shortened only where it must be."""

    def test_ampersand_lines_shortened(self):
        self.assertEqual(inst.tube_line_label("hammersmith-city"), "H&C")
        self.assertEqual(inst.tube_line_label("waterloo-city"), "W&C")

    def test_others_title_cased(self):
        self.assertEqual(inst.tube_line_label("victoria"), "Victoria")
        self.assertEqual(inst.tube_line_label("northern"), "Northern")


class SettingsKeys(unittest.TestCase):
    """The three key lists must agree, or settings vanish in transit.

    Weather and the clock once went missing this way: the installer filtered
    the stored mode against a list that had never gained them.
    """

    def web_keys(self):
        src = open(os.path.join(ROOT, "web", "js", "config.js"), encoding="utf-8").read()
        block = re.search(r"export const KEYS = \[(.*?)\];", src, re.S).group(1)
        return [k.strip().strip("'") for k in block.replace("\n", " ").split(",") if k.strip()]

    def firmware_keys(self):
        src = open(os.path.join(ROOT, "src", "config.cpp"), encoding="utf-8").read()
        return set(re.findall(r'k == "([a-z0-9]+)"', src))

    def test_page_and_installer_identical(self):
        self.assertEqual(self.web_keys(), list(inst.CONFIG_KEYS))

    def test_firmware_accepts_every_key_sent(self):
        self.assertEqual(sorted(set(self.web_keys()) - self.firmware_keys()), [])

    def test_no_firmware_key_goes_unsent(self):
        self.assertEqual(sorted(self.firmware_keys() - set(self.web_keys())), [])

    def test_tube_slots_present(self):
        for key in ("tube", "tubeline", "tubedir", "tubename",
                    "tubeline2", "tubedir2", "tubeline3", "tubedir3"):
            self.assertIn(key, self.web_keys(), key)


class TubeSlots(unittest.TestCase):
    """Three line+direction pairs at one station (#10).

    Checked against the firmware source rather than a copy of its rules, since
    the C++ cannot be executed here.
    """

    def config_h(self):
        return open(os.path.join(ROOT, "include", "config.h"), encoding="utf-8").read()

    def test_three_slots_and_no_more(self):
        self.assertIn("kTubeSlots = 3", self.config_h())

    def test_slot_needs_line_and_direction(self):
        src = self.config_h()
        self.assertIn("tube_line_at(slot).length()", src)
        self.assertIn("tube_dir_at(slot).length()", src)

    def test_nvs_keys_within_the_fifteen_character_limit(self):
        src = open(os.path.join(ROOT, "src", "config.cpp"), encoding="utf-8").read()
        for key in re.findall(r'prefs\.put\w+\("([^"]+)"', src):
            self.assertLessEqual(len(key), 15, f"NVS key {key!r} is too long")


class ResponseCap(unittest.TestCase):
    """The Tube buffer must hold what TfL really sends.

    Measured live: Euston on the Northern is 34 KB and Acton Town on the
    Piccadilly 26.7 KB. A 24 KB cap made both of those screens fail every poll.
    """

    MEASURED_WORST_BYTES = 34057

    def test_cap_covers_the_largest_measured_response(self):
        src = open(os.path.join(ROOT, "include", "app_config.h"), encoding="utf-8").read()
        cap = int(re.search(r"#define TUBE_MAX_RESPONSE\s+(\d+)", src).group(1))
        self.assertGreater(cap, self.MEASURED_WORST_BYTES,
                           "TUBE_MAX_RESPONSE is smaller than a real response")


class JavaScriptMirror(unittest.TestCase):
    """The page's JavaScript must resolve the same way as the installer."""

    def setUp(self):
        if not any(os.access(os.path.join(p, "node.exe" if os.name == "nt" else "node"), os.X_OK)
                   for p in os.environ.get("PATH", "").split(os.pathsep) if p):
            self.skipTest("node not on PATH")

    def test_direction_labels_match_python(self):
        names = list(DirectionLabels.CASES)
        script = (
            "(async () => {"
            "const api = await import('file:///" + ROOT.replace("\\", "/") + "/web/js/api.js');"
            "const names = " + json.dumps(names) + ";"
            "console.log(JSON.stringify(names.map((n) => api.tubeDirTag(n))));"
            "})()"
        )
        out = subprocess.run([("node"), "-e", script], capture_output=True, text=True, timeout=120)
        self.assertEqual(out.returncode, 0, out.stderr)
        got = json.loads(out.stdout.strip().splitlines()[-1])
        self.assertEqual(got, [inst.tube_dir_tag(n) for n in names])


if __name__ == "__main__":
    unittest.main(verbosity=2)
