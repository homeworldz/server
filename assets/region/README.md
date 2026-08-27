# Bundled region assets

`default-avatar/` contains the four base body-part assets, a shirt and pants,
and five source textures needed to render Homeworldz's initial system outfit.
They were derived from the Halcyon simulator asset set without changing their
wearable parameters and named by viewer UUID so the region can import them
deterministically.

`library/textures/` contains the canonical Blank, Plywood, Transparent, and
Media texture assets expected by Second Life-compatible viewers and scripts.
They retain their standard viewer UUIDs. Library inventory records attribute
their import to the `Homeworldz Library` service identity, independently of
their upstream artwork provenance.

`viewer-standard/` contains server-backed textures and UI sounds that
Second Life-compatible viewers request by fixed UUID. These are protocol
resources rather than user-visible Library inventory. Supplying them prevents
grey fallback images, missing interaction sounds, and missing environment
settings on a new grid. The standard default-day settings asset is sourced
from OpenSimulator's BSD-licensed asset set; its upstream notice is retained
beside the asset.

`viewer-standard/sounds/` holds the OpenSimulator `OSSnd*` replacement sounds
from the same BSD-licensed set, each copied to the viewer UUID that Firestorm's
`settings.xml` names for it (`UISndClick`, `UISndTyping`, and so on). One file
serves several ids where the viewer asks for several: the eight
`UISndPieMenuSliceHighlight*` slots are one upstream sound copied eight times,
because the id is what the viewer fetches and it fetches all eight.

**A viewer asks for these constantly, and asks again every time.** A missing one
is not a single failed fetch: Firestorm marks the source corrupted and retries
on the next event, so one absent id produces an unbounded warning stream. That
is how the gap was noticed — `UISndFootsteps` fires on every footstep an avatar
takes.

**`UISndFootsteps` (`e8af4a28-aa83-4310-a7c4-c047e15ea0df`) came from elsewhere.**
The upstream BSD set has no footstep replacement — it maps 27 ids and that is
not one of them — so this one is a CC0 recording by GboxMikeFozzy, published on
OpenGameArt as `01-footstep.ogg` and dedicated to the public domain with no
attribution required. Resampled to mono 44100 from the 48 kHz original; nothing
else about it was changed.

It is the id that prompted the whole set: `getStepSound` returns it on every
step an avatar takes on land, so while it was missing it produced a warning on
every footstep.

`viewer-standard/textures/` also answers Firestorm's four hardcoded terrain
fallbacks — `TERRAIN_DIRT_DETAIL`, `TERRAIN_GRASS_DETAIL`,
`TERRAIN_MOUNTAIN_DETAIL`, `TERRAIN_ROCK_DETAIL` (indra_constants.cpp) — with
**our own CC0 layer textures under those ids**, rather than with anything
sourced from Second Life. Each is a byte-for-byte copy of the matching file in
`library/terrain/`, paired by what it depicts: dirt gets Ground079L, grass gets
Grass005, mountain gets Rock060, rock gets Rock002.

They are duplicated files rather than an alias mechanism, deliberately: the
alternative was code, and repeated bytes are a smaller debt than a format only
this directory understands. The grid is unaffected either way — blobs are
content-addressed, so both ids resolve to one stored blob.

**Shipped as `.j2c`, and it has to be.** The first attempt shipped the PNG
canonicals and let the pipeline derive the JPEG2000 the way it did for the
originals. It cannot: meshsmith reads canonical bytes from the vault, and a
bundled region asset is not vaulted (ADR 0026 narrowed the vault to
user-owned content), so every conversion failed with "canonical bytes
unavailable (vault answered 404)". A `.png` here registers an asset no viewer
can read. Ship the derived form for anything bundled that a viewer fetches as
a texture.

A viewer asks for these before the region handshake tells it which textures
this region actually uses, so serving them prevents a 404 rather than changing
what anyone sees; regions render from their own configured layers
(`terrain::layer_assets`) as before.

`IMG_MOON` and `DEFAULT_CLOUD_ID` are answered the same way, from the upstream
set's own `moon.jp2` and `cloud.jp2` — raw j2c codestreams despite the
extension, so they are copied under the viewer's ids unconverted.
`IMG_RAINBOW` and `IMG_HALO` have no counterpart anywhere available and remain
unanswered; both are optional sky overlays and their absence costs one 404 per
session each.

**Most of the ids a viewer hardcodes must NOT be served, which is why this
list is short.** Of 321 constants in `indra_constants.cpp`, `sound_ids.cpp` and
`llsettingssky.cpp`, 258 are absent here and nearly all of them should stay
that way:

- `IMG_USE_BAKED_*` (head, upper, lower, eyes, skirt, hair, arms, legs, aux) are
  **sentinels**, not assets. They mean "composite the bake for this slot".
  Serving bytes under one would make a viewer draw a texture where it must
  build a bake.
- `BLANK_MATERIAL_ASSET_ID` and `BLANK_OBJECT_NORMAL` are sentinels too — "no
  material", "no normal map".
- 220 `SND_*` are Second Life's library collision, slide and roll sounds, only
  ever requested when a *region triggers them*. This region has no SoundTrigger
  path at all, and Firestorm's OpenSim build swaps the whole matrix to the
  `OPENSIM_SND_*` ids, all 49 of which are shipped.

The list worth serving is the one a viewer actually asks for unprompted, and
the "asset could not be served" log is what identifies it. Reach for that
before adding anything here.

The collision sounds beside them are the same story one layer down.
`LLVOAvatar::getStepSound` returns the footstep id only while the avatar is on
*land*; on anything rezzed it returns the collision sound for that prim's
material, so a floor made of prims asks for a different id and produces the
same unbounded stream when it is missing. Firestorm's OpenSim build swaps the
whole matrix to OpenSim's own ids (`OPENSIM_SND_*`, llmaterialtable.cpp), and
the upstream set supplies all 49 of them; the 56 here are that set entire,
since the seven it does not currently name belong to the same matrix.

**Format is not a preference here, it is a decode requirement.** The viewer
decodes any Vorbis file and then writes a WAV header that is *hardcoded* to
mono, 44100 Hz, 16-bit (llaudiodecodemgr.cpp). It does not read the stream's
own rate or channel count back out. A stereo file is therefore played as
interleaved mono — garbled, and about twice as fast — and a 48 kHz file plays
about 8.8% slow. Everything in this directory is mono 44100 for that reason,
and anything added later must be too.

The Halcyon source is distributed under the 3-clause BSD license. The texture
set's provenance notice says that some included textures derive from Second
Life Viewer Artwork, copyright Linden Research, Inc., and are licensed under
Creative Commons Attribution-ShareAlike 3.0. The upstream notices are retained
in `HALCYON-ASSET-LICENSES.txt`.

Source: <https://github.com/HalcyonGrid/halcyon>
