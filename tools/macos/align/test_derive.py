# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of the frame derivation (derive.py) on synthetic stacks.

  python3 -I -m unittest discover -s tools/macos -p 'test_*.py'"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import derive  # noqa: E402


class Image:
    """Functions as 0x100-byte blocks: an address's function is its address rounded down."""

    def functionOf(self, address):
        return address & ~0xFF


def pair(source, target, block=10):
    return ('root', ('W', 1), tuple(source), tuple(target), block)


class DeriveTest(unittest.TestCase):
    def derived(self, pairs, table, fpair):
        image = Image()
        derived, failures = derive.derive(pairs, table, fpair, image, image)
        self.assertEqual(failures, [])
        self.assertEqual(derive.replay(pairs, table, derived), [])
        return derived

    def test_splits_chains_over_helpers_the_target_calls(self):
        # The source writes two fields itself; the target writes them through a helper of its own
        # (0x7000), which its frames in the paired function carry.
        table = {(0x1010, 0, 0): (0xA0,), (0x2010, 0, 0): (0xB1,), (0x2020, 0, 0): (0xB2,)}
        pairs = [pair([0x1010, 0x2010], [0x5010, 0x6010, 0x7010]),
                 pair([0x1010, 0x2020], [0x5010, 0x6020, 0x7010])]
        derived = self.derived(pairs, table, {0x1000: 0x5000, 0x2000: 0x6000})
        self.assertEqual(derived[(0x5010, 0, 0)], (0xA0,))
        self.assertEqual(derived[(0x6010, 0, 0)], (0xB1,))
        self.assertEqual(derived[(0x7010, 0, 0)], ())

    def test_splits_one_frame_over_the_frames_the_target_outlined(self):
        # One source frame stands for a rating and its part; the target calls the ratings' writer,
        # so the part stays on the outer frame and each rating goes to its write.
        table = {(0x1010, 0, 0): (0xC1, 0xD0), (0x1020, 0, 0): (0xC2, 0xD0), (0x1030, 0, 0): (0xC1, 0xD1),
                 (0x1040, 0, 0): (0xC2, 0xD1)}
        pairs = [pair([0x1010], [0x5010, 0x6010]), pair([0x1020], [0x5010, 0x6020]),
                 pair([0x1030], [0x5020, 0x6010]), pair([0x1040], [0x5020, 0x6020])]
        derived = self.derived(pairs, table, {0x1000: 0x5000})
        self.assertEqual(derived[(0x5010, 0, 0)], (0xD0,))
        self.assertEqual(derived[(0x6020, 0, 0)], (0xC2,))

    def test_restricts_a_shared_helper_to_its_callers(self):
        # A helper writing four bytes stands for the header's bytes under one caller, and for a
        # plan's under another.
        table = {}
        pairs = []
        for k in range(2):
            table[(0x1010 + 4 * k, 0, 0)] = (0xE0 + k,)
            table[(0x3010 + 4 * k, 0, 0)] = (0xF0 + k,)
            pairs.append(pair([0x1010 + 4 * k], [0x5010, 0x7010 + 4 * k]))
            pairs.append(pair([0x3010 + 4 * k], [0x6010, 0x7010 + 4 * k]))
        derived = self.derived(pairs, table, {0x1000: 0x5000, 0x3000: 0x6000})
        self.assertEqual(derive.translate(derived, (0x5010, 0x7014)), (0xE1,))
        self.assertEqual(derive.translate(derived, (0x6010, 0x7014)), (0xF1,))
        self.assertTrue(any(key[1] for key in derived))

    def test_looks_up_the_most_specific_context(self):
        table = {(0x10, 0, 0): (1,), (0x10, 0x20, 0): (2,), (0x10, 0x20, 0x30): (3,)}
        self.assertEqual(derive.translate(table, (0x40, 0x20, 0x10)), (2,))
        self.assertEqual(derive.translate(table, (0x30, 0x20, 0x10)), (3,))
        self.assertEqual(derive.translate(table, (0x50, 0x10)), (1,))
        self.assertEqual(derive.untranslated(table, (0x50, 0x10)), [0x50])


if __name__ == '__main__':
    unittest.main()
