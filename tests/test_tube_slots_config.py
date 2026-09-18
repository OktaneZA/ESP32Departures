"""What the setup page sends for a Tube station with one, two or three lines.

toDeviceConfig() is the last gate before settings reach a board, and the shape
that nearly shipped was a station with no direction: the panel left slot 1 blank
whenever a station offered more than one, so a config could name a station and
no platform. The firmware would store it and run no screen (TUBE-03), which
looks like a broken board rather than an unfinished form.

    python tests/test_tube_slots_config.py

Runs the page's own JavaScript through Node, so this tests the shipped module
rather than a restatement of its rules. Skips if Node is absent.
"""
import json
import os
import shutil
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG_JS = "file:///" + os.path.join(ROOT, "web", "js", "config.js").replace("\\", "/")


def device_config(overrides):
    """Run toDeviceConfig() over defaultConfig() plus `overrides`."""
    script = (
        "(async () => {"
        "const cfg = await import('" + CONFIG_JS + "');"
        "const ui = cfg.defaultConfig();"
        "Object.assign(ui, " + json.dumps(overrides) + ");"
        "console.log(JSON.stringify(cfg.toDeviceConfig(ui)));"
        "})()"
    )
    out = subprocess.run(["node", "-e", script], capture_output=True, text=True, timeout=120)
    if out.returncode != 0:
        raise AssertionError(out.stderr)
    return json.loads(out.stdout.strip().splitlines()[-1])


ACTON = {"services": ["tube"], "tube": "940GZZLUACT", "tubename": "Acton Town"}


@unittest.skipIf(shutil.which("node") is None, "node not on PATH")
class TubeSlotsConfig(unittest.TestCase):

    def test_one_line_sends_one_slot(self):
        got = device_config(dict(ACTON, tubeline="district", tubedir="Eastbound"))
        self.assertEqual(got["tube"], "940GZZLUACT")
        self.assertEqual((got["tubeline"], got["tubedir"]), ("district", "Eastbound"))
        self.assertEqual((got["tubeline2"], got["tubedir2"]), ("", ""))
        self.assertEqual((got["tubeline3"], got["tubedir3"]), ("", ""))
        self.assertIn("tube", got["mode"].split(","))

    def test_two_lines_share_the_station(self):
        got = device_config(dict(ACTON, tubeline="district", tubedir="Eastbound",
                                 tubeline2="piccadilly", tubedir2="Westbound"))
        # One station, two line+direction pairs: the #10 case.
        self.assertEqual(got["tube"], "940GZZLUACT")
        self.assertEqual((got["tubeline2"], got["tubedir2"]), ("piccadilly", "Westbound"))
        self.assertEqual((got["tubeline3"], got["tubedir3"]), ("", ""))

    def test_three_lines_all_sent(self):
        got = device_config(dict(ACTON, tubeline="district", tubedir="Eastbound",
                                 tubeline2="piccadilly", tubedir2="Westbound",
                                 tubeline3="piccadilly", tubedir3="Eastbound"))
        self.assertEqual((got["tubeline3"], got["tubedir3"]), ("piccadilly", "Eastbound"))

    def test_station_without_a_direction_enables_no_tube_screen(self):
        # The defect this file exists for: a station chosen, no direction picked.
        got = device_config(dict(ACTON, tubeline="district", tubedir=""))
        self.assertNotIn("tube", got["mode"].split(","),
                         "a station with no direction must not enable the Tube screen")
        self.assertEqual(got["tube"], "")

    def test_tube_off_clears_every_slot(self):
        got = device_config({"services": ["clock"], "tube": "940GZZLUACT",
                             "tubename": "Acton Town", "tubeline": "district",
                             "tubedir": "Eastbound", "tubeline2": "piccadilly",
                             "tubedir2": "Westbound"})
        for key in ("tube", "tubename", "tubeline", "tubedir",
                    "tubeline2", "tubedir2", "tubeline3", "tubedir3"):
            self.assertEqual(got[key], "", key)

    def test_dwell_is_the_stations_whole_slice(self):
        # The board divides dwtube between its Tube screens rather than
        # spending it per screen, so the value sent is unchanged by slot count.
        one = device_config(dict(ACTON, tubeline="district", tubedir="Eastbound"))
        two = device_config(dict(ACTON, tubeline="district", tubedir="Eastbound",
                                 tubeline2="piccadilly", tubedir2="Westbound"))
        self.assertEqual(one["dwtube"], two["dwtube"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
