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

**`UISndFootsteps` (`e8af4a28-aa83-4310-a7c4-c047e15ea0df`) is deliberately not
here.** The upstream BSD set has no footstep replacement — it maps 27 ids and
that is not one of them — so there is nothing to copy that this project has the
rights to. It needs a sound of our own or a compatibly licensed one, and until
there is one, that warning stream is expected rather than mysterious.

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
