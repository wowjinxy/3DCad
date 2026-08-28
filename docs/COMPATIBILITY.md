# Compatibility matrix

| Format/workflow | Read | Write | Notes |
| --- | :---: | :---: | --- |
| Later X11 CAD records | Yes | Yes | Native fixed-offset big-endian stream, including tags 3/4; unchanged sources save byte-exactly |
| Legacy packed CAD | Yes | Conversion only | Unnamed import; Save As writes later X11 CAD |
| `3DAN` animation text | Yes | Yes | Unnamed import; deterministic CRLF plus DOS EOF export |
| `3DGI` animation text | Yes | Yes | Same recovered grammar with alternate header |
| 3DG1 | Yes | Yes | Preserves 2–16-point faces |
| Wavefront OBJ | Yes | Yes | Negative indices and two-point line export supported |
| ASM shape libraries | Yes | No | Search/preview followed by explicit document Replace |
| `.COL` / `.PAL` | Yes | No | Indexed BGR555 preview data |
| SF2 Transfer | No | No | Deferred until deterministic game export is recovered |

All imports parse into temporary data and replace the live document only after
complete validation and unsaved-change confirmation. Native saves and ANM
exports encode to memory and use bounded UTF-8 platform services for flushed,
atomic replacement.

The optional corpus harnesses validate 500 recovered CAD files (475 X11 and 25
legacy), 379 recovered ANM files (358 `3DAN` and 21 `3DGI`), and an ASM catalog
of 445 entries (443 decoded and two explicitly unsupported clipping-plane
helpers). Recovered assets remain external to the repository.
