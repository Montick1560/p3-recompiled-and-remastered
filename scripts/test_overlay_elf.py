#!/usr/bin/env python3
"""Tests for scripts/overlay_elf.py placeholder selection.

Run: python -I scripts/test_overlay_elf.py
"""
import importlib.util
import os
import sys
import unittest

_spec = importlib.util.spec_from_file_location(
    "overlay_elf", os.path.join(os.path.dirname(os.path.abspath(__file__)), "overlay_elf.py"))
overlay_elf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(overlay_elf)

WINDOW = 0x08ABB180
# Patapon 3 EBOOT: PT_LOAD main, then PT_NULL placeholders in this order.
PHDRS = [
    (1, 0x08804000, 0x3C2480),
    (0, WINDOW, 0x23480),   # OL_Azito
    (0, WINDOW, 0x10B300),  # OL_Mission
    (0, WINDOW, 0xDDD80),   # OL_Title
]


class PickPlaceholder(unittest.TestCase):
    def test_each_overlay_gets_its_own_header(self):
        # (file length + bss) as computed from each overlay's MWo3 header
        self.assertEqual(overlay_elf.pick_placeholder(PHDRS, WINDOW, 0x23400 + 0x80), 1)
        self.assertEqual(overlay_elf.pick_placeholder(PHDRS, WINDOW, 0x10A900 + 0xA00), 2)
        self.assertEqual(overlay_elf.pick_placeholder(PHDRS, WINDOW, 0xDA180 + 0x3C00), 3)

    def test_no_placeholder_large_enough(self):
        self.assertIsNone(overlay_elf.pick_placeholder(PHDRS, WINDOW, 0x200000))


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
