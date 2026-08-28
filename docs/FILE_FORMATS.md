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
temporary model; failure leaves the live document unchanged. Saving encodes to
memory, writes a temporary sibling, flushes it, and atomically replaces the
destination. Selection flags are editor state and are canonically written as
zero, matching the recovered writer.

Some recovered files contain harmless, historically uninitialized root or
paired-face fields. The decoder accepts only the evidence-backed cases,
canonicalizes them, and reports a warning. It does not use a permissive
byte-offset fallback.

## Animated documents

Animation tags are decoded and re-encoded even though the animation editor is
not part of the static-editor release. Populated frames must form one global,
contiguous prefix; every animated face owns a distinct index and an exact,
finite point chain for each frame. Coordinate transforms are applied to the
matching points in every frame. Topology-changing commands stay disabled until
the user creates an unnamed static copy, preventing accidental animation loss.

This release's animation guarantee applies to tags 3/4 embedded in the binary
X11 CAD stream. The recovered standalone textual `3DAN` and `3DGI` `.anm`
project formats are identified but not imported or exported yet.
