"""tools/mkadt.py: model-number (About > Model Number) is the board codename; sweet keeps its legacy text."""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import adt, mkadt  # noqa: E402


def root_of(board):
    root = adt.parse(adt.serialize(mkadt.build(seed=b"\0" * 64, board=board)))[0]
    return dict((k, v) for k, _f, v in root.props), root


class BoardIdentity(unittest.TestCase):
    def test_oriole(self):
        self.assertEqual(root_of("oriole")[0]["model-number"], b"oriole\0")

    def test_bluejay_unchanged(self):
        self.assertEqual(root_of("bluejay")[0]["model-number"], b"bluejay\0")

    def test_sweet_unchanged(self):
        self.assertEqual(root_of("sweet")[0]["model-number"], b"bluejay\0")

    @unittest.skipUnless(os.path.exists(mkadt.D22AP_ADT), "needs research/ipsw-x/d22ap.adt")
    def test_oriole_product_description(self):
        _, root = root_of("oriole")
        prod = next(dict((k, v) for k, _f, v in c.props) for c in root.children
                    if dict((k, v) for k, _f, v in c.props).get("name") == b"product\0")
        self.assertEqual(prod["product-description"], b"Pixel 6\0")


if __name__ == "__main__":
    unittest.main()
