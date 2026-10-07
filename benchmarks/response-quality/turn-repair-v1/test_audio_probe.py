import tempfile
import unittest
import wave
from pathlib import Path

from audio_repair_probe import distance, pcm, repair_present, words


class AudioProbeTests(unittest.TestCase):
    def test_edits_and_repair_polarity(self):
        self.assertEqual(distance(words("a b c"), words("a c")), 1)
        self.assertEqual(distance(words("a b"), words("a c d")), 2)
        self.assertTrue(repair_present("question", "No wait, is she here?"))
        self.assertFalse(repair_present("question", "Now wait, is she here?"))
        self.assertTrue(repair_present("cancel", "No, don't cast it."))
        self.assertFalse(repair_present("cancel", "Cast it."))

    def test_empty_synthesis_is_not_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "empty.wav"
            with wave.open(str(path), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(16000)
                wav.writeframes(b"")
            with self.assertRaisesRegex(ValueError, "Empty or oversized"):
                pcm(path)


if __name__ == "__main__":
    unittest.main()
