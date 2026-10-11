import io
import json
import importlib.util
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
import wave
from pathlib import Path


ENGINE = Path(sys.argv.pop(1)).resolve()
CLI = Path(__file__).resolve().parents[1] / "python" / "symvoice.py"
SPEC = importlib.util.spec_from_file_location("symvoice_cli", CLI)
SYMVOICE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SYMVOICE)


def write_wav(path, channels, sample_rate, samples, sample_width=2):
    with wave.open(str(path), "wb") as output:
        output.setnchannels(channels)
        output.setsampwidth(sample_width)
        output.setframerate(sample_rate)
        if sample_width == 2:
            output.writeframes(b"".join(struct.pack("<h", value) for value in samples))
        else:
            output.writeframes(bytes(samples))


class PrepareAudioTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp_dir.name)
        self.input_path = self.directory / "input.wav"
        self.output_path = self.directory / "output.wav"

    def tearDown(self):
        self.temp_dir.cleanup()

    def read_wav(self):
        return self.read_wav_from(self.output_path)

    def read_wav_from(self, path):
        with wave.open(str(path), "rb") as audio:
            metadata = audio.getparams()
            frames = audio.readframes(audio.getnframes())
            samples = struct.unpack("<" + "h" * (len(frames) // 2), frames)
        return metadata, samples

    def test_mono_16khz_samples_are_preserved(self):
        write_wav(self.input_path, 1, 16000, [0, 1000, -1000])
        subprocess.run([ENGINE, "prepare", self.input_path, self.output_path], check=True)

        metadata, samples = self.read_wav()
        self.assertEqual((metadata.nchannels, metadata.sampwidth, metadata.framerate), (1, 2, 16000))
        self.assertEqual(samples, (0, 1000, -1000))

    def test_audio_is_downmixed_and_resampled(self):
        write_wav(self.input_path, 2, 8000, [0, 0, 1000, 1000, 2000, 2000])
        subprocess.run([ENGINE, "prepare", self.input_path, self.output_path], check=True)

        metadata, samples = self.read_wav()
        self.assertEqual(metadata.nframes, 6)
        self.assertEqual(samples, (0, 500, 1000, 1500, 2000, 2000))

    def test_unsupported_sample_width_is_rejected(self):
        write_wav(self.input_path, 1, 16000, [128, 129], sample_width=1)
        result = subprocess.run(
            [ENGINE, "prepare", self.input_path, self.output_path], capture_output=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output_path.exists())

    def test_trim_removes_long_silent_edges_and_preserves_voice_margin(self):
        samples = [0] * 6000
        samples[2000:2101] = [1000] * 101
        write_wav(self.input_path, 1, 16000, samples)

        result = subprocess.run(
            [ENGINE, "trim", self.input_path, self.output_path], capture_output=True)

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        metadata, trimmed = self.read_wav()
        self.assertEqual(metadata.nframes, 3301)
        self.assertEqual(trimmed[:1600], (0,) * 1600)
        self.assertEqual(trimmed[1600:1701], (1000,) * 101)

    def test_trim_rejects_silent_audio(self):
        write_wav(self.input_path, 1, 16000, [0] * 100)

        result = subprocess.run(
            [ENGINE, "trim", self.input_path, self.output_path], capture_output=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.input_path.exists())
        self.assertFalse(self.output_path.exists())

    def test_recording_rejects_zero_duration_without_opening_microphone(self):
        result = subprocess.run(
            [ENGINE, "record", "0", self.output_path], capture_output=True)

        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.output_path.exists())

    def test_recording_rejects_duration_above_limit(self):
        result = subprocess.run(
            [ENGINE, "record", "3601", self.output_path], capture_output=True)

        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.output_path.exists())

    @unittest.skipIf(sys.platform == "win32", "Windows microphone input is hardware-dependent")
    def test_recording_is_reported_as_unsupported_off_windows(self):
        result = subprocess.run(
            [ENGINE, "record", "1", self.output_path], capture_output=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"only on Windows", result.stderr)
        self.assertFalse(self.output_path.exists())

    def test_playback_rejects_invalid_wav(self):
        self.input_path.write_bytes(b"not a WAV file")
        result = subprocess.run(
            [ENGINE, "play", self.input_path], capture_output=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"valid 16-bit PCM WAV", result.stderr)

    @unittest.skipIf(sys.platform == "win32", "Windows playback depends on an audio device")
    def test_playback_is_reported_as_unsupported_off_windows(self):
        write_wav(self.input_path, 1, 16000, [0, 1000, -1000])
        result = subprocess.run(
            [ENGINE, "play", self.input_path], capture_output=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"only on Windows", result.stderr)


class VoiceDatasetTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp_dir.name)
        self.dataset = self.directory / "voice"
        self.input_path = self.directory / "input.wav"
        subprocess.run([sys.executable, CLI, "dataset", "init", self.dataset], check=True)

    def tearDown(self):
        self.temp_dir.cleanup()

    def run_cli(self, *arguments, check=False):
        return subprocess.run(
            [sys.executable, CLI, *map(str, arguments)], capture_output=True, check=check)

    def add_sample(self, text="A short voice sample."):
        write_wav(self.input_path, 2, 8000, [0, 0, 1000, 1000, 2000, 2000])
        return self.run_cli(
            "dataset", "add", self.dataset, self.input_path, "--text", text,
            "--engine", ENGINE)

    def test_add_normalizes_labels_and_validates_sample(self):
        result = self.add_sample("  Hello Symvoice.  ")

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        manifest = json.loads((self.dataset / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(len(manifest["samples"]), 1)
        entry = manifest["samples"][0]
        self.assertEqual(entry["text"], "Hello Symvoice.")
        self.assertEqual(entry["duration_seconds"], 0.000375)
        with wave.open(str(self.dataset / entry["audio"]), "rb") as audio:
            self.assertEqual(
                (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()),
                (1, 2, 16000),
            )

        validated = self.run_cli("dataset", "validate", self.dataset)
        self.assertEqual(validated.returncode, 0, validated.stderr.decode())
        self.assertIn(b"1 sample(s)", validated.stdout)

    def test_add_rejects_empty_transcript_without_adding_sample(self):
        result = self.add_sample("   ")

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(list((self.dataset / "audio").glob("*.wav")))
        manifest = json.loads((self.dataset / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["samples"], [])

    def test_add_rejects_invalid_input_without_changing_corpus(self):
        self.input_path.write_bytes(b"not audio")
        result = self.run_cli(
            "dataset", "add", self.dataset, self.input_path, "--text", "words",
            "--engine", ENGINE)

        self.assertNotEqual(result.returncode, 0)
        manifest = json.loads((self.dataset / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["samples"], [])

    def test_validate_rejects_unsafe_manifest_audio_path(self):
        manifest_path = self.dataset / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["samples"].append({
            "id": "unsafe",
            "audio": "../outside.wav",
            "text": "hello",
            "duration_seconds": 1,
        })
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

        result = self.run_cli("dataset", "validate", self.dataset)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"unsafe audio path", result.stderr)

    def test_record_rejects_out_of_range_duration(self):
        result = self.run_cli(
            "dataset", "record", self.dataset, "--text", "Hello.", "--seconds", "0",
            "--engine", ENGINE)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"between 1 and 3600", result.stderr)

    def test_word_record_rejects_more_than_one_word(self):
        result = self.run_cli(
            "dataset", "word-record", self.dataset, "--text", "two words",
            "--engine", ENGINE)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"exactly one word", result.stderr)

    def add_word(self, text, samples):
        write_wav(self.input_path, 1, 16000, samples)
        return self.run_cli(
            "dataset", "word-add", self.dataset, self.input_path, "--text", text,
            "--engine", ENGINE)

    def test_recorded_words_synthesize_to_wav_with_a_short_join_pause(self):
        self.assertEqual(self.add_word("Hello", [1000, 2000]).returncode, 0)
        self.assertEqual(self.add_word("world", [-1000, -2000]).returncode, 0)
        output_path = self.directory / "spoken.wav"

        result = self.run_cli(
            "dataset", "speak", self.dataset, output_path, "--text", "HELLO, world!",
            "--engine", ENGINE)

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        with wave.open(str(output_path), "rb") as audio:
            self.assertEqual(
                (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()),
                (1, 2, 16000),
            )
            frames = audio.readframes(audio.getnframes())
        samples = struct.unpack("<" + "h" * (len(frames) // 2), frames)
        self.assertEqual(samples[:2], (1000, 2000))
        self.assertEqual(samples[2:322], (0,) * 320)
        self.assertEqual(samples[322:], (-1000, -2000))

    def test_synthesis_accepts_piped_ai_text(self):
        self.assertEqual(self.add_word("Hello", [1000, 2000]).returncode, 0)
        output_path = self.directory / "spoken.wav"
        result = subprocess.run(
            [sys.executable, CLI, "dataset", "speak", self.dataset, output_path,
             "--text", "-", "--engine", str(ENGINE)],
            input="Hello!\n", text=True, capture_output=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output_path.is_file())
        self.assertIn("Created speech WAV", result.stdout)

    def test_piped_synthesis_text_has_a_size_limit(self):
        result = subprocess.run(
            [sys.executable, CLI, "dataset", "speak", self.dataset,
             self.directory / "spoken.wav", "--text", "-", "--engine", str(ENGINE)],
            input="x" * 65537, text=True, capture_output=True,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must not exceed 65536 characters", result.stderr)

    def test_synthesis_reports_missing_words_without_creating_output(self):
        output_path = self.directory / "spoken.wav"

        result = self.run_cli(
            "dataset", "speak", self.dataset, output_path, "--text", "unrecorded",
            "--engine", ENGINE)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"record these words or phrases first", result.stderr)
        self.assertFalse(output_path.exists())

    def test_synthesis_prefers_a_recorded_phrase_to_individual_words(self):
        write_wav(self.input_path, 1, 16000, [3000, -3000, 2000])
        added = self.run_cli(
            "dataset", "add", self.dataset, self.input_path, "--text", "Hello there.",
            "--engine", ENGINE)
        self.assertEqual(added.returncode, 0, added.stderr.decode())
        output_path = self.directory / "spoken.wav"

        result = self.run_cli(
            "dataset", "speak", self.dataset, output_path, "--text", "HELLO, there!",
            "--engine", ENGINE)

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        with wave.open(str(output_path), "rb") as audio:
            self.assertEqual(audio.getnframes(), 3)
            self.assertEqual(
                struct.unpack("<hhh", audio.readframes(3)),
                (3000, -3000, 2000),
            )

    def test_synthesis_cannot_overwrite_a_recorded_word(self):
        self.assertEqual(self.add_word("hello", [1000, 2000]).returncode, 0)
        manifest = json.loads((self.dataset / "manifest.json").read_text(encoding="utf-8"))
        word_path = self.dataset / manifest["samples"][0]["audio"]

        result = self.run_cli(
            "dataset", "speak", self.dataset, word_path, "--text", "hello",
            "--engine", ENGINE)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"must not overwrite", result.stderr)
        self.assertEqual(wave.open(str(word_path), "rb").getnframes(), 2)

    def test_word_recording_rejects_duplicate_word(self):
        self.assertEqual(self.add_word("Hello!", [1000]).returncode, 0)
        result = self.add_word("hello", [2000])

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"already recorded", result.stderr)


class SpeechApiTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp_dir.name)
        self.dataset = self.directory / "voice"
        self.input_path = self.directory / "hello.wav"
        self.output_path = self.directory / "hello-normalized.wav"
        subprocess.run([sys.executable, CLI, "dataset", "init", self.dataset], check=True)
        write_wav(self.input_path, 1, 16000, [1000, 2000, -1000])
        subprocess.run(
            [sys.executable, CLI, "dataset", "word-add", self.dataset, self.input_path,
             "--text", "hello", "--engine", ENGINE],
            check=True, capture_output=True,
        )
        self.server = SYMVOICE.ThreadingHTTPServer(
            ("127.0.0.1", 0), SYMVOICE.make_speech_handler(self.dataset, ENGINE))
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.temp_dir.cleanup()

    def post_json(self, value):
        request = urllib.request.Request(
            self.url + "/v1/audio/speech",
            data=json.dumps(value).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        return urllib.request.urlopen(request, timeout=5)

    def test_health_endpoint(self):
        with urllib.request.urlopen(self.url + "/health", timeout=5) as response:
            self.assertEqual(response.status, 200)
            self.assertEqual(json.loads(response.read()), {"status": "ok"})

    def test_speech_endpoint_returns_recorded_voice_as_wav(self):
        with self.post_json({"input": "Hello!"}) as response:
            self.assertEqual(response.status, 200)
            self.assertEqual(response.headers.get_content_type(), "audio/wav")
            payload = response.read()
        with wave.open(io.BytesIO(payload), "rb") as audio:
            self.assertEqual(
                (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()),
                (1, 2, 16000),
            )
            self.assertGreater(audio.getnframes(), 0)

    def test_missing_vocabulary_returns_json_client_error(self):
        with self.assertRaises(urllib.error.HTTPError) as raised:
            self.post_json({"input": "missing"})

        self.assertEqual(raised.exception.code, 400)
        body = json.loads(raised.exception.read())
        self.assertIn("record these words or phrases first", body["error"])

    def test_non_wav_format_is_rejected(self):
        with self.assertRaises(urllib.error.HTTPError) as raised:
            self.post_json({"input": "hello", "response_format": "mp3"})

        self.assertEqual(raised.exception.code, 400)
        self.assertIn("only WAV", json.loads(raised.exception.read())["error"])

    def test_oversized_request_is_rejected_before_body_read(self):
        body_limit = SYMVOICE.MAX_REQUEST_BYTES
        request = urllib.request.Request(
            self.url + "/v1/audio/speech",
            data=b"x" * (body_limit + 1),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with self.assertRaises(urllib.error.HTTPError) as raised:
            urllib.request.urlopen(request, timeout=5)

        self.assertEqual(raised.exception.code, 413)

    def test_text_character_limit_is_enforced(self):
        with self.assertRaises(urllib.error.HTTPError) as raised:
            self.post_json({"input": "h" * (SYMVOICE.MAX_TEXT_LENGTH + 1)})

        self.assertEqual(raised.exception.code, 400)
        self.assertIn(
            "must not exceed", json.loads(raised.exception.read())["error"])

    def test_utf8_request_body_with_multibyte_text_fits_byte_limit(self):
        request = urllib.request.Request(
            self.url + "/v1/audio/speech",
            data=json.dumps({"input": "é" * 1000}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with self.assertRaises(urllib.error.HTTPError) as raised:
            urllib.request.urlopen(request, timeout=5)

        self.assertEqual(raised.exception.code, 400)
        self.assertIn(
            "record these words or phrases first",
            json.loads(raised.exception.read())["error"],
        )


class WindowsIntegrationTests(unittest.TestCase):
    def test_sapi_com_class_guid_matches_registration_scripts(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "native" / "src" / "sapi_engine.c").read_text(encoding="utf-8")
        register = (root / "scripts" / "Register-Symvoice.ps1").read_text(encoding="utf-8")
        unregister = (root / "scripts" / "Unregister-Symvoice.ps1").read_text(encoding="utf-8")
        clsid = "73EA0E66-7B92-4D89-8BE0-A633F9274291"

        self.assertIn("0x73ea0e66", source.lower())
        self.assertIn("0x7b92", source.lower())
        self.assertIn("0x4d89", source.lower())
        self.assertIn("0x8b, 0xe0, 0xa6, 0x33, 0xf9, 0x27, 0x42, 0x91", source.lower())
        self.assertIn(clsid, register)
        self.assertIn(clsid, unregister)


if __name__ == "__main__":
    unittest.main()
