"""The package source record cannot claim publication or use an unpinned source."""

from pathlib import Path
import sys
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from release_source import create, validate  # noqa: E402


class ReleaseSourceTests(unittest.TestCase):
    def test_record_is_pinned_without_availability_claim(self):
        commit = "a" * 40
        record = create("3.0.0-beta.1", commit)
        self.assertIn(commit, record["projectSourceUrl"])
        self.assertIn(commit, record["thirdPartySourceUrl"])
        self.assertNotIn("verifiedAvailable", record)
        validate(record, "3.0.0-beta.1")
        record["verifiedAvailable"] = True
        with self.assertRaises(ValueError):
            validate(record, "3.0.0-beta.1")

    def test_rejects_unpinned_or_unverified_source(self):
        with self.assertRaises(ValueError):
            validate({**create("3.0.0-beta.1", "a" * 40),
                      "thirdPartySourceUrl": "http://example.test/source.md"}, "3.0.0-beta.1")
        with self.assertRaises(ValueError):
            create("3.0.0-beta.1", "a" * 39)


if __name__ == "__main__":
    unittest.main()
