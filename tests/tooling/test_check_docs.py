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


class ReferenceAndBoundaryTests(DocumentFixture):
    def test_full_collapsed_and_shortcut_references_are_case_insensitive(self):
        (self.root / "data file.csv").write_text("data", encoding="utf-8")
        path = self.document("[Full][Data File]\n[DATA FILE][]\n[Data File]\n"
                             "[Data File]: <../data file.csv>\n")
        self.assertEqual(checker.check_document(path, self.root), [])


class CodeExampleTests(DocumentFixture):
    def test_ignores_fenced_and_indented_markdown_examples(self):
        path = self.document("```markdown\n[Example](placeholder.md)\n```\n"
                             "~~~~markdown\n![Example](placeholder.png)\n~~~\n~~~~\n"
                             "    [Indented](placeholder.md)\n\t[Tabbed](placeholder.md)\n")
        self.assertEqual(checker.check_document(path, self.root), [])

    def test_ignores_inline_code_comments_and_escaped_labels(self):
        path = self.document("`[Example](placeholder.md)`\n``![Example](placeholder.png)``\n"
                             "<!-- [Hidden](placeholder.md) -->\n\\[Escaped](placeholder.md)\n")
        self.assertEqual(checker.check_document(path, self.root), [])

    def test_preserves_real_link_line_number_after_multiline_examples(self):
        path = self.document("```markdown\n[Example](placeholder.md)\n```\n"
                             "<!--\n[Hidden](placeholder.md)\n-->\n[Real](missing.md)\n")
        issues = checker.check_document(path, self.root)
        self.assertEqual(len(issues), 1)
        self.assertIn("docs/readme.md:7:", issues[0])

    def test_reports_undefined_and_duplicate_definitions(self):
        path = self.document("[Full][missing]\n[Repeat]: ../LICENSE\n[repeat]: ../LICENSE\n")
        issues = checker.check_document(path, self.root)
        self.assertTrue(any("undefined reference missing" in issue for issue in issues))
        self.assertTrue(any("duplicate reference definition repeat" in issue for issue in issues))

    def test_rejects_parent_absolute_and_encoded_root_escapes(self):
        path = self.document("[Parent](../../outside.txt)\n[Absolute](/etc/passwd)\n"
                             "[Encoded](%2e%2e/%2e%2e/outside.txt)\n")
        issues = checker.check_document(path, self.root)
        self.assertEqual(len(issues), 3)
        self.assertTrue(all("escapes repository root" in issue for issue in issues))

    def test_rejects_symlinks_to_outside_repository(self):
        outside = tempfile.TemporaryDirectory()
        self.addCleanup(outside.cleanup)
        target = Path(outside.name) / "outside.md"
        target.write_text("outside", encoding="utf-8")
        try:
            (self.root / "docs/link.md").symlink_to(target)
        except OSError as error:
            self.skipTest(f"symlinks unavailable: {error}")
        path = self.document("[Link](link.md)\n")
        self.assertIn("escapes repository root", checker.check_document(path, self.root)[0])

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
