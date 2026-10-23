#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Focused lexer, validation, reporting and generated-include regression tests."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools/gen_i18n.py"
spec = importlib.util.spec_from_file_location("gen_i18n", SCRIPT)
gen = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = gen
spec.loader.exec_module(gen)


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "src").mkdir()
        (self.root / "i18n/en").mkdir(parents=True)

    def source(self, text):
        path = self.root / "src/test.c"
        path.write_text(text, encoding="utf-8")
        return path

    def translations(self, text):
        (self.root / "i18n/en/test.tsv").write_text(text, encoding="utf-8")

    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root), *args],
                              capture_output=True, text=True)

    def test_comments_escapes_and_concatenation(self):
        path = self.source(r'''
// T("偽")
// continued \
T("偽物")
/* N_("無視") */
char c = '\"';
T /* comment */ ("日本" /* another */ "語\n%d");
N_("漢字\"\\"); T("\\x41");
const char *binary = "\x89PNG";
T("日本\x41" "B");
''')
        used, report = gen.scan(path)
        self.assertEqual(used, {'日本語\n%d', '漢字"\\', '\\x41', '日本AB'})
        self.assertEqual(report, [])

    def test_report_keep_only_actual_trailing_comment(self):
        path = self.source('''const char *a = "日本語"; /* i18n:keep */
const char *b = "日本語"; // /* i18n:keep */
const char *c = "日本語"; /* i18n:keep */ int x;
T("表示");
''')
        used, report = gen.scan(path)
        self.assertEqual(used, {"表示"})
        self.assertEqual([line for line, _ in report], [2, 3])

    def test_missing_translation_and_format_order(self):
        self.source('T("件数%d 名前%.*s サイズ%zu %%");')
        self.assertNotEqual(self.run_cli().returncode, 0)
        self.translations("件数%d 名前%.*s サイズ%zu %%\tCount %d Name %.*s Size %zu %%\n")
        self.assertEqual(self.run_cli().returncode, 0)
        self.translations("件数%d 名前%.*s サイズ%zu %%\tName %.*s Count %d Size %zu %%\n")
        result = self.run_cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("printf format mismatch", result.stderr)

    def test_unused_warning_and_report_status(self):
        self.source('char *s = "未翻訳";')
        self.translations("未使用\tUnused\n")
        result = self.run_cli("--report")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unwrapped Japanese", result.stdout)
        self.assertIn("warning: unused", result.stderr)
        self.assertNotIn("static const", result.stdout)
        self.assertEqual(self.run_cli().returncode, 0)

    def test_header_extraction_and_report(self):
        header = self.root / "src/rules.h"
        header.write_text('#define RULE N_("説明" "文")\n'
                          'const char *s = T("見出し");\n'
                          'const char *data = "《"; /* i18n:keep */\n'
                          'const char *missing = "未包み";\n', encoding="utf-8")
        self.translations("説明文\tDescription\n見出し\tHeading\n")
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Description', result.stdout)
        self.assertIn('Heading', result.stdout)
        report = self.run_cli("--report")
        self.assertNotEqual(report.returncode, 0)
        self.assertIn('rules.h:4: unwrapped Japanese', report.stdout)
        self.assertNotIn('rules.h:3:', report.stdout)
        self.translations("見出し\tHeading\n")
        self.assertIn('missing English translation', self.run_cli().stderr)

    def test_tsv_escaping_duplicate_conflicts_and_invalid_escape(self):
        self.source(r'T("日本\t語\n\\");')
        self.translations('日本\\t語\\n\\\\\tJapanese\\ttext\\n\\\\\n')
        self.assertEqual(self.run_cli().returncode, 0)
        self.translations('日本\tA\n日本\tB\n')
        self.assertIn("conflicting translation", self.run_cli().stderr)
        self.translations('日本\tBad\\q\n')
        self.assertIn("unsupported TSV escape", self.run_cli().stderr)

    def test_output_sort_empty_and_compile(self):
        output = self.root / "build/table.inc"
        self.assertEqual(self.run_cli("--output", str(output)).returncode, 0)
        self.assertIn("i18n_en_count = 0", output.read_text())
        self.source('T("漢字"); T("あいう"); T("日本");')
        self.translations("漢字\tKanji\nあいう\tKana\n日本\tJapan\n")
        self.assertEqual(self.run_cli("--output", str(output)).returncode, 0)
        before = output.stat().st_mtime_ns
        self.assertEqual(self.run_cli("--output", str(output)).returncode, 0)
        self.assertEqual(before, output.stat().st_mtime_ns)
        rows = output.read_text()
        self.assertLess(rows.index('"あいう"'), rows.index('"日本"'))
        self.assertLess(rows.index('"日本"'), rows.index('"漢字"'))
        cfile = self.root / "table.c"
        cfile.write_text('#include "build/table.inc"\nint main(void) { return i18n_en_count != 3 || !i18n_en[0].ja; }\n')
        compile_result = subprocess.run(["cc", "-std=c99", "-Wall", "-Werror", str(cfile), "-o", str(self.root / "table")], capture_output=True, text=True, env={**os.environ, "TMPDIR": str(self.root)})
        self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
        self.assertEqual(subprocess.run([str(self.root / "table")]).returncode, 0)


if __name__ == "__main__":
    unittest.main()
