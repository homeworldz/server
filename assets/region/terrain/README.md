# Bundled Region Heightmaps

Homeworldz terrain image import follows the OpenSimulator and Halcyon
`GenericSystemDrawing` convention: a lossless image is flipped vertically into
terrain coordinates and each pixel's HSL lightness is multiplied by 128
metres. Imported images must exactly match the region dimensions. Homeworldz
accepts PNG and intentionally rejects JPEG because lossy compression artifacts
become terrain height spikes.

`plateau-square.png` and `plateau-round.png` are deterministic, project-created
256-by-256 grayscale heightmaps. Both have an 18-metre seabed, a smooth shore
transition through the standard 20-metre waterline, and a calm 22-metre
plateau. The default square shoreline is approximately 250 by 250 metres, with
slightly softened corners. The separate alternate round shoreline is
approximately 200 metres in diameter.
Their corresponding `.raw` files are the current region service's rounded
eight-bit metre representation. Regenerate all four files with:

```cmd
go run ./grid/cmd/generate-default-terrain
```

`island3-smoothed.raw` is the earlier experimental Island 3 terrain and remains
available as an alternate raw heightmap. `sources/island3-preview.png` is the
user-supplied PNG conversion of the `Image-island3` free resource from which
that terrain was authored. It is retained only for provenance and visual
reference; its rendered water, ground texture, lighting, and pre-existing
compression artifacts mean it is not a terrain heightmap merely because its
container is now lossless PNG. Its original creator and upstream package are
not identified in the supplied files. Homeworldz records the stable
`Homeworldz Library` service identity as its importing creator/uploader rather
than inventing an author attribution. The source PNG has SHA-256
`f13dd19bf0c0be1cd9deb84fbb990ca3e6f8a219cc2aa06d6727eea946ca6acb`.

Set `region.terrain_path` in `region.ini` to another raw 65,536-byte heightmap
to override the development default. A missing or invalid file falls back to
the former flat 25-metre terrain.

## Rectangular plateaus (`.r32`)

`plateau-512x256.r32` is a 512-by-256 working surface for the macro regions
of ADR 0036: flat at 22 metres, falling to zero over the outermost 8 metres
of every edge. It is not a shoreline — there is no seabed and no waterline
transition — because its purpose is flat ground with a definite boundary.

The two `-join-` variants keep one half-edge at plateau height where a
neighbour abuts. Two adjacent regions that both ramp at their shared border
produce a trough along it, and since the ramp bottoms out below the 20-metre
waterline that border becomes a canal you swim rather than ground you walk.
`-join-sw` leaves the western half of the south edge flat and `-join-ne` the
eastern half of the north edge, which is the pairing for two 512-by-256
regions offset by one 256-metre tile.

All three are written by the same generator as the squares above.

## Exact heightfields (`.r32`)

`region.terrain_path` also accepts a file of `width * width` little-endian
32-bit floats — metres, one per sample, row-major — which is byte-for-byte what
a running region serves at `GET /map/terrain.raw`. Download from one region and
point another at the file to reproduce its ground exactly.

This is the same layout OpenSimulator calls **RAW32** and reads and writes with
`terrain load`/`terrain save` on a `.r32` file, hence the extension. Row order
was the one thing that could have differed silently — a mismatch loads mirrored
rather than failing — and it agrees. `RAW32.cs` iterates `y` outer, ascending
from 0, `x` inner, in both `LoadStream` and `SaveStream`, indexing `map[x, y]`;
that is `y * width + x`, the same as here, with `y` counting north from the
south edge on both sides. .NET's `BinaryWriter` emits little-endian floats, so
the byte order agrees as well.

Checked against OpenSimulator's source rather than against an exported file, so
a first real exchange is still worth eyeballing — but the layout is not in
doubt.

Two other formats are worth knowing about and are not implemented here. The
viewer's own Region/Estate **Download/Upload RAW terrain** buttons use Linden
RAW, thirteen bytes per sample and 256x256 only — supporting it is what would
make those buttons work, and it needs the Xfer transfer path. Terragen `.ter` is
the other format OpenSimulator interchanges, compact and widely supported by
terrain editors, but sixteen-bit and therefore quantised.

The eight-bit `.raw` form cannot do this. It stores whole metres, so a graded
slope becomes a staircase: the 65-degree face below rises 2.145 m per metre, and
rounding that to integers alternates between 2 m and 3 m steps — 63.4 and 71.6
degrees, one constant angle replaced by two wrong ones.

`gamma-slope-fixtures.r32` is the Gamma region (1024 m) captured 2026-08-06,
carrying the three slope test faces that make the walkable-slope limit
falsifiable. Each is a 10-metre ramp from a 25-metre base, 51 m wide, and
**exactly** constant: every step across a ramp is identical to the last bit, so
the angles are 0.000 degrees apart, not 0.1. Verified by an independent decoder
reading the committed file rather than by the code that wrote it.

Index as `y * width + x`, x varying fastest. A transposed read does not fail —
it returns flat ground at every band, 22.00 or 25.00 m with zero rise, which
looks like a plausible region and would be reported as the fixtures having gone
missing:

| face | x | y band | base to top |
| --- | --- | --- | --- |
| 70.0 deg | 300-310 | 180-230 | 25.0 to 52.5 m |
| 65.0 deg | 300-310 | 275-325 | 25.0 to 46.4 m |
| 57.5 deg | 300-310 | 380-430 | 25.0 to 40.7 m |

`welcome.r32` is the Welcome region (256 m) captured 2026-08-07, immediately
after its terrain was baked as its own revert baseline. Heights run 7.19 to
36.05 m over an 18 m seabed, so it is the shipped plateau plus the operator's
terraforming rather than a synthetic surface. 262,144 bytes.

The published traversal limit is 65 degrees, so the three faces sit either side
of it and on it. Note the ordering: the shallowest face is the *last* band, not
the first. A working note in this project had the outer two reversed; the table
above is what the bytes say, and is the one to trust.
