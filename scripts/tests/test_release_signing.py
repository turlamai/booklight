"""Offline release signing contract tests; no production key required."""
import base64
import importlib.util
import pathlib
import subprocess
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "check-update-signing-key.py"
spec = importlib.util.spec_from_file_location("release_signing", SCRIPT)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class SigningKeyPreflightTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.key = subprocess.check_output(
            ["openssl", "genpkey", "-algorithm", "ED25519", "-outform", "DER"])
        cls.public = subprocess.check_output(
            ["openssl", "pkey", "-inform", "DER", "-pubout", "-outform", "DER"],
            input=cls.key)

    def test_matching_key(self):
        checker.verify(self.key, self.public)

    def test_mismatched_key(self):
        other = subprocess.check_output(
            ["openssl", "genpkey", "-algorithm", "ED25519", "-outform", "DER"])
        with self.assertRaisesRegex(ValueError, "does not match"):
            checker.verify(other, self.public)

    def test_missing_or_corrupt_key(self):
        for data in (b"", b"not a private key"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                checker.verify(data, self.public)

    def test_embedded_public_key(self):
        source = '(define update-public-key-b64 "' + base64.b64encode(self.public).decode() + '")'
        self.assertEqual(checker.public_key_from_app(source), self.public)
        with self.assertRaises(ValueError):
            checker.public_key_from_app('(define update-public-key-b64 "invalid!")')


if __name__ == "__main__":
    unittest.main()
