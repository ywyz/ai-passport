import importlib.util
import io
import json
from pathlib import Path
import stat
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "translation_device_config", ROOT / "tools/configure-translation-device.py")
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)
ORIGIN = "https://translate.example.test"
TOKEN = "translation_token_0123456789abcdef"
REQUEST = "0123456789abcdef0123456789abcdef"


class TranslationDeviceConfigurationTests(unittest.TestCase):
    def test_origin_and_token_policy(self):
        config.validate(ORIGIN, TOKEN)
        for origin in ("http://translate.example.test", "https://a:b@translate.example.test",
                       "https://translate.example.test/", "https://translate.example.test:443",
                       "https://translate.example.test?token=x", "https://a..test",
                       "https://-a.test", "https://a.test/#fragment"):
            with self.assertRaises(ValueError):
                config.validate(origin, TOKEN)
        for token in ("short", TOKEN + "\r\n", TOKEN + "/", "x" * 129):
            with self.assertRaises(ValueError):
                config.validate(ORIGIN, token)

    def test_frame_and_ack_are_strict(self):
        frame, request = config.build_frame(ORIGIN, TOKEN, REQUEST)
        payload = json.loads(frame)
        self.assertEqual(set(payload), {"schema", "operation", "request_id", "origin", "token"})
        self.assertEqual(payload["schema"], 1)
        self.assertEqual(payload["operation"], "set_translation_config")
        self.assertEqual(payload["request_id"], REQUEST)
        self.assertLessEqual(len(frame), config.MAX_FRAME)
        status, verified = config.parse_ack(
            b'{"request_id":"' + REQUEST.encode() + b'","status":"OK","verified":true}\n', REQUEST)
        self.assertEqual((status, verified), ("OK", True))
        for raw in (
                b'{"request_id":"' + REQUEST.encode() + b'","status":"OK","verified":false}\n',
                b'{"request_id":"' + REQUEST.encode() + b'","status":"OK"}\n',
                b'{"request_id":"ffffffffffffffffffffffffffffffff","status":"OK","verified":true}\n',
                b'{"request_id":"' + REQUEST.encode() + b'","status":"NOPE","verified":true}\n',
                b'{"request_id":"' + REQUEST.encode() + b'","status":"OK","verified":true,"origin":"x"}\n',
                b"not-json\n", b""):
            with self.assertRaises(ValueError):
                config.parse_ack(raw, REQUEST)

    def test_private_config_is_separate_from_cli(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "translation.json"
            path.write_text(json.dumps({"origin": ORIGIN, "token": TOKEN}), encoding="utf-8")
            if config.os.name != "nt":
                path.chmod(stat.S_IRUSR | stat.S_IWUSR)
            self.assertEqual(config.load_private_config(path), (ORIGIN, TOKEN))
            path.write_text(json.dumps({"origin": ORIGIN, "token": TOKEN, "extra": 1}),
                            encoding="utf-8")
            with self.assertRaises(ValueError):
                config.load_private_config(path)
        source = (ROOT / "tools/configure-translation-device.py").read_text(encoding="utf-8")
        self.assertNotIn('add_argument("--token"', source)
        self.assertNotIn('add_argument("--origin"', source)
        self.assertNotIn("esptool", source)
        self.assertNotIn("erase_flash", source)

    def test_serial_transport_keeps_modem_lines_inactive(self):
        events = []

        class FakeSerial:
            def __init__(self):
                self.received = bytearray()

            def open(self):
                events.append("open")

            def close(self):
                events.append("close")

            def reset_input_buffer(self):
                events.append("reset-input")

            def write(self, data):
                events.append("write")
                self.received += data
                return len(data)

            def flush(self):
                events.append("flush")

            def read(self, size):
                data = b'{"request_id":"' + REQUEST.encode() + b'","status":"OK","verified":true}\n'
                if not hasattr(self, "offset"):
                    self.offset = 0
                chunk = data[self.offset:self.offset + 1]
                self.offset += len(chunk)
                return chunk

            def __setattr__(self, name, value):
                if name in {"dtr", "rts", "dsrdtr", "rtscts", "xonxoff"}:
                    events.append(f"{name}={value}")
                object.__setattr__(self, name, value)

        instance = FakeSerial()

        def serial_for_url(url, **kwargs):
            self.assertEqual(url, "rfc2217://localhost:4000")
            self.assertTrue(kwargs["do_not_open"])
            self.assertIsNone(kwargs["write_timeout"])
            events.append("factory")
            return instance

        fake = types.SimpleNamespace(serial_for_url=serial_for_url)
        frame, request = config.build_frame(ORIGIN, TOKEN, REQUEST)
        with patch.dict(sys.modules, {"serial": fake}):
            self.assertEqual(config.configure("rfc2217://localhost:4000", frame, request, 2.0),
                             ("OK", True))
        self.assertIn("dtr=False", events)
        self.assertIn("rts=False", events)
        self.assertIn("open", events)
        self.assertIn("close", events)
        self.assertNotIn("setDTR", events)
        self.assertIn(TOKEN.encode(), instance.received)
        self.assertLess(events.index("dtr=False"), events.index("open"))
        self.assertLess(events.index("rts=False"), events.index("open"))
        self.assertLess(events.index("factory"), events.index("open"))

    def test_local_serial_keeps_bounded_write_timeout_and_closes(self):
        events = []

        class LocalSerial:
            def __init__(self):
                self.data = bytearray(
                    b'{"request_id":"' + REQUEST.encode()
                    + b'","status":"OK","verified":true}\n')

            def open(self):
                events.append("open")

            def close(self):
                events.append("close")

            def reset_input_buffer(self):
                events.append("reset-input")

            def write(self, data):
                events.append("write")
                return len(data)

            def flush(self):
                events.append("flush")

            def read(self, size):
                chunk = bytes(self.data[:size])
                del self.data[:size]
                return chunk

            def __setattr__(self, name, value):
                if name in {"dtr", "rts", "dsrdtr", "rtscts", "xonxoff"}:
                    events.append(f"{name}={value}")
                object.__setattr__(self, name, value)

        instance = LocalSerial()

        def serial_for_url(url, **kwargs):
            self.assertEqual(url, "COM7")
            self.assertTrue(kwargs["do_not_open"])
            self.assertEqual(kwargs["write_timeout"], 2.0)
            events.append("factory")
            return instance

        fake = types.SimpleNamespace(serial_for_url=serial_for_url)
        frame, request = config.build_frame(ORIGIN, TOKEN, REQUEST)
        with patch.dict(sys.modules, {"serial": fake}):
            self.assertEqual(config.configure("COM7", frame, request, 2.0), ("OK", True))
        self.assertEqual(events.count("write"), 1)
        self.assertEqual(events.count("close"), 1)
        self.assertIn("dtr=False", events)
        self.assertIn("rts=False", events)

    def test_write_and_ack_failure_close_without_resend(self):
        for mode in ("write", "ack"):
            events = []

            class FailingSerial:
                def open(self):
                    events.append("open")

                def close(self):
                    events.append("close")

                def reset_input_buffer(self):
                    events.append("reset-input")

                def flush(self):
                    events.append("flush")

                def write(self, data):
                    events.append("write")
                    if mode == "write":
                        raise OSError("write failed")
                    return len(data)

                def read(self, size):
                    if mode == "ack":
                        return (b'{"request_id":"' + REQUEST.encode()
                                + b'","status":"OK","verified":true,"extra":1}\n')
                    return b""

                def __setattr__(self, name, value):
                    object.__setattr__(self, name, value)

            instance = FailingSerial()
            fake = types.SimpleNamespace(serial_for_url=lambda *a, **kw: instance)
            frame, request = config.build_frame(ORIGIN, TOKEN, REQUEST)
            with self.subTest(mode=mode), patch.dict(sys.modules, {"serial": fake}):
                with self.assertRaises((OSError, ValueError)):
                    config.configure("COM7", frame, request, 0.2)
            self.assertEqual(events.count("write"), 1)
            self.assertEqual(events.count("close"), 1)

    def test_console_prefix_before_exact_ack_matches_observed_210_bytes(self):
        prefix = b"\x1b[0;32mI (1234) suite_tasks: refresh done \x1b[0m"
        prefix += b" " * (131 - len(prefix))
        ack = (b'{"request_id":"' + REQUEST.encode()
               + b'","status":"OK","verified":true}')
        self.assertEqual(len(prefix), 131)
        self.assertEqual(len(ack), 79)
        self.assertEqual(len(prefix + ack), 210)
        self.assertEqual(self._run_script(prefix + ack + b"\n"), ("OK", True))

    def test_prefixed_ack_rejects_suffix_extra_keys_duplicates_and_controls(self):
        prefix = b"I (1234) suite_tasks: refresh done "
        ack = (b'{"request_id":"' + REQUEST.encode()
               + b'","status":"OK","verified":true}')
        bad_frames = (
            prefix + ack + b"suffix\n",
            prefix + b'{"request_id":"' + REQUEST.encode()
            + b'","status":"OK","verified":true,"extra":1}\n',
            prefix + b'{"request_id":"' + REQUEST.encode()
            + b'","request_id":"' + REQUEST.encode()
            + b'","status":"OK","verified":true}\n',
            prefix + ack + b"\x01\n",
            prefix + b'{"request_id":"' + REQUEST.encode()
            + b'","status":"O\x01K","verified":true}\n',
            prefix + ack + ack + b"\n",
        )
        for data in bad_frames:
            with self.assertRaises(ValueError):
                self._run_script(data)

    def test_prefixed_wrong_id_is_ignored_then_matching_ack_accepted(self):
        prefix = b"I (1234) suite_tasks: refresh done "
        wrong = (b'{"request_id":"' + b"f" * 32
                 + b'","status":"OK","verified":true}')
        ack = (b'{"request_id":"' + REQUEST.encode()
               + b'","status":"OK","verified":true}')
        data = prefix + wrong + b"\n" + prefix + ack + b"\n"
        self.assertEqual(self._run_script(data), ("OK", True))

    def test_prefixed_ack_rejects_nested_truncated_and_concatenated_structures(self):
        prefix = b"I (1234) suite_tasks: refresh done "
        ack = (b'{"request_id":"' + REQUEST.encode()
               + b'","status":"OK","verified":true}')
        bad_frames = (
            # complete outer object does not match the ACK schema
            prefix + b'{"extra": ' + ack + b"}",
            # truncated enclosing object
            prefix + b'{"extra": ' + ack,
            # truncated enclosing array
            prefix + b"[" + ack,
            # complete enclosing array
            prefix + b"[" + ack + b"]",
            # concatenated objects
            prefix + ack + ack,
            # object prefix with no opening brace
            prefix + b'"extra": ' + ack,
        )
        for data in bad_frames:
            with self.assertRaises(ValueError):
                self._run_script(data + b"\n")

    def test_transport_stage_tracks_ack_wait(self):
        class IdleSerial:
            def open(self):
                pass

            def close(self):
                pass

            def reset_input_buffer(self):
                pass

            def write(self, data):
                return len(data)

            def flush(self):
                pass

            def read(self, size):
                return b""

        instance = IdleSerial()
        fake = types.SimpleNamespace(serial_for_url=lambda *a, **kw: instance)
        frame, request = config.build_frame(ORIGIN, TOKEN, REQUEST)
        state = {"name": "transport"}
        with patch.dict(sys.modules, {"serial": fake}):
            with self.assertRaises(TimeoutError):
                config.configure("COM7", frame, request, 0.05, state)
        self.assertEqual(state["name"], "ack")

    def test_failure_diagnostic_reports_stage_and_class_without_secrets(self):
        def fail(*args, **kwargs):
            args[4]["name"] = "ack"
            raise ValueError("synthetic")

        output = io.StringIO()
        with patch.object(config, "load_private_config",
                          return_value=(ORIGIN, TOKEN)), \
                patch.object(config, "configure", side_effect=fail), \
                patch.object(sys, "argv",
                             ["configure-translation-device.py",
                              "--port", "COM7",
                              "--config", "ignored.json"]), \
                patch.object(sys, "stderr", new=output):
            self.assertEqual(config.main(), 1)
        text = output.getvalue()
        self.assertIn("stage=ack", text)
        self.assertIn("exception=ValueError", text)
        self.assertNotIn(TOKEN, text)
        self.assertNotIn(ORIGIN, text)

    def _run_script(self, data, timeout=1.0):
        class Scripted:
            def __init__(self):
                self.data = bytearray(data)

            def open(self):
                pass

            def close(self):
                pass

            def reset_input_buffer(self):
                pass

            def flush(self):
                pass

            def write(self, frame):
                return len(frame)

            def read(self, size):
                if not self.data:
                    return b""
                chunk = bytes(self.data[:1])
                del self.data[:1]
                return chunk

        instance = Scripted()
        fake = types.SimpleNamespace(serial_for_url=lambda *a, **kw: instance)
        frame, request = config.build_frame(ORIGIN, TOKEN, REQUEST)
        with patch.dict(sys.modules, {"serial": fake}):
            return config.configure("COM7", frame, request, timeout)

    def test_console_logs_are_filtered_without_extending_deadline(self):
        stale = json.dumps({"request_id": "f" * 32, "status": "OK", "verified": True}).encode()
        matching = json.dumps({"request_id": REQUEST, "status": "OK", "verified": True}).encode()
        data = (b"I (123) suite_weather: update complete\n" + stale + b"\n"
                + b"W (124) suite_tasks: refresh done\n" + matching + b"\n")
        self.assertEqual(self._run_script(data), ("OK", True))

        bad_acks = (
            b'{"request_id":"' + REQUEST.encode() + b'","status":"OK"}\n',
            b'{"request_id":"' + REQUEST.encode() + b'","request_id":"' + REQUEST.encode()
            + b'","status":"OK","verified":true}\n',
            b'{"request_id":"' + REQUEST.encode() + b'","status":"OK","verified":true}\x00\n',
            b'{"request_id":"' + REQUEST.encode() + b'","status":"OK","verified":true,"x":1}\n',
        )
        for data in bad_acks:
            with self.assertRaises(ValueError):
                self._run_script(b"boot log\n" + data)

        with self.assertRaises(TimeoutError):
            self._run_script(b"I (1) boot\n" * 200, timeout=0.05)


if __name__ == "__main__":
    unittest.main()
