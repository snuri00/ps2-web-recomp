# PS2 Web Recomp

PlayStation 2 games running in the browser through static recompilation. The game's Emotion Engine machine code is translated ahead of time into C++ and compiled to WebAssembly, so the main CPU is never interpreted; the rest of the console, its operating system and hardware, is emulated by a runtime that draws with WebGPU.

<p align="center">
  <img src="docs/media/demo.webp" alt="Gran Turismo 4 running in a browser" width="720">
  <br>
  <a href="docs/media/demo.mp4">Watch the full recording (1:24)</a>
</p>

This is the sibling of [psp-web-recomp](https://github.com/snuri00/psp-web-recomp) and follows the same method. The first title brought up this way is Gran Turismo 4, chosen because it is one of the most demanding games on the system: it leans on both vector units, on exact floating point behaviour for its replays and license tests, and on graphics tricks that read rendered images back as palettes. It plays from boot through the opening movie, the menus and races, with music and sound effects, and saves to a memory card kept in the browser. A race runs at full speed, 50 frames per second for this PAL release, in Chrome on a laptop's integrated GPU at twice the PS2's resolution, which is how the recording above was made.

God of War II followed through the same scripts. It needed no game-specific code, but it found two bugs that Gran Turismo 4 had never triggered, one in the recompiler and one in the vector unit timing, and a few missing pieces of the PS2's input/output processor. It now plays from the menus through its cutscenes into combat, and holds 60 frames per second in heavy combat at the PS2's resolution in Chrome. At twice the resolution it runs at 48 to 60, limited by the laptop's integrated GPU.

No game data is included here. You bring a disc image of a game you own, and the scripts in this repository turn it into a web page on your machine.

## How it works

**Recompilation.** `eerecomp` reads the game's executable, follows its control flow to find the functions and emits C++ for them in units of 16 KiB of guest code. The output keeps the R5900's quirks: 128-bit registers and the multimedia instructions that work on them, branch delay slots, the PS2's floating point unit, which has no infinities or denormals and rounds differently from IEEE, and vector unit 0 used as a coprocessor. Jumps between units become tail calls, and a thread switch unwinds the host stack and resumes at the guest address, so the generated code never needs a stack per guest thread.

**A small PS2 kernel.** The EE side of the operating system is answered by high-level emulation: threads, semaphores, interrupt handlers for the DMA controller and the vertical blank, alarms, and the SIF channel that connects the main CPU to the I/O processor. The I/O processor runs the game's own IRX modules, the sound driver, the disc streaming code and the controller and memory card drivers that ship on the disc, on an interpreter of its R3000 CPU, while the IOP kernel and the libraries those modules call are reimplemented in C++. The disc is read over HTTP Range requests, so only the executable is downloaded up front.

**Vector units.** Most of a PS2 game's 3D work happens in microprograms the game uploads to vector unit 1. They run with the pipeline delays of the MAC, status and clip flags modelled, because games schedule their code around them, and with the PS2's float rules for special values: no infinities or NaNs, denormals flushed to zero and overflow clamped to the largest float. The arithmetic itself is the host's, so a result can differ from the console's in its last bit; the console truncates where the host rounds, and its multiplier is not exact. Microprograms seen during a test run are compiled ahead of time into SIMD C++; any the page meets for the first time are translated into a WebAssembly module on the spot, which takes a few milliseconds per program.

**Graphics.** The Graphics Synthesizer's 4 MB of video memory lives on the GPU, in its native swizzled layouts, and drawing is done by WebGPU compute shaders: triangles are binned into tiles and rasterized with the GS's own rules for texturing, palettes, alpha blending, depth and destination alpha tests. Because everything stays in GS memory, effects that render into a texture or reinterpret a framebuffer as a palette work the way they do on the console. The internal resolution can be one, two or three times the native one. A software rasterizer of the same GS remains as the reference the GPU output is compared against.

**Threads.** The emulation runs in a worker, vector unit 1 has a worker of its own the way it runs in parallel on the console, and the GS commands go through a ring buffer to the page thread, which owns the WebGPU device.

**Sound and movies.** The SPU2 is reimplemented with its 48 voices of ADPCM, ADSR envelopes and the streaming DMA games use for music, and its output goes to an AudioWorklet through shared memory. Movies are decoded by an implementation of the IPU, the PS2's MPEG-2 decoder.

## Lessons from making it fast

The first race ran at 3 frames per second natively, with a software rasterizer and an interpreter for vector unit 1. Moving the GS to WebGPU compute shaders and compiling the vector unit programs brought it to about 70 natively, and most of the remaining work was about where the browser spent its time rather than the emulation.

The first version of the GPU renderer submitted around 650 compute dispatches per frame, because every texture upload and palette load split the batch of triangles around it. Hoisting uploads ahead of the triangles that do not depend on them, with the same output, brought that to about 80.

Gran Turismo 4 uploads the same 3 MB of textures every frame of a race. GS memory is swapped between frames, so the uploads cannot simply be skipped, but a 16 MB cache of recently uploaded contents on the GPU lets most of them be served without crossing from WebAssembly to the GPU again, which cut the data sent per frame by about 60 percent.

At three times the native resolution the frame rate dropped to 23. Profiling the shader showed that the per-triangle setup was being loaded after a branch that differed between threads, which made the whole setup divergent, and that uploads split into 1 MB pieces blocked Chrome's page thread. Loading the setup uniformly and writing each frame's data in one piece brought three times the resolution to 48 to 50 frames per second on the same integrated GPU.

The browser's own scheduling mattered too. Nested `setTimeout` calls are clamped to 4 ms, so the emulation loop now yields through a `MessageChannel` instead, and moving the emulation into a worker, so that the page thread only feeds the GPU, steadied a race that had wandered between 40 and 50 frames per second at a constant 50.

## A second game

God of War II went from an ISO to a page through `port.sh` with only generic changes, and each one was a bug or gap that the first game had simply not exercised.

It started with silence. The game's sound driver drives its tick from one of the I/O processor's hardware timers, which nothing had implemented, so the driver never mixed a sample. The intro after the difficulty menu was a mess of huge flat polygons, and when the gameplay was about to begin the game crashed after its render list ran past the end of memory and overwrote its own vertical blank handlers. Following a corrupted linked list back with hardware watchpoints led to the recompiler: when a branch was the last instruction of a 16 KiB unit, the path that does not take the branch continued in the next unit at the delay slot instead of after it, running that instruction twice, or running the instruction a branch-likely should have skipped. That fixed the crash and the game reached combat, still with giant polygons over parts of the screen. Those came from triangles that should have been culled. God of War II's microcode reads the clip flags three instructions after the `CLIP` that sets them, and the vector unit model made them visible only after four, which Gran Turismo 4, whose microcode always waits longer, could not have noticed.

Along the way it also needed a boot path like the BIOS gives, a module loaded from memory instead of the disc, and the scratchpad DMA's destination chain mode.

## Port a game of your own

You need git, CMake, Ninja, a C++20 compiler (clang is preferred and picked up automatically), Python 3, and 7-Zip if your disc image is in a `.7z`. Everything has been run on Linux on x86-64, and the scripts also build on macOS on Apple silicon, where the scripts need a newer bash than the system 3.2 (`brew install bash cmake ninja sevenzip`) and the native runner links wgpu-native against Metal. The first run installs the Emscripten SDK into `tools/emsdk`, or uses the one `EMSDK` points to, and downloads wgpu-native for the native runner. With `--native-only` the script skips the browser build and never installs the SDK; the native window then needs the SDL2 development files. Expect about 300 MB of disk space per game for the generated code and the builds; the disc image itself is linked, not copied.

```bash
git clone https://github.com/snuri00/ps2-web-recomp.git
cd ps2-web-recomp
scripts/port.sh mygame "My Game.iso"
python3 scripts/serve.py build/web-mygame games/mygame/disc.iso    # http://localhost:8620/
```

`port.sh` takes a disc image (an ISO, or a ZIP or 7z archive holding one) and does the whole job. It reads the boot program named in the disc's `SYSTEM.CNF`, translates its machine code to C++, and then runs the game headless a few times. Static analysis cannot see every function a game reaches through pointers, so each run writes down the addresses it could not find, the script adds them as entry points and translates again, until a run gets through without new ones. The same runs record the vector unit microprograms the game uploads, which are compiled ahead of time as well. Last it builds the page into `build/web-mygame` and the native runner into `build/native-mygame`. God of War II goes from ISO to page in three and a half minutes on an 8-core laptop.

The exploring runs press start and then cross every five seconds, which gets most games through their title screens, and put an empty memory card in slot 1 the way the page does, so they follow the path a player takes. `--press 900:cross` replaces that with your own inputs and `--frames` sets how long each run lasts. Frames from the last run land in `games/mygame/shots`, which is the quickest way to see how far a new game gets. `scripts/build_web.sh mygame` rebuilds only the page.

Set your expectations accordingly: two games have been brought up so far. Another game will most likely stop at a system call or an IOP library function nobody implemented yet, which the runner logs as `[sys] unhandled syscall` or `[iop] missing <library> #<n>`, or use a GS feature the renderer does not handle yet.

## Browsers

The page needs WebGPU for the GPU renderer. Chrome and Edge have it on Windows and macOS. On Linux it is off by default in Chrome and Chromium: turn on `chrome://flags/#enable-unsafe-webgpu` and `chrome://flags/#enable-vulkan` and restart the browser, as the page tells you when it finds no usable adapter. Without WebGPU the software renderer takes over, which is fine for menus and far too slow for 3D. On laptops with two GPUs, a canvas drawn on the discrete GPU sometimes cannot be shown on the screen; the page tries both adapters and falls back to copying frames through a 2D canvas.

The keyboard maps the face buttons to Z (cross), X (circle), A (square) and S (triangle), the shoulders to Q, W, 1 and 2, start and select to Enter and Backspace, the D-pad to the arrow keys and the left stick to I, J, K and L. Gamepads work through the browser's gamepad API. Their analog triggers reach the game as the DualShock 2's pressure-sensitive L2 and R2, and controllers with motors vibrate when the game asks for it, in browsers that expose vibration (Chrome and Edge do). Phones get on-screen controls, and F toggles fullscreen. The resolution selector reloads the page at the new size.

## Playing natively

The native runner, `build/native-mygame/ps2run`, plays the game in a window when SDL2 is installed; `scripts/port.sh mygame "My Game.iso" --native-only` builds only this one. It draws through wgpu-native on Vulkan and plays sound through SDL. The keyboard uses the page's keys for the buttons, with the arrow keys as both the D-pad and the left stick, and any game controller SDL recognizes works too, the Steam Deck's own controls included, with the same trigger pressure and vibration as in the browser.

```bash
build/native-mygame/ps2run games/mygame/disc/SLUS_123.45 --disc games/mygame/disc.iso --window --memcard mygame.ps2
```

`--fullscreen` starts in fullscreen and F11 switches back and forth. `PS2_SCALE=2` or `3` raises the internal resolution. As in the browser, the emulation, vector unit 1 and the GS each run on their own thread, which takes God of War II from 43 to 60 frames per second at the native resolution on the test laptop; `PS2_ONE_THREAD=1` puts everything back on one thread. The memory card file uses the same layout as the page's exported cards. On a Steam Deck the runner can be added to Steam as a non-Steam game, though nobody has tried it on one yet.

## Saves

The page puts a memory card in slot 1 and keeps it in the browser's storage for the address you play from, so progress survives a reload. Games format and use it as they would a real card. **Export card** downloads it as a `.ps2` file in the layout PCSX2 uses, and **Import card** puts such a file back in the slot, so a card can travel between browsers or come from PCSX2. `?nocard` starts with the slot empty.

## Hosting

The page uses SharedArrayBuffer for its threads, so it has to be served cross-origin isolated, with `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`. `scripts/serve.py` sends both headers and supports the Range requests the disc streaming relies on. Anyone who can reach the server can load the game, so keep it to yourself.

## Legal

This repository contains only original code and third-party code under its own license. It contains no game code or data. The generated C++ and the built WebAssembly are translations of the game's executable, so they belong to the game's owners: keep them on your own machine and do not publish them. Use disc images of games you own. Gran Turismo, God of War and PlayStation are trademarks of Sony Interactive Entertainment; this project is not affiliated with or endorsed by Sony, Polyphony Digital, Santa Monica Studio or any game publisher.

## Credits

The VLC tables of the MPEG decoder are adapted from [Play!](https://github.com/jpd002/Play-) (BSD 2-clause, notice in `runtime/src/ipu/NOTICE`). PCSX2, Play! and the ps2sdk headers were the references for much of the hardware and system behavior emulated here. wgpu-native and Dawn provide WebGPU natively and in the browser, and Emscripten builds the WebAssembly.

## Experimental GT3 bring-up

See [the GT3 bring-up notes](docs/gt3.md) for the focused runtime/tooling changes,
synthetic regression tests, snapshot compatibility and current validation limits.
This is not a claim of complete GT3 compatibility.

## License

The code in this repository is available under the MIT License, see [LICENSE](LICENSE). `runtime/src/ipu/tables.hpp` keeps the BSD 2-clause notice of Play!.
