# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of what gen_tables.py refuses in a plugin edition's mappings: frames, synthetic IDs and
absent sites. Each test changes a copy of the FM27 mapping.

  python3 -I -m unittest discover -s tools/macos/align -p 'test_*.py'"""
import copy
import json
import os
import sys
import tempfile
import unittest

MACOS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, MACOS)
import gen_tables  # noqa: E402

MAPPING = os.path.join(MACOS, 'steam-27.0.0.2486716.json')


class GenTablesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(MAPPING) as file:
            cls.mapping = json.load(file)

    def generate(self, *mappings):
        with tempfile.TemporaryDirectory() as folder:
            sources = []
            for index, mapping in enumerate(mappings):
                path = os.path.join(folder, f'steam-27.{index}.json')
                with open(path, 'w') as file:
                    json.dump(mapping, file)
                sources.append(path)
            target = os.path.join(folder, 'tables.c')
            gen_tables.main(sources, target)
            with open(os.path.join(folder, 'sites_fm27.h')) as file:
                return file.read()

    def changed(self):
        return copy.deepcopy(self.mapping)

    def test_writes_the_named_synthetic_sites(self):
        sites = self.generate(self.mapping)
        self.assertIn('SITE27_TACTIC_PLAN_2 = 0x7E000002,', sites)
        self.assertIn('SITE27_TACTIC_SLOT_OOP_LAST = 0x7E00002A,', sites)
        self.assertNotIn('0x7E001000', sites)

    def test_refuses_a_synthetic_id_the_sites_do_not_give(self):
        mapping = self.changed()
        mapping['slices']['arm64']['frames'][0]['win'] = ['0x7E0FFFFF']
        with self.assertRaisesRegex(ValueError, 'which `sites` does not give'):
            self.generate(mapping)

    def test_refuses_a_grandparent_without_a_parent(self):
        mapping = self.changed()
        frame = next(f for f in mapping['slices']['x86_64']['frames'] if 'parent' not in f)
        frame['grandparent'] = frame['mac']
        with self.assertRaisesRegex(ValueError, 'grandparent without a parent'):
            self.generate(mapping)

    def test_refuses_a_site_left_out_without_a_reason(self):
        mapping = self.changed()
        mapping['absent'] = [a for a in mapping['absent'] if a.get('name') != 'SITE_TEAM_STAFF_1']
        with self.assertRaisesRegex(ValueError, 'leaves out SITE_TEAM_STAFF_1'):
            self.generate(mapping)
        mapping = self.changed()
        next(a for a in mapping['absent'] if a.get('name') == 'SITE_TEAM_STAFF_1')['reason'] = ''
        with self.assertRaisesRegex(ValueError, 'SITE_TEAM_STAFF_1 is listed absent'):
            self.generate(mapping)

    def test_refuses_a_site_listed_absent_that_is_mapped(self):
        mapping = self.changed()
        mapping['absent'].append({'name': 'SITE_TEAM_STAFF_2', 'reason': 'mapped after all'})
        with self.assertRaisesRegex(ValueError, 'SITE_TEAM_STAFF_2 is listed absent'):
            self.generate(mapping)

    def test_refuses_two_mappings_giving_a_name_two_ids(self):
        other = self.changed()
        other['game_build'] += 1
        for name in ('arm64', 'x86_64'):
            other['slices'][name]['uuid'] = '00000000-0000-0000-0000-00000000000' + str(len(name) % 10)
        self.generate(self.mapping, other)
        next(s for s in other['sites'] if s.get('name') == 'SITE27_TACTIC_PLAN_2')['id'] = '0x7E000004'
        with self.assertRaisesRegex(ValueError, 'SITE27_TACTIC_PLAN_2 is not the ID'):
            self.generate(self.mapping, other)


if __name__ == '__main__':
    unittest.main()
