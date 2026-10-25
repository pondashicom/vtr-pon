"""The Windows source checker must not accept the Mac tree or mixed roots."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('source_access', ROOT / 'scripts/check-release-source-access.py')
source_access = importlib.util.module_from_spec(spec)
import sys
sys.path.insert(0, str(ROOT / 'scripts'))
spec.loader.exec_module(source_access)


class SourceLayoutTests(unittest.TestCase):
    def make_archive(self, root, *, second=None, split=False, index=b'https://example.test/source ' + b'a' * 64):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / 'source.zip'
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr(root + '/release/version.json', json.dumps({'version': '3.0.0-beta.2', 'releaseReady': True}))
            archive.writestr(('snapshot/3/mac' if split else root) + '/BUILDING.md', 'Build instructions')
            archive.writestr(root + '/LICENSE/THIRD-PARTY-SOURCES.md', index)
            archive.writestr(root + '/LICENSE/third-party-sources.json', json.dumps({'sources': [
                {'url': 'https://example.test/source', 'sha256': 'a' * 64}]}))
            if second:
                archive.writestr(second + '/release/version.json', json.dumps({'version': 'different', 'releaseReady': False}))
        return path

    def check(self, path):
        source_access.verify_project_zip(path, '3.0.0-beta.2',
            hashlib.sha256(b'https://example.test/source ' + b'a' * 64).hexdigest())

    def test_windows_selected_with_mac_alongside(self):
        self.check(self.make_archive('snapshot/3/windows', second='snapshot/3/mac'))

    def test_legacy_tag_layout(self):
        self.check(self.make_archive('snapshot/3'))

    def test_mac_only_rejected(self):
        with self.assertRaises(ValueError):
            self.check(self.make_archive('snapshot/3/mac'))

    def test_mixed_documents_rejected(self):
        with self.assertRaises(ValueError):
            self.check(self.make_archive('snapshot/3/windows', split=True))

    def test_ambiguous_windows_roots_rejected(self):
        with self.assertRaises(ValueError):
            self.check(self.make_archive('snapshot/3/windows', second='snapshot/3'))

    def test_tampered_source_index_rejected(self):
        with self.assertRaises(ValueError):
            self.check(self.make_archive('snapshot/3/windows', index=b'changed'))


if __name__ == '__main__':
    unittest.main()
