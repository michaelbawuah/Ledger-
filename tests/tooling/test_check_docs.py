"""Offline Markdown-link checker behavior and filesystem boundary tests."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "tools/check_docs.py"
SPEC = importlib.util.spec_from_file_location("check_docs", SCRIPT)
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class DocumentFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        (self.root / "docs").mkdir()

    def document(self, text, name="docs/readme.md"):
        path = self.root / name
        path.write_text(text, encoding="utf-8")
        return path


class InlineLinkTests(DocumentFixture):
    def test_existing_parent_paths_spaces_images_and_balanced_parentheses(self):
        for name in ("data file.csv", "plot(1).svg", "plot.svg"):
            (self.root / name).write_text("asset", encoding="utf-8")
        path = self.document('[Data](../data file.csv)\n![Plot](../plot(1).svg)\n'
                             '[Encoded](../data%20file.csv)\n[Title](<../data file.csv> "Data")\n'
                             '![Plot](../plot.svg "Chart")\n')
        self.assertEqual(checker.check_document(path, self.root), [])

    def test_reports_missing_links_with_line_numbers(self):
        path = self.document("Intro\n[Missing](missing.md)\n![Absent](absent.png)\n")
        issues = checker.check_document(path, self.root)
        self.assertEqual(len(issues), 2)
        self.assertIn("docs/readme.md:2:", issues[0])
        self.assertIn("absent.png", issues[1])

    def test_external_urls_are_ignored_without_network_access(self):
        path = self.document("[Web](https://invalid.example/no-page)\n"
                             "[Email](mailto:user@example.com)\n[CDN](//invalid.example/image.png)\n")
        self.assertEqual(checker.check_document(path, self.root), [])


if __name__ == "__main__":
    unittest.main()
