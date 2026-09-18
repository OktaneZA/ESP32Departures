"""The Home Assistant requirements (HA-nn), checked automatically.

MQTT is the first feature that can be commanded from outside the board, so the
things worth pinning are not "does it publish" - a broker proves that in
seconds - but the rules that are easy to break later and expensive to notice:
that the broker password is never echoed, that a board with no broker opens no
socket, that Home Assistant's override outranks the blank hours rather than the
other way round, and that a local press can always take control back.

Some of this is checked by running the real JavaScript through Node and the real
installer through Python. The firmware's C++ cannot be executed here - there is
no native toolchain - so those requirements are checked against the source that
implements them, which is still better than checking nothing: it catches the
deletion, the reorder and the rename.

    python tests/test_mqtt_config.py
    python tests/test_mqtt_config.py -v
"""
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG_JS = "file:///" + os.path.join(ROOT, "web", "js", "config.js").replace("\\", "/")

MQTT_KEYS = ("mqtthost", "mqttport", "mqttuser", "mqttpass", "mqttprefix", "mqtten")


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as fh:
        return fh.read()


def load_installer():
    spec = importlib.util.spec_from_file_location(
        "installer_mqtt_test", os.path.join(ROOT, "installer", "installer.py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules["installer_mqtt_test"] = module
    try:
        spec.loader.exec_module(module)
    except SystemExit:
        pass
    return module


inst = load_installer()


def device_config(overrides):
    """Run the setup page's own toDeviceConfig() over defaultConfig()."""
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


class Settings(unittest.TestCase):
    """HA-01/02: the settings exist everywhere, and fit where they are stored."""

    def test_every_front_end_knows_the_keys(self):
        web = read("web", "js", "config.js")
        for key in MQTT_KEYS:
            self.assertIn("'%s'" % key, web, "%s missing from the setup page" % key)
            self.assertIn('"%s"' % key, read("installer", "installer.py"),
                          "%s missing from the installer" % key)
            self.assertIn('k == "%s"' % key, read("src", "config.cpp"),
                          "%s missing from the firmware" % key)

    def test_nvs_names_fit(self):
        # NVS caps a key at 15 characters and fails silently past it, which is
        # why the wire names are longer than the stored ones.
        src = read("src", "config.cpp")
        for name in re.findall(r'prefs\.put\w+\("(mq[^"]*)"', src):
            self.assertLessEqual(len(name), 15, "NVS key %r is too long" % name)

    def test_stored_and_committed_names_agree(self):
        # A key read under one name and written under another loses the setting
        # on every reboot, silently.
        src = read("src", "config.cpp")
        got = set(re.findall(r'prefs\.getString\("(mq[^"]*)"', src)) | \
              set(re.findall(r'prefs\.getInt\("(mq[^"]*)"', src))
        put = set(re.findall(r'prefs\.put\w+\("(mq[^"]*)"', src))
        self.assertEqual(got, put, "load and commit disagree about the NVS names")


class Secrets(unittest.TestCase):
    """HA-03: the broker password is never reported back (PROV-07, SEC-03)."""

    def test_get_reports_a_length_not_a_password(self):
        src = read("src", "config.cpp")
        self.assertIn('Serial.print("mqttpasslen=")', src)
        # The value itself must never reach the wire.
        self.assertNotIn("Serial.println(g_cfg.mqtt_pass)", src)

    def test_installer_never_prints_the_password(self):
        src = read("installer", "installer.py")
        block = src[src.index("def summary("):]
        block = block[:block.index("\ndef ")]
        self.assertNotIn('cfg["mqttpass"]}', block)
        self.assertIn("(password set)", block)

    def test_documented_as_a_secret(self):
        self.assertIn("mqttpasslen", read("docs", "mqtt.md"))


@unittest.skipIf(shutil.which("node") is None, "node not on PATH")
class Enablement(unittest.TestCase):
    """HA-04: off means off — a board with no broker opens no socket."""

    def test_off_sends_no_broker(self):
        got = device_config({"mqttOn": False, "mqtthost": "192.168.1.73",
                             "mqttuser": "u", "mqttpass": "p"})
        self.assertEqual(got["mqtthost"], "")
        self.assertEqual(got["mqtten"], 0)
        # Credentials must not be left on a board that is not using them.
        self.assertEqual(got["mqttuser"], "")
        self.assertEqual(got["mqttpass"], "")

    def test_on_sends_what_was_typed(self):
        got = device_config({"mqttOn": True, "mqtthost": "192.168.1.73",
                             "mqttport": 8883, "mqttuser": "u", "mqttpass": "p"})
        self.assertEqual(got["mqtthost"], "192.168.1.73")
        self.assertEqual(got["mqttport"], 8883)
        self.assertEqual(got["mqtten"], 1)

    def test_anonymous_is_legal(self):
        # A blank username is the common case on a home broker, and must not be
        # treated as an incomplete configuration.
        got = device_config({"mqttOn": True, "mqtthost": "192.168.1.73"})
        self.assertEqual(got["mqttuser"], "")
        self.assertEqual(got["mqtten"], 1)

    def test_firmware_agrees_that_a_host_is_required(self):
        self.assertIn("mqtt_on != 0 && mqtt_host.length()", read("include", "config.h"))


class SwitchingOffKeepsCredentials(unittest.TestCase):
    """HA-05: declining the feature must not wipe a password nobody can read."""

    def test_installer_returns_none_rather_than_empty(self):
        # None means "don't send this key", so the board keeps what it has;
        # "" would actively clear it. The board never reports the password
        # back, so clearing it would mean retyping something unreadable.
        original = inst.ask
        try:
            inst.ask = lambda *a, **k: "1"          # "1) No Home Assistant"
            result = inst.ask_home_assistant({"mqtthost": "192.168.1.73"}, True)
        finally:
            inst.ask = original
        self.assertEqual(result[0], 0, "the switch should report off")
        for value in result[1:]:
            self.assertIsNone(value, "off must preserve the stored settings")


class Precedence(unittest.TestCase):
    """HA-06/07: who wins when Home Assistant and the board disagree."""

    def main_cpp(self):
        return read("src", "main.cpp")

    def test_home_assistant_outranks_blank_hours(self):
        # The override has to be tested BEFORE the blank-hours branch, or a
        # motion sensor could never light the board at 02:00 - which is the
        # whole point of the feature.
        src = self.main_cpp()
        body = src[src.index("static ScreenState screenState"):]
        body = body[:body.index("\n}")]
        self.assertLess(body.index("lightOverrideActive"), body.index("isBlankHour"),
                        "blank hours must not be able to overrule Home Assistant")

    def test_off_means_backlight_zero(self):
        # renderBlank() only paints black; without brightness 0 the panel is a
        # lit black rectangle, which is not what "off" means to anybody.
        src = self.main_cpp()
        self.assertIn("return { true, false, 0 }", src)

    def test_a_local_press_takes_control_back(self):
        # The recovery path when a broker dies with the screen forced off. On
        # the CYD the touchscreen is the only local control there is.
        src = self.main_cpp()
        self.assertIn("ha::clearLightOverride()", src)

    def test_the_override_is_never_written_to_nvs(self):
        # It must die at reboot: a board that came back from a power cut still
        # dark, because of a forgotten automation, is a fault report.
        self.assertNotIn("mqtt_light", read("src", "config.cpp"))
        self.assertNotIn("prefs.putBool(\"light", read("src", "config.cpp"))

    def test_view_requests_do_not_use_a_rotation_index(self):
        # active[] is rebuilt every frame and shrinks as feeds drop out, so an
        # index would select a different screen from one frame to the next.
        src = self.main_cpp()
        self.assertIn("viewToScreen(wantView)", src)


class DiscoveryContract(unittest.TestCase):
    """HA-08/09/10: what Home Assistant is promised, and when."""

    def ha_cpp(self):
        return read("src", "ha.cpp")

    def test_seven_entities(self):
        # Call sites only: `publishOne("` excludes the definition, whose first
        # parameter is not a literal. Counting `publishOne(` would include it
        # and quietly allow six real entities to pass as seven.
        src = self.ha_cpp()
        self.assertEqual(src.count('publishOne("'), 7,
                         "the documented entity count and the code disagree")

    def test_every_documented_entity_is_published(self):
        src = self.ha_cpp()
        for component, obj in (("light", "backlight"), ("button", "next"),
                               ("button", "refresh"), ("select", "view"),
                               ("sensor", "viewstate"), ("sensor", "rssi"),
                               ("binary_sensor", "screen")):
            self.assertIn('publishOne("%s", "%s"' % (component, obj), src)

    def test_discovery_is_retained(self):
        # Not retained, and the entities disappear whenever Home Assistant
        # restarts before the board does.
        src = self.ha_cpp()
        self.assertIn("pub(t, payload, 1, /*retain=*/true)", src)

    def test_republished_when_home_assistant_restarts(self):
        src = self.ha_cpp()
        self.assertIn("HA_DISCOVERY_PREFIX) + \"/status\"", src)
        self.assertIn("publishDiscovery();", src)

    def test_retained_commands_are_ignored_on_subscribe(self):
        # Otherwise a retained light/set would re-blank the board on every
        # reconnect, and the override would outlive a reboot after all.
        src = self.ha_cpp()
        self.assertIn("RETAINED_GUARD_MS", src)

    def test_availability_is_a_will_not_a_message(self):
        # "offline" has to be the broker's job: a board that has crashed cannot
        # announce that it has crashed.
        src = self.ha_cpp()
        self.assertIn("mc.lwt_msg    = \"offline\"", src)
        self.assertIn("mc.lwt_retain = 1", src)

    def test_the_render_loop_never_publishes(self):
        # MQTT never draws, and the renderer never talks to MQTT.
        src = read("src", "main.cpp")
        self.assertNotIn("esp_mqtt", src)

    def test_view_names_cover_every_view_id(self):
        src = self.ha_cpp()
        names = re.findall(r'case VIEW_\w+:\s*return "([^"]+)"', src)
        self.assertEqual(len(names), 8, "a ViewId exists with no label")
        self.assertEqual(len(set(names)), len(names), "two views share a label")


class Documented(unittest.TestCase):
    """HA-11: the requirements exist, and the guide matches the firmware."""

    def test_requirements_carry_ha_ids(self):
        src = read("REQUIREMENTS.md")
        self.assertIn("HA-01", src)
        self.assertIn("Home Assistant", src)

    def test_config_table_lists_every_key(self):
        src = read("REQUIREMENTS.md")
        for key in MQTT_KEYS:
            self.assertIn("`%s`" % key, src, "%s is not in the §4 table" % key)

    def test_guide_documents_the_real_status_words(self):
        # The troubleshooting table is only useful if it matches what GET says.
        guide = read("docs", "mqtt.md")
        for word in ("mqtt=up", "mqtt=down", "mqtt=auth", "mqtt=off"):
            self.assertIn(word, guide)
        ha = read("src", "ha.cpp")
        for word in ("\"up\"", "\"down\"", "\"auth\"", "\"off\""):
            self.assertIn(word, ha)

    def test_default_port_and_prefix_agree_everywhere(self):
        self.assertIn("kMqttDefaultPort = 1883", read("include", "config.h"))
        self.assertIn('kMqttDefaultPrefix = "departurebuddy"', read("include", "config.h"))
        self.assertIn("departurebuddy", read("docs", "mqtt.md"))
        self.assertIn("1883", read("docs", "mqtt.md"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
