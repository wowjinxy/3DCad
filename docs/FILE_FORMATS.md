# Recovered 3Ddraw file formats

3DCad deliberately keeps the recovered formats as its native interchange. The
codec reads and writes fields explicitly; it never serializes the host C
structures or assumes the host byte order, alignment, or `double` layout.

## Later X11 record stream

Each record begins with a one-byte tag and a big-endian unsigned 16-bit table
index. Payloads use the recovered SGI/NEWS layout:

| Tag | Record | Payload bytes |
| --- | --- | ---: |
| 0 | Object | 40 |
| 1 | Polygon | 14 |
| 2 | Point | 32 |
| 3 | Animation index (64 frames) | 130 |
| 4 | Animation point | 32 |

Integers and IEEE-754 doubles are big-endian. Padding bytes are consumed and
written explicitly. New documents and converted imports are saved in this
format. The supported historical limits are 256 objects, 1,024 polygons,
1,024 static points, 256 animation indices, 8,192 animation points, 64 frames,
and 16 vertices per face.

## Legacy packed stream

The earlier stream uses a big-endian 16-bit key instead of the three-byte
header. Its top two bits identify the record type and the lower 14 bits are the
table index:

| Key bits | Record | Payload bytes |
| --- | --- | ---: |
| `00` | Object | 40 |
| `01` | Polygon | 8 |
| `10` | Point | 32 |

Legacy polygons predate the animation, paired-side, and side fields. On import,
3DCad initializes `animation` and `both` to `-1` and reconstructs the side flag
from the recovered normal-sign rule. The source is treated as an import: it is
never silently overwritten, and Save opens a Save As flow for the later X11
stream.

## Validation and safety

Format detection requires a candidate stream to reach exact end-of-file and
pass index, count, acyclic reciprocal object-hierarchy, polygon-chain,
point-chain, paired-face, and animation validation. Decoding occurs into a
temporary model; failure leaves the live document unchanged. Saving writes a
temporary sibling, flushes it, and atomically replaces the destination. An
unchanged native document reuses its retained validated source bytes exactly.
After an edit, saving encodes to memory; selection flags are editor state and
are then canonically written as zero, matching the recovered writer.

Some recovered files contain harmless, historically uninitialized root or
paired-face fields. The decoder accepts only the evidence-backed cases,
canonicalizes them, and reports a warning. It does not use a permissive
byte-offset fallback.

## Animated native documents

Populated frames form one global contiguous prefix. Every animated face owns a
distinct index and one exact, finite point chain per frame; unattached records
are reported separately and preserved. The editor maps stable static point IDs
to animation points by polygon-chain ordinal. Once an animation is changed,
frame 0 is synchronized to the static base coordinates.

Animation is fixed-topology morphing. Chain-changing commands are disabled
while any animation records exist. Current-frame edits are the default; All
Frames applies the same affine transform and displayed pivot to corresponding
points in every frame. Preview interpolation is evaluated into an immutable
pose and is never serialized. Baking creates an unnamed animation-free copy
from the exact displayed pose.

## Standalone 3DAN and 3DGI animation text

Both headers use the same recovered grammar:

1. `3DAN` or `3DGI` header
2. global point-track count
3. frame count (1–64)
4. `point count × frame count` signed integer XYZ triples, frame-major
5. one or more faces: point count, point-track indices, then color

Faces contain 2–16 points and colors are 0–255. The decoder accepts LF or CRLF
and an optional terminal DOS `0x1A`, rejects overflow/truncation/trailing data,
and transactionally expands global tracks into native per-face static and
animation chains. Frame 0 supplies static geometry. Reciprocal sides are
reconstructed from matching point-index sets, and side bits use the recovered
normal rule: `normalY < 0` sets bit 0, `normalZ >= 0` sets bit 1, and
`normalX < 0` sets bit 2.

Encoding defaults to `3DAN`, writes deterministic CRLF text plus DOS EOF, and
uses recovered half-away-from-zero coordinate rounding. Tracks are deduplicated
only when their rounded coordinates match in every frame. Static faces repeat
their base coordinates across the exported frame range. Quantization and the
recovered game-coordinate range of -127…127 are reported as warnings before
the caller writes the buffer.

## COL color tables

A `.COL` file is a 256-entry SNES color table. Its canonical encoded size is
0x200 bytes. Each entry is one little-endian BGR555 word:

| Bits | Meaning |
| --- | --- |
| 0–4 | Red, 0–31 |
| 5–9 | Green, 0–31 |
| 10–14 | Blue, 0–31 |
| 15 | Uninterpreted retained bit |

The display conversion expands the five-bit channels to opaque eight-bit RGB;
bit 15 does not affect the preview but is preserved by edits and round trips.
The decoder consumes the historical first 0x200 bytes. It accepts a larger
buffer with a diagnostic that trailing bytes were ignored; encoding always
writes the canonical 0x200 bytes.

## PAL material maps

A `.PAL` file is not another color table. It is an exact 0x8200-byte material
map that refers to entries in an associated `.COL` table:

| Offset | Size | Contents |
| ---: | ---: | --- |
| 0x0000 | 0x0200 | 256 two-byte material descriptors |
| 0x0200 | 0x8000 | 256 material records × 128 raw `.COL` indices |

Descriptor `i` occupies bytes `i * 2` and `i * 2 + 1`. The first byte is the
material type. In the second byte, the high nibble stores palette number minus
one and the low nibble stores color count minus one, so both decoded values are
in the range 1–16. The 128 sample indices for material `i` begin at
`0x0200 + i * 0x80`.

Recovered material types are:

| Value | Type |
| ---: | --- |
| 0 | Normal |
| 1 | Depth Cue |
| 2 | Light Source |
| 3 | Light Depth |
| 4 | Animation |
| 5 | Texture Map |

Unknown type values are retained and reported rather than discarded. The
runtime meaning and timing/axis of the 128 entries is not yet established, so
the editor deliberately presents them as numbered samples. New `.PAL` files
use the recovered default descriptor—Light Depth, palette 4, 16 colors—and
initialize all sample indices to zero.

For preview, a face or two-point line's 0–255 color index selects a material
record. The currently displayed sample supplies a raw `.COL` index, which is
then converted from BGR555. Without a loaded `.PAL`, the geometry index selects
the `.COL` entry directly; without a `.COL`, the material/sample values remain
editable and are visualized as indices rather than invented colors.

Both palette codecs are buffer-based and validate into temporary resources
before replacing live state. The document keeps native CAD, `.COL`, and `.PAL`
source/save paths and dirty revisions independently. All three use the shared
bounded UTF-8 and atomic-save platform services, and none may silently
overwrite an associated resource of the other format.
