# Gran Turismo 3 bring-up (experimental)

This work builds on **snuri00/ps2-web-recomp**, retaining its history, MIT
license and attribution. It contributes runtime/tooling prerequisites identified
while bringing up Gran Turismo 3; it is not a separate graphics implementation
or a claim of complete GT3 compatibility.

## Scope

- Recognize `CORE.GT3` as well as `CORE.GT4` in the port pipeline, using the
  existing CORE-to-ELF converter. Cached `core.elf` and ordinary BOOT2 executable
  fallback behavior are preserved. Selection consumes the full directory listing
  to avoid producer SIGPIPE under `set -o pipefail`.
- Read SIF EE descriptor words through the memory resolver, including scratchpad.
  This does not implement scratchpad DMA payload sources.
- Enter idle and unwind when `ExitDeleteThread` leaves no runnable thread.
- Minimal local i.Link identification, serial-clock-ready and PHY-access
  completion/register storage, completion status/interrupt and write-one-to-clear
  behavior. This is sufficient to exercise initialization with no attached peer;
  it is **not** complete IEEE 1394 hardware, packet transfer or GT3 network play.
- Regression coverage for the existing upstream guest-visible INTC latch. Current
  upstream already retains status after HLE handler delivery; that behavior is
  preserved, not replaced. The retained latch is additionally saved/restored.
- Zero-length snapshot reader/writer guards for empty container data pointers.

No projection/FOV patch, independent render scaling, camera override, SIMD
optimization, benchmark harness or LAN streaming code is included. Stock
rendering, controls and launch defaults otherwise remain in place.

## Snapshot compatibility

Runtime snapshot **version 3** adds the retained INTC latch and i.Link PHY
registers. Version 2 snapshots are deliberately rejected at the header gate,
before CPU/RAM mutation; no migration is implemented. Preserve old snapshots
and create new ones with this build. This does not change or delete memory-card
`.ps2` files, disc images or previous builds. Snapshot formats from separate
experimental forks should not be assumed interchangeable even if their version
numbers happen to match.

## Validation and limits

Five native CTest groups pass on Linux x86-64, Clang 23.1.1, in Release and
Debug with ASan/UBSan (leak detection and halt-on-UB enabled):

1. SIF scratchpad descriptor list, conflicting RAM descriptors and five RAM aliases.
2. Deleted-thread idle/unwind, dormant-thread and ready-worker control cases.
3. INTC polling/delivery/W1C, latch restoration, local i.Link initialization,
   masked/enabled completion IRQs, PHY register restoration and reset.
4. Full **synthetic** runtime snapshot round-trip, version gate and zero-length
   serialization. Generated temporary files contain no game data and are removed.
5. Synthetic GT3/GT4 CORE conversion, actual port-script selection, cached-core
   precedence, normal-executable fallback and a long directory listing.

Tests added to unmodified upstream fail in the corresponding missing behaviors.
The existing live INTC polling behavior already passes upstream; its **snapshot
retention** is the new fix. Full snapshot sanitizer testing also exposed the
previous zero-length/null `memcpy` issue.

Earlier private native experiments reached GT3 USA v1.10 boot, menus and driving,
including camera and save/reload checks. Those experiments contained additional
changes deliberately excluded here. **This clean branch has not been freshly
validated end-to-end with GT3 game data, in the browser, or against GT4 gameplay.**
Other revisions/regions and universal black-screen fixes are not claimed.
GCC is currently blocked by pre-existing Clang-specific vector builtins upstream.

## Reproduce the synthetic tests

No game, generated translation, SDL window or GPU setup is needed in a fresh
clone. Tests default OFF and are native-only. Python 3 and Bash are needed for
CORE/tooling tests; CMake registers that group when Python is available.

```bash
cmake -S . -B build/gt3-tests -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DPS2_BUILD_TESTS=ON
cmake --build build/gt3-tests --target \
  sif_scratchpad_test kernel_exit_delete_thread_test gt3_initialization_test gt3_state_test
ctest --test-dir build/gt3-tests --output-on-failure
```

## Try with a disc you own

Follow the repository's existing toolchain setup instructions, then:

```bash
bash scripts/port.sh gt3 /path/to/your/gt3.iso --native-only
build/native-gt3/ps2run games/gt3/core.elf --disc games/gt3/disc.iso --ntsc --window
# Browser build (requires the documented Emscripten setup):
bash scripts/port.sh gt3 /path/to/your/gt3.iso
python3 scripts/serve.py build/web-gt3 games/gt3/disc.iso
```

The window command requires an SDL-enabled native build; omit `--window` for
headless diagnostics. Use the appropriate region's video standard, rather than
forcing NTSC for a PAL disc. Exploration reaching its frame limit is not proof
that boot or rendering succeeded: inspect the exploration log and frames.
These are reproduction entry points, not guarantees of a complete port.

Keep disc contents, CORE/ELF files, generated game C++, built game libraries and
binaries, snapshots, memory dumps, game-derived assembly/decompilation and
captures local. None are shipped here. Prefer sanitized runtime diagnostics
rather than posting game material in issues or PRs.
