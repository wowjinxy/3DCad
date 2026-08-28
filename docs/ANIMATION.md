# Animation authoring in 3DCad 0.3

3DCad edits the recovered per-vertex morph animation stored in native X11 CAD
tags 3 and 4. It does not add bones, curves, timing metadata, or per-frame
topology. The historical limits remain authoritative: 64 frames, 256 animated
faces, 8,192 animation points, and 16 vertices per face.

## Starting an animation

Open the Animation panel and choose **Create All** to attach every active face,
or select faces and choose **Create Sel**. The initial frame count is 16 unless
the remaining animation-point capacity supports fewer. Static faces can later
be attached with **Add Faces**.

Once any animation exists, commands that create, delete, reorder, pair, cut, or
otherwise change polygon/point chains are disabled. Their command state includes
the reason. Use **Make Static Copy** when topology editing is needed; this makes
an unnamed document from the exact visible pose and never overwrites the
animated source file.

## Frames and editing

The strip and readout are zero-based. First, Previous, Next, Last, Insert,
Duplicate, Delete, and frame-count controls are one validated undo operation
each. **Copy All** arms a complete-pose copy; **Copy Sel** arms a copy of stable
selected point IDs. Select the destination frame in the strip to complete it.

Transforms affect the current stored frame by default. **All Frames** applies
the same affine transform and the displayed pivot to corresponding points in
every stored frame. Starting any edit pauses playback and snaps a fractional
preview to its nearest stored frame. Updating frame 0 also updates the static
base pose required by native CAD readers.

## Playback and interpolation

Preview starts at 12 FPS, with interpolation on and looping off. These settings
are session state because neither CAD nor ANM stores timing.

- Interpolation is display-only and never creates or serializes extra frames.
- A loop interpolates from the last frame to frame 0.
- Non-looping playback holds and stops at the last frame.
- Scrubbing can show a fractional pose and snaps to the nearest frame on release.
- Pause chooses the nearest stored frame.
- Stop returns to the frame where playback began.

One immutable `CadScene` is evaluated per GUI iteration and shared by all four
views, picking, and coordinate display. Reciprocal face visibility and normals
are derived from that same pose, including during interpolation.

## Standalone ANM files

Import accepts both recovered `3DAN` and `3DGI` headers. An import is unnamed
and dirty; Save therefore requires a native X11 CAD path and never overwrites
the ANM source. Export defaults to `3DAN`, with an explicit `3DGI` variant.
Quantization and coordinates outside the recovered -127…127 game range are
reported before the atomic write.

Unattached native animation records are distinguished from editable face-bound
frames. They continue to round-trip, but authoring controls do not reinterpret
them.
