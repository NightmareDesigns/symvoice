import struct
import subprocess
import sys
import tempfile
import unittest
import wave
from pathlib import Path


ENGINE = Path(sys.argv.pop(1)).resolve()


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
        with wave.open(str(self.output_path), "rb") as audio:
            metadata = audio.getparams()
            samples = struct.unpack("<" + "h" * (len(audio.readframes(audio.getnframes())) // 2),
                                    self.output_path.read_bytes()[44:])
        return metadata, samples

    def test_mono_16khz_samples_are_preserved(self):
        write_wav(self.input_path, 1, 16000, [0, 1000, -1000])
        subprocess.run([ENGINE, self.input_path, self.output_path], check=True)

        metadata, samples = self.read_wav()
        self.assertEqual((metadata.nchannels, metadata.sampwidth, metadata.framerate), (1, 2, 16000))
        self.assertEqual(samples, (0, 1000, -1000))

    def test_audio_is_downmixed_and_resampled(self):
        write_wav(self.input_path, 2, 8000, [0, 0, 1000, 1000, 2000, 2000])
        subprocess.run([ENGINE, self.input_path, self.output_path], check=True)

        metadata, samples = self.read_wav()
        self.assertEqual(metadata.nframes, 6)
        self.assertEqual(samples, (0, 500, 1000, 1500, 2000, 2000))

    def test_unsupported_sample_width_is_rejected(self):
        write_wav(self.input_path, 1, 16000, [128, 129], sample_width=1)
        result = subprocess.run([ENGINE, self.input_path, self.output_path], capture_output=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output_path.exists())


if __name__ == "__main__":
    unittest.main()
