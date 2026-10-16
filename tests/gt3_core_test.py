"""Game-free CORE wrapper/conversion and port-script selection regressions."""
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('core', ROOT / 'scripts/gt4_core.py')
CORE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CORE)


class CoreTests(unittest.TestCase):
    def test_gt3_and_gt4_synthetic_containers(self):
        # Metadata and payloads are invented; no game executable is embedded.
        parts = [(0x10000, b'synthetic-first'), (0x12000, b'synthetic-second')]
        image = b''
        for name in (b'synthetic', b'fixture'):
            image += struct.pack('<H', len(name)) + name
        image += struct.pack('<iI', len(parts), 0x10000)
        for address, data in parts:
            image += struct.pack('<II', address, len(data)) + data
        compressor = zlib.compressobj(wbits=-15)
        packed = struct.pack('<HI', 0, len(image)) + compressor.compress(image) + compressor.flush()
        self.assertEqual(CORE.sections(CORE.unpack(packed)), (0x10000, parts))
        for name in ('CORE.GT3', 'CORE.GT4'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                src, out = Path(tmp) / name, Path(tmp) / 'synthetic.elf'
                src.write_bytes(packed)
                subprocess.run([sys.executable, '-I', str(ROOT / 'scripts/gt4_core.py'), str(src), str(out)],
                               check=True, capture_output=True, text=True)
                elf = out.read_bytes()
                self.assertEqual(elf[:7], b'\x7fELF\x01\x01\x01')
                self.assertEqual(struct.unpack_from('<I', elf, 24)[0], 0x10000)
                self.assertEqual(struct.unpack_from('<I', elf, 36)[0], CORE.R5900_FLAGS)
                self.assertEqual(struct.unpack_from('<H', elf, 44)[0], len(parts))
                for i, (address, data) in enumerate(parts):
                    header = struct.unpack_from('<8I', elf, 52 + i * 32)
                    self.assertEqual(header[0], 1)
                    self.assertEqual(header[2:6], (address, address, len(data), len(data)))
                    self.assertEqual(elf[header[1]:header[1] + header[4]], data)

    def selection(self, listing, cached=False):
        # Exercise the actual executable-selection block, not a copy of its awk.
        script = (ROOT / 'scripts/port.sh').read_text()
        stage = script.split('echo "== [2/6] executable"', 1)[1].split('echo "${elf#$ROOT/}"', 1)[0]
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp) / 'game'
            (game / 'disc').mkdir(parents=True)
            if cached:
                (game / 'core.elf').write_bytes(b'synthetic-cache')
            listing_file = Path(tmp) / 'listing'
            listing_file.write_text(listing)
            calls = Path(tmp) / 'calls'
            env = dict(os.environ, ROOT=str(ROOT), here=str(ROOT / 'scripts'), game=str(game),
                       iso='unused-synthetic.iso', program='BOOT.ELF', LISTING_FILE=str(listing_file), CALLS=str(calls))
            mock = r'''
python3() {
  printf '%s\n' "$*" >> "$CALLS"
  if [[ "$2" == "$here/disc.py" ]]; then
    if [[ "$3" == list ]]; then
      while IFS= read -r line; do printf '%s\n' "$line"; done < "$LISTING_FILE"
    elif [[ "$3" == extract ]]; then
      touch "$5/$6"
    else return 1; fi
  elif [[ "$2" == "$here/gt4_core.py" ]]; then
    touch "$4"
  else return 1; fi
}
'''
            result = subprocess.run(['bash', '-euo', 'pipefail', '-c', mock + stage + '\nprintf "%s\\n" "$elf"'],
                                    env=env, text=True, capture_output=True, check=True)
            return Path(result.stdout.strip()).name, calls.read_text() if calls.exists() else ''

    def test_gt3_selection(self):
        selected, calls = self.selection('1 2 3 CORE.GT3\n')
        self.assertEqual(selected, 'core.elf')
        self.assertIn('extract', calls)
        self.assertIn('CORE.GT3', calls)

    def test_gt4_selection(self):
        selected, calls = self.selection('1 2 3 CORE.GT4\n')
        self.assertEqual(selected, 'core.elf')
        self.assertIn('CORE.GT4', calls)

    def test_normal_boot_fallback(self):
        selected, calls = self.selection('1 2 3 OTHER.DAT\n')
        self.assertEqual(selected, 'BOOT.ELF')
        self.assertNotIn('extract', calls)

    def test_cached_core_precedence(self):
        selected, calls = self.selection('1 2 3 CORE.GT3\n', cached=True)
        self.assertEqual(selected, 'core.elf')
        self.assertEqual(calls, '')

    def test_selection_consumes_long_listing(self):
        # Early awk exit would SIGPIPE a long producer under set -o pipefail.
        selected, _ = self.selection('1 2 3 CORE.GT3\n' + '1 2 3 SYNTHETIC.DAT\n' * 10000)
        self.assertEqual(selected, 'core.elf')


if __name__ == '__main__':
    unittest.main()
