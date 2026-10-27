"""Reviewed patch migration must be exact and never discard local changes."""

import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/apply-reviewed-patch.sh"


class ReviewedPatchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.repo = self.root / "checkout"
        self.repo.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Patch Test")
        self.git("config", "user.email", "patch-test@example.invalid")
        (self.repo / "tracked").write_text("base\n")
        (self.repo / "other").write_text("unchanged\n")
        self.git("add", "tracked", "other")
        self.git("commit", "-qm", "base")
        self.patch_old = self.make_patch("old\n", "old added\n", "old.patch")
        self.patch_new = self.make_patch("new\n", "new added\n", "new.patch")
        self.old_sha = hashlib.sha256(self.patch_old.read_bytes()).hexdigest()

    def tearDown(self):
        self.tmp.cleanup()

    def git(self, *args):
        return subprocess.run(
            ["git", "-C", str(self.repo), *args],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ).stdout

    def make_patch(self, tracked, added, filename):
        (self.repo / "tracked").write_text(tracked)
        (self.repo / "added").write_text(added)
        self.git("add", "--intent-to-add", "added")
        patch = self.root / filename
        patch.write_bytes(self.git("diff", "--binary", "--full-index", "HEAD"))
        self.git("restore", "tracked")
        self.git("reset", "-q", "--", "added")
        (self.repo / "added").unlink()
        return patch

    def run_apply(self, previous_sha, check=True):
        return subprocess.run(
            [str(SCRIPT), str(self.repo), str(self.patch_new), previous_sha, "test"],
            check=check,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def test_pristine_and_repeated_application(self):
        self.run_apply(self.old_sha)
        self.run_apply(self.old_sha)
        self.assertEqual(
            self.git("diff", "--binary", "--full-index", "HEAD"),
            self.patch_new.read_bytes(),
        )

    def test_unpatched_checkout_with_local_changes_is_left_untouched(self):
        (self.repo / "other").write_text("user change\n")
        before = self.git("diff", "--binary", "--full-index", "HEAD")
        result = self.run_apply(self.old_sha, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"patch was not applied", result.stderr)
        self.assertEqual(before, self.git("diff", "--binary", "--full-index", "HEAD"))
        self.assertEqual((self.repo / "tracked").read_text(), "base\n")
        self.assertFalse((self.repo / "added").exists())

    def test_exact_old_patch_is_upgraded_including_created_files(self):
        self.git("apply", str(self.patch_old))
        self.git("add", "--intent-to-add", "added")
        self.run_apply(self.old_sha)
        self.assertEqual((self.repo / "added").read_text(), "new added\n")
        self.assertEqual(
            self.git("diff", "--binary", "--full-index", "HEAD"),
            self.patch_new.read_bytes(),
        )

    def test_any_one_of_several_exact_previous_hashes_is_upgraded(self):
        self.git("apply", str(self.patch_old))
        self.git("add", "--intent-to-add", "added")
        self.run_apply("0" * 64 + "," + self.old_sha + "," + "f" * 64)
        self.assertEqual((self.repo / "added").read_text(), "new added\n")

    def test_hash_substrings_and_unlisted_hashes_are_rejected(self):
        self.git("apply", str(self.patch_old))
        self.git("add", "--intent-to-add", "added")
        before = self.git("diff", "--binary", "--full-index", "HEAD")
        result = self.run_apply("prefix" + self.old_sha + "suffix", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(before, self.git("diff", "--binary", "--full-index", "HEAD"))

    def test_staged_old_patch_is_not_upgraded_or_unstaged(self):
        self.git("apply", str(self.patch_old))
        self.git("add", "tracked", "added")
        before_index = self.git("diff", "--cached", "--binary", "--full-index", "HEAD")
        before_worktree = self.git("diff", "--binary", "--full-index", "HEAD")
        result = self.run_apply(self.old_sha, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"staged changes; patch was not upgraded", result.stderr)
        self.assertEqual(
            before_index,
            self.git("diff", "--cached", "--binary", "--full-index", "HEAD"),
        )
        self.assertEqual(
            before_worktree, self.git("diff", "--binary", "--full-index", "HEAD")
        )

    def test_conflicting_untracked_file_restores_the_previous_patch(self):
        (self.repo / "tracked").write_text("new\n")
        (self.repo / "added").write_text("new added\n")
        (self.repo / "collision").write_text("created by new patch\n")
        self.git("add", "--intent-to-add", "added", "collision")
        self.patch_new.write_bytes(self.git("diff", "--binary", "--full-index", "HEAD"))
        self.git("restore", "tracked")
        self.git("reset", "-q", "--", "added", "collision")
        (self.repo / "added").unlink()
        (self.repo / "collision").unlink()
        self.git("apply", str(self.patch_old))
        self.git("add", "--intent-to-add", "added")
        (self.repo / "collision").write_text("user file\n")
        before = self.git("diff", "--binary", "--full-index", "HEAD")
        result = self.run_apply(self.old_sha, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"restored previous patch", result.stderr)
        self.assertEqual(before, self.git("diff", "--binary", "--full-index", "HEAD"))
        self.assertEqual((self.repo / "added").read_text(), "old added\n")
        self.assertEqual((self.repo / "collision").read_text(), "user file\n")

    def test_old_patch_with_unknown_changes_is_rejected_without_losing_work(self):
        self.git("apply", str(self.patch_old))
        self.git("add", "--intent-to-add", "added")
        (self.repo / "other").write_text("user change\n")
        before = self.git("diff", "--binary", "--full-index", "HEAD")
        result = self.run_apply(self.old_sha, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"refusing to replace unknown local changes", result.stderr)
        self.assertEqual(before, self.git("diff", "--binary", "--full-index", "HEAD"))
        self.assertEqual((self.repo / "added").read_text(), "old added\n")
        self.assertEqual((self.repo / "other").read_text(), "user change\n")


if __name__ == "__main__":
    unittest.main()
