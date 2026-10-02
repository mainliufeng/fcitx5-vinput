#!/usr/bin/env python3
import json
from pathlib import Path
import tempfile
import unittest

from quality import edits, normalize, score, tokens, mix_noise


class QualityTest(unittest.TestCase):
    def test_metric_normalization(self):
        self.assertEqual(normalize("ＡＰＩ，测试。"), "api测试")
        self.assertEqual(edits("abc", "ac"), 1)
        self.assertEqual(edits("", "幻觉"), 2)
        self.assertEqual(
            tokens("部署 GitHub API，今天"), ["部", "署", "github", "api", "今", "天"]
        )

    def test_english_word_boundary_is_not_hidden(self):
        self.assertEqual(normalize("hello world"), normalize("helloworld"))
        self.assertGreater(edits(tokens("hello world"), tokens("helloworld")), 0)

    def test_noise_is_reproducible_and_does_not_clip(self):
        clean = [1000, -1000] * 100
        self.assertEqual(mix_noise(clean, 10, 123), mix_noise(clean, 10, 123))
        self.assertLessEqual(max(abs(x) for x in mix_noise(clean, 10, 123)), 30000)

    def test_missing_duplicate_and_failed_results_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, results = Path(directory) / "m.jsonl", Path(directory) / "r.jsonl"
            case = dict(id="a", reference="你好", category="zh/clean", fold="dev")
            manifest.write_text(json.dumps(case) + "\n")
            for rows in [
                [],
                [dict(id="a", text=""), dict(id="a", text="")],
                [dict(id="a", error="failed")],
            ]:
                results.write_text("".join(json.dumps(r) + "\n" for r in rows))
                with self.assertRaises(ValueError):
                    score(manifest, results)
            results.write_text(json.dumps(dict(id="a", text="")) + "\n")
            self.assertEqual(
                score(manifest, results)["summary"]["dev/zh/clean"]["cer"], 1
            )

    def test_silence_false_insertions(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, results = Path(directory) / "m.jsonl", Path(directory) / "r.jsonl"
            manifest.write_text(
                json.dumps(dict(id="a", reference="", category="silence", fold="dev"))
                + "\n"
            )
            results.write_text(json.dumps(dict(id="a", text="谢谢观看")) + "\n")
            self.assertEqual(
                score(manifest, results)["summary"]["dev/silence"]["false_insertions"],
                1,
            )


if __name__ == "__main__":
    unittest.main()
