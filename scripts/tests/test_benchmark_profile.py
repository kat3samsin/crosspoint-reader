import configparser
import pathlib
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]


class BenchmarkProfileTests(unittest.TestCase):
    def setUp(self):
        self.config = configparser.ConfigParser(interpolation=None)
        self.config.read(REPO_ROOT / "platformio.ini")

    def test_fast_profile_disables_serial_logging(self):
        flags = self.config["env:katre_fast"]["build_flags"]
        self.assertIn("-UENABLE_SERIAL_LOG", flags)
        self.assertIn("-DFREEINK_DEVICE_X4=1", flags)
        self.assertIn("-DFREEINK_DEVICE_X3=1", flags)

    def test_benchmark_profile_reenables_serial_without_inheriting_fast_undefine(self):
        profile = self.config["env:katre_benchmark"]

        self.assertEqual(profile["extends"], "base")
        self.assertIn("${base.build_flags}", profile["build_flags"])
        self.assertIn("-DENABLE_SERIAL_LOG", profile["build_flags"])
        self.assertIn("-DFREEINK_DEVICE_X4=1", profile["build_flags"])
        self.assertIn("-DFREEINK_DEVICE_X3=1", profile["build_flags"])
        self.assertNotIn("-UENABLE_SERIAL_LOG", profile["build_flags"])


if __name__ == "__main__":
    unittest.main()
