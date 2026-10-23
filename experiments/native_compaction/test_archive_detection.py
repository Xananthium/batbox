"""Regression for a summary mentioning archive labels without retaining rows."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).with_name('live_100k.py')
spec = importlib.util.spec_from_file_location('live_100k', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ArchiveDetection(unittest.TestCase):
    def test_complete_records_and_summary_label_mentions(self):
        row = ('ARCHIVE_ROW_000123: sensor cedar status nominal; batch 26; '
               'no decision or action is needed.')
        summary = 'Archived labels ARCHIVE_ROW_000000 through ARCHIVE_ROW_003399 were disposable.'
        self.assertEqual(module.archive_row_count(row), 1)
        self.assertEqual(module.archive_row_count((row + '\n' + row).encode()), 2)
        self.assertEqual(module.archive_row_count(summary), 0)
        self.assertEqual(module.archive_row_count(summary + '\n' + row), 1)


if __name__ == '__main__':
    unittest.main()
