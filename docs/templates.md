# Code templates

Some games load code into an area that each image links to other addresses:
Metal Gear Solid 3 keeps every stage's code in `/MGS/STAGE.DAT`, prelinked for
0x252180, and each stage places the same modules (actors, the camera, menus)
in another order. Captured the usual way, as images of a code window, every
stage is a new image: about 20 MB of WebAssembly each, for 152 stages.

With its address fields masked (j/jal targets, the immediates of lui, addi(u),
ori, daddi(u) and loads/stores) that code is the same few modules over and
over: 1.6 M distinct words in shared code for all 152 stages instead of 77 M.
Templates translate each of those once.

## Building them

`eetemplates <load address> <out dir> <image>...` takes every image of the
area. It cuts each into pieces that end after `jr ra` and its delay slot,
takes pieces equal once masked as one piece, and chains pieces that always
follow one another into a template. For every template it writes `T<n>.bin`
(the words of its first place) and `T<n>.txt`:

| line | meaning |
| --- | --- |
| `size <bytes>` | its size |
| `piece <offset> <words> <hash>` | a piece, found at run time by this hash of its masked words |
| `field <offset>` | an immediate that differs between its places: read from memory when it runs |
| `dyn <offset>` | a j/jal whose target differs: read from memory when it runs |
| `int <offset>` | a j/jal into the template itself, rewritten to the template's own base |
| `weak <offset>` | a piece start, or where a word of some image points: code if it decodes as code |
| `at <image> <address>` | a place it occupies |

For MGS2 and MGS3, `scripts/mgs_templates.sh <name>` does it all: `mgs_stages`
decrypts and inflates the stage images from the disc (the cipher is described
at the top of `recomp/mgs_stages.cpp`; MGS2's stages load at 0x1EF600) and
`eetemplates` builds `games/<name>/templates`. `port.sh` passes the folder to
the recompiler when it exists.

## Translating them

`eerecomp --templates <dir>` translates each template as a relocatable module
placed at `kTemplateBase` (0x08000000): its addresses are `B + offset`, where
`B` is set by whoever enters it. Fields and dynamic jumps read the instruction
word from memory, internal jumps are direct calls, and a j/jal leaving the
template keeps its absolute target. Every code word becomes a check; one with
bit 0 of its offset set is compared under the mask, as its address field
differs between places.

## Finding them

When dispatch has no translated code at a pc inside the area, it looks for
the piece around it: back to the word after the previous `jr ra` and its delay
slot, forward past the next ones. It hashes the masked words, takes the
template pieces with that hash and the same length, and serves the first whose
checks hold over the piece. The instance (template and base) is cached by
where it starts and spot checked on later calls, like a window image. A pc no
template serves is remembered for 120 frames, so the long lookup is not
repeated each call.

`PS2_NO_TEMPLATES` turns templates off. `PS2_TEMPLATE_STATS` prints how
dispatch used them at exit; set to an absolute path, it also lists the pcs
found without an entry and those no template served.
