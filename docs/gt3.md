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

Runtime snapshot **version 7** adds the retained INTC latch and i.Link PHY
registers on top of upstream version 6. Upstream versions 2–6 remain loadable;
fields absent from those layouts reset to zero. Versions 0, 1 and newer than 7
are rejected at the header gate before CPU/RAM mutation. Older builds cannot
load version 7 snapshots. Preserve old snapshots before upgrading. This does
not change or delete memory-card `.ps2` files, disc images or previous builds.
Snapshot formats from separate experimental forks (including the original
PR's version 3 layout) are not interchangeable merely because their version
numbers happen to match.

## Validation and limits

Rebased onto upstream `e026b97`. Five native CTest groups pass on Linux x86-64,
Clang 23.1.3, in Release and Debug with ASan/UBSan (leak detection and
halt-on-UB enabled). The existing tail-call discovery, executable BSS and
codegen fallthrough regressions also pass:

1. SIF scratchpad descriptor list, conflicting RAM descriptors and five RAM aliases.
2. Deleted-thread idle/unwind, dormant-thread and ready-worker control cases.
3. INTC polling/delivery/W1C, latch restoration, local i.Link initialization,
   masked/enabled completion IRQs, PHY register restoration and reset, and
   hardware-section compatibility/defaults for upstream snapshot versions 2–6.
4. Full **synthetic** runtime snapshot round-trip, version gate and zero-length
   serialization. Generated temporary files contain no game data and are removed.
5. Synthetic GT3/GT4 CORE conversion, actual port-script selection, cached-core
   precedence, normal-executable fallback and a long directory listing.

Tests added to unmodified upstream fail in the corresponding missing behaviors.
The existing live INTC polling behavior already passes upstream; its **snapshot
retention** is the new fix. Full snapshot sanitizer testing also exposed the
previous zero-length/null `memcpy` issue. A separate full synthetic snapshot
written by an unmodified upstream-v6 runtime also loads successfully here:
CPU, RAM and pending interrupts restore, while the newly added latch and PHY
storage reset to zero. This checks an actual old layout, not a new snapshot
with its version header relabeled.

Fresh native validation on this rebased branch, using a local GT3 USA v1.10
disc image and newly generated game code, reached boot, the title screen,
Arcade Single Race menus, track/car selection, countdown and driving through
4,800 frames. Captures were inspected: the car accelerates through 42, 67 and
85 mph, and R1 switches from bumper to chase camera while driving. A version 7 race snapshot saved at frame 4,200 resumes successfully;
frames 4,500 and 4,800 match the uninterrupted run byte-for-byte.

A fresh `port.sh --native-only` run from the original disc detects `CORE.GT3`,
translates the executable, explores 3,600 frames with zero missing entry points,
records/compiles 91 VU1 programs and produces the final native runner. That
runner also loads the frame-4,200 race snapshot and advances 600 frames;
its frame-4,500 and frame-4,800 captures match the uninterrupted run byte-for-byte.

Browser validation uses Chrome for Testing 156.0.8078.4 on Linux with an AMD
RDNA 4 WebGPU adapter and cross-origin isolation enabled. Both direct WebGPU
presentation and the existing `?copy` fallback reach boot, menus and driving
through 4,800 frames. Canvas captures pass nonblank-pixel checks and were
inspected visually. Both paths save/load a version 7 snapshot and advance at
least 120 more frames after loading.

For headless Chromium checks, use `--enable-unsafe-webgpu
--enable-features=Vulkan --use-angle=vulkan` without
`--disable-vulkan-surface`. The latter caused blank captures/timeouts even on a
standalone solid-color WebGPU probe; this was a harness issue, not a GT3 fix.

GT4 gameplay, other revisions/regions and universal black-screen fixes are not
claimed. GCC remains blocked by pre-existing Clang-specific vector builtins
upstream. These checks do not establish complete game compatibility.

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

For a repeatable native race/snapshot check after building, run the following
from Bash. This uses the USA disc's NTSC rate and stores private artifacts under
`build/gt3-check`; inspect captures as well as the exit status.

```bash
mkdir -p build/gt3-check/continuous build/gt3-check/resumed
presses=(--press 300:start)
for ((f = 600; f <= 3300; f += 300)); do presses+=(--press "$f:cross"); done
build/native-gt3/ps2run games/gt3/core.elf --disc games/gt3/disc.iso --ntsc \
  --memcard build/gt3-check/card.ps2 --frames 4800 "${presses[@]}" \
  --press 3600:cross:1200 --press 4000:select \
  --save-state 4200:build/gt3-check/race.state \
  --frames-dir build/gt3-check/continuous --frame-every 300
build/native-gt3/ps2run games/gt3/core.elf --disc games/gt3/disc.iso --ntsc \
  --load-state build/gt3-check/race.state --run 600 --press 4200:cross:600 \
  --frames-dir build/gt3-check/resumed --frame-every 300
for f in 04500 04800; do
  cmp "build/gt3-check/continuous/frame_$f.ppm" "build/gt3-check/resumed/frame_$f.ppm"
done
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
