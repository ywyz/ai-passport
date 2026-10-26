import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("device_config", ROOT / "tools/configure-task-device.py")
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)
TOKEN = "synthetic_read_token_0123456789abcdef"
WRITE = "synthetic_write_token_0123456789abcdef"


class DeviceConfigurationTests(unittest.TestCase):
    def test_origin_and_token_policy(self):
        config.validate("https://tasks.example.test", TOKEN)
        for origin in ("http://example.test", "https://a:b@example.test", "https://a.test/",
                       "https://a.test:443", "https://a.test?token=x", "https://a..test",
                       "https://-a.test", "https://a.test/#fragment"):
            with self.assertRaises(ValueError):
                config.validate(origin, TOKEN)
        for token in ("short", TOKEN + "\r\n", TOKEN + "/", "x" * 129):
            with self.assertRaises(ValueError):
                config.validate("https://tasks.example.test", token)
        self.assertIsNone(config.NoRedirect().redirect_request(None, None, 302, "", {}, "https://evil.test"))
        config.validate("https://tasks.example.test",TOKEN,WRITE)
        for token in (TOKEN,"short",WRITE+"\r\n"):
            with self.assertRaises(ValueError): config.validate("https://tasks.example.test",TOKEN,token)

    def test_optional_completion_token_is_private_and_separate(self):
        with tempfile.TemporaryDirectory() as temp:
            output=Path(temp)/"private"
            def generate(arguments,**kwargs):
                source=(output/"input.csv").read_text()
                self.assertIn("write_token",source); self.assertIn(WRITE,source); self.assertIn(TOKEN,source)
                self.assertNotIn(WRITE,arguments); self.assertNotIn(TOKEN,arguments)
                (output/"taskcfg.bin").write_bytes(b"B"*12288)
            with patch.object(config,"private_directory",side_effect=lambda p:p.mkdir()),patch.object(config.subprocess,"run",side_effect=generate):
                receipt=config.prepare("https://tasks.example.test",TOKEN,output,WRITE)
            self.assertTrue(receipt["completion_configured"])
            self.assertNotIn(WRITE,(output/"manifest.json").read_text())
            self.assertFalse((output/"input.csv").exists())

    def test_private_images_without_secret_in_arguments_or_receipt(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "private"

            def generate(arguments, **kwargs):
                self.assertNotIn(TOKEN, arguments)
                self.assertTrue(kwargs["capture_output"])
                self.assertIn(TOKEN, (output / "input.csv").read_text())
                key = (output / "taskkeys.bin").read_bytes()
                self.assertEqual(len(key), 4096)
                self.assertEqual(struct.unpack_from("<I", key, 64)[0], zlib.crc32(key[:64], 0xffffffff))
                self.assertGreater(len(set(key[:64])), 8)
                (output / "taskcfg.bin").write_bytes(b"B" * 12288)

            with patch.object(config.subprocess, "run", side_effect=generate):
                # Private directory ACL creation is separately exercised by the
                # actual Windows preparation; Linux CI uses 0700/0600 modes.
                if config.os.name == "nt":
                    with patch.object(config, "private_directory", side_effect=lambda p: p.mkdir()):
                        config.prepare("https://tasks.example.test", TOKEN, output)
                else:
                    config.prepare("https://tasks.example.test", TOKEN, output)
            self.assertFalse((output / "input.csv").exists())
            manifest = (output / "manifest.json").read_text()
            self.assertNotIn(TOKEN, manifest)
            self.assertFalse(json.loads(manifest)["flashed"])
            before = (output / "taskcfg.bin").read_bytes()
            with self.assertRaises(FileExistsError):
                config.prepare("https://tasks.example.test", TOKEN, output)
            self.assertEqual((output / "taskcfg.bin").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
