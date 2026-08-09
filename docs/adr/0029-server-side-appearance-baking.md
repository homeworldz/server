# ADR 0029: Server-Side Appearance Baking

Status: Accepted

Avatars are rendered from **baked textures** — flattened composites of the
layers a user wears (skin, tattoos, clothing) for each body region. Today the
**viewer** does this baking: Firestorm composites the bake layers and uploads
finished JPEG2000 baked textures, and the region only caches them
(`baked_texture_cache`, keyed by an outfit/wearable hash), stores them
content-addressed, re-serves them, and rebroadcasts `AvatarAppearance`. That
design depends on **every client baking**. Thin or headless clients — notably
LibreMetaverse/TestClient — never bake, so they render as gray clouds. Second
Life moved baking to the server ("server-side appearance"); Homeworldz adopts
the same model so that **any** client rezzes correctly with no client-side
baking.

## Decision

The region bakes avatar appearance itself. The pipeline:

1. **Resolve the Current Outfit** — read the user's Current Outfit folder
   (system type 46) from grid inventory, follow its link items (asset type 24)
   to the worn wearables/body parts.
2. **Fetch layers** — parse each `.bodypart`/`.clothing` (LLWearable) asset for
   its layer texture UUIDs and tint/alpha params, and fetch those textures.
3. **Decode** the layer textures (JPEG2000 → RGBA).
4. **Composite** the Second Life bake slots (head / upper body / lower body /
   eyes / hair / skirt), applying per-layer alpha and color tint at the
   standard bake resolutions.
5. **Encode** each baked slot back to JPEG2000.
6. **Store** the baked texture as a content-addressed asset (grid-registered)
   and record it in `baked_texture_cache` keyed by the outfit hash.
7. **Assemble** the avatar's `texture_entry` with the baked UUIDs, set the
   server-side-appearance flags, and **broadcast `AvatarAppearance`**.

## Coexistence with viewer baking

Server-side baking is **additive, not a replacement**. Baking viewers
(Firestorm) keep uploading their own bakes via the existing
`UploadBakedTexture` path, and the cache is shared and keyed by outfit hash, so
a viewer bake and a server bake for the same outfit are interchangeable. The
region bakes when a connecting client does **not** supply one — in particular
thin/headless clients. This preserves the current Firestorm behavior while
filling the gap for everything else.

## JPEG2000 and imaging

JPEG2000 decode and encode use **OpenJPEG** (BSD-2, C) via vcpkg, compiled into
the C++ region. Compositing operates on raw RGBA buffers (alpha blend + tint),
needing no additional imaging dependency. Cinder Roxley's **CoreJ2K** is .NET —
a reference, not a dependency here; Second Life's open-source server-side
appearance and LibreMetaverse's `Baker` are layout references. To the asset
layer, baked textures remain opaque `image/x-j2c` blobs, exactly as today.

## Reuse vs. new work

**Reused (already in the tree):** `baked_texture_cache` +
`store_baked_texture`/`find_baked_texture`; the content-addressed asset store +
grid registration; `encode_avatar_appearance` + its broadcast loops and the
already-wired (currently `0`) server-side-appearance flags field;
`texture_entry` parse/build helpers; the Current Outfit model and grid
inventory access.

**New:** the OpenJPEG dependency; an RGBA image buffer with J2C decode/encode;
an LLWearable parser; region-side Current-Outfit resolution; the bake
compositor; `texture_entry` assembly + appearance-flag setting; and a bake
trigger (login, and later outfit change).

## Derived data and performance

Baked textures are **regenerable derived data** — cache, never authoritative
content — and are exempt from vault durability (consistent with ADR 0026's
baked-texture exemption). Baking is CPU-bound image work and must run **off the
authoritative scene thread** (a worker), delivering `AvatarAppearance` when the
bake completes.

## Phasing

- **Phase 1:** OpenJPEG + J2C decode/encode; composite the **default
  six-wearable outfit** (opaque layers) into the standard bake slots;
  region-side Current-Outfit resolution; trigger on login; broadcast
  `AvatarAppearance`. Goal: default avatars — including LibreMetaverse bots —
  rez server-side instead of as clouds.
- **Later:** full wearable coverage (tattoos, stacked clothing, alpha layers),
  per-layer tint/alpha, all bake slots, re-bake on outfit change, and
  cache-invalidation on wearable edits.

## Relationship to other ADRs

- **ADR 0014 / 0026 / 0027** — baked textures are blobs: derived, cache-tier,
  and vault-exempt.
- Complements the thin-client capability work (`FetchInventoryDescendents2`,
  etc.) but largely **removes** the appearance dependency on client-side
  inventory/asset fetch, since the region produces the finished bakes.

## Phase 1 status (2026-07-22/23)

Implemented and deployed to the cloud grid (all four regions). Live-tested with
a LibreMetaverse bot (which cannot bake at all — it fails to enumerate its
wearables) plus a Firestorm avatar.

**Working:**
- Full bake pipeline, each stage unit-tested: J2C codec wrapper (OpenJPEG),
  LLWearable parser, compositing engine, bake-slot compositor, `TextureEntry`
  encoder, orchestrator, and the 253-param visual-params assembly.
- The bot rezzes with **no cloud and no skirt cone**, at the correct height
  (geometry derived from the default shape and applied to the seeded avatar).
- Key correctness fixes found live:
  - **Viewer asset/texture caps** must accept the slashless `?<type>_id=<uuid>`
    query form, not only `/?…` — otherwise every wearable/texture fetch 404s.
  - Unbaked bake slots must use **`IMG_DEFAULT_AVATAR`** (`c228d1cf-…`), the
    viewer's "never rendered" sentinel. `IMG_WHITE` (5748decc) draws a solid
    grey mesh (skirt cone); `IMG_INVISIBLE` (3a367d1c) is treated as an
    unfinished bake and leaves the avatar a cloud.
  - Never substitute the server bake into a *real* client's appearance stream:
    a baker mid-bake briefly presents zero-creator textures, and re-marking
    them oscillates the avatar between its own shape and the default. The seed
    covers headless clients; real bakers relay untouched.
  - Server-side appearance is signalled by **`appearance_version = 1`** in the
    `AppearanceData` field, which must agree with visual param **11000**
    (`llvoavatar.cpp`: `setIsUsingServerBakes(appearance_version > 0)`).

**Layer fidelity — the default outfit renders correctly (2026-07-23):**
The default shirt/pants use the opaque `IMG_WHITE` ("Blank") texture and are
shaped/coloured entirely by wearable params, so a faithful default bake needs
both tint and alpha masks (the SL layer model), not just raw compositing:
- **Per-layer color tint.** Clothing color is three params (r,g,b in 0..1) that
  multiply the layer (shirt grey `803/804/805`, pants reddish `806/807/808`).
  Hair is an `LLTexGlobalColor` ("hair_color"): params `112/113/114/115` each
  ramped (avatar_lad.xml) and summed (default Blonde `114`=.5 → brown).
- **Clothing alpha masks.** Without them the opaque Blank clothing covered the
  whole upper/lower region (grey hands, red feet). A TGA decoder
  (`image::decode_tga`) loads the shirt/pants masks (bundled under
  `assets/region/default-avatar/`, read directly from disk — they are not
  UUID-named store assets); they are applied per LibreMetaverse's bake — normal
  masks (sleeve `800`, pants length `815`) union coverage, multiply masks (shirt
  bottom `801`, collar `802`) carve — thresholded per texel at
  `mask <= (1-value)*255`. Garments now stop at wrists/ankles; hands and feet
  stay skin.

Result: a LibreMetaverse bot (whose own client baker crashes in
`Baker.ApplyTint`) rezzes as a correct default avatar — brown hair, grey shirt
with skin hands, reddish pants with skin feet — entirely from the server seed.

**Notes / follow-ups:**
- Earlier a "204-byte empty body bake" was misdiagnosed as a bug and the bake
  briefly skipped Blank textures; that was reverted — Blank is opaque *white*,
  not "no layer", and the correct result comes from tint + masks above.
- LibreMetaverse's baker threw `IndexOutOfRangeException` in
  `Baker.ApplyTint` (`BakeLayer.cs`) on the default outfit — a real LMV bug
  (reported to Cinder, **fixed in LMV v3.1.3**, 2026-07-24, along with the same
  guard in `MultiplyLayerFromAlpha`). While it crashed it forced the bot onto
  the server bake, making it a pure server-side-bake test; with v3.1.3 the bot
  baker works again, so testing the server bake alone needs the bot's baking
  disabled or a client that never bakes.
- Only the default six-wearable outfit is exercised end-to-end. Stacked
  clothing, tattoos, jacket/glove/shoe masks, skin tone params, and re-bake on
  outfit change remain future work.
- **A grid must serve `IMG_INVISIBLE`**
  (`3a367d1c-bef1-6d43-7595-e88c1e3aadb3`). It is a viewer built-in named in
  `indra_constants.cpp` as a "dataserver" asset, exactly like `IMG_WHITE`
  (`5748decc…`), and this grid served the second and not the first. Checking a
  box on an Alpha wearable sets that body region's alpha texture to it, and a
  layerset compositing *fully* transparent is never uploaded at all — the
  viewer sets the bake itself to `IMG_INVISIBLE`
  (`llviewertexlayer.cpp`). Unserved, one absent asset produced three distinct
  failures: the viewer marked its own bakes missing and rendered a **cloud**;
  the server bake fetched the mask, got nothing, and hid nothing while
  reporting success; and the wearable cache will not record a bake slot naming
  an asset the region lacks, so every `AgentCachedTexture` missed and the
  viewer **re-baked without end** (114 uploads, 17.8 MB in ninety seconds,
  stopping only at logout). The region now synthesizes it at startup
  (32×32 transparent J2C) so a deployment repairs itself.

  The general rule this is an instance of: a viewer built-in UUID is part of
  the protocol, and a grid either serves every one of them or finds out which
  it missed from a symptom nowhere near the cause. Alpha wearables verified
  worn in Firestorm 2026-08-09.

  What that verified is the **viewer's** path: the viewer composites its own
  bake and the region serves and caches it. The **server** bake's alpha
  masking is unit-tested (`region/tests/bake_test.cpp`) and, since the mask a
  region actually applies is a J2C it synthesizes at startup rather than a
  bundled file, the test now encodes that exact image, decodes it, asserts its
  alpha survives the codec, and drives a bake with the result — undersized
  against the skin, so the resize an in-world mask goes through is exercised
  too. Both assertions fail when the multiply in `bake.cpp` is removed.

  It was **unreachable** until the bake read a real outfit: it only ever ran
  the fixed six-item default list, which holds no Alpha wearable. That is
  fixed — see *Per-outfit baking* below — and the server bake is now proven on
  a worn alpha too (same section).

## Per-outfit baking (2026-08-09)

The bake is keyed by the outfit, not owned by the default. Two avatars in the
same clothes composite to identical images, and the default outfit is simply
the outfit most of them are in — it is one entry in that cache rather than a
special case. The key is the worn asset ids sorted and deduplicated; the bake
is handed the order the outfit listed, which compositing reads.

`ensure_worn_outfit_bake` resolves the Current Outfit folder (system type 46)
through the grid, lists it, and keeps clothing (asset type 5) and body parts
(13). Worn objects live in that folder too, and an attachment has nothing to
do with a bake. Listing needs a route that did not exist: server-to-server,
nothing could read a folder. `GET /api/v1/inventory/{user}/folders/{id}/items`
answers it and resolves each link's target in the same read, because a COF
holds links (asset type 24) whose `assetId` names an *item*, and the region
makes these calls on the thread it serves HTTP from.

Every failure falls back to the default outfit and says which failure it was.
A wearer who owns nothing and a grid that did not answer produce the same naked
avatar and are not the same fact. A failed bake is remembered as failed, so a
broken outfit is not re-fetched in full on every arrival; the cost is that one
lost to a transient grid failure stays lost until the region restarts.

Verified live on the cloud grid the day it shipped: an appearance-less bot
joining Welcome produced `system-folders/46` 200 in 5 ms, the folder listing
200 in 1 ms, and `"server outfit bake ready","outfit":"worn","slots":5,
"visualParams":253,"unfetchableMasks":0` — the wearer's own outfit, not the
default. Two grid calls, six milliseconds, on the path that carries the
thread-blocking hazard.

### The alpha proof

A bake now measures the fraction of each slot it left fully transparent, on the
composited image rather than from the wearables it was given. That number is
the one piece of evidence the IMG_INVISIBLE failure lacked: the bake fetched a
mask, applied nothing, and reported success in every other respect.

An Alpha wearable masking `TEX_LOWER_ALPHA` (21) was put in a test account's
Current Outfit — a real asset the region serves, a real item, a real COF link —
and an appearance-less client joined. The bake reported

```
"outfit":"worn","slots":5,"unfetchableMasks":0,
"hiddenBySlot":{"8":0.010872,"9":0.000000,"10":1.000000,"11":0.851013,"20":0.125210}
```

Slot 10 is `TEX_LOWER_BAKED`: the masked region came out **entirely
transparent**, while head, upper, eyes and hair were untouched (the eyes and
hair figures are their own texture's transparency, unchanged by any mask). That
is the whole chain — COF listing, wearable parse, IMG_INVISIBLE fetched from the
region store, the alpha multiply, J2C encode — exercised on real assets rather
than fixtures.

Worth recording about the fixture: it was first written masking texture 21 and
*named* for the head. 21 is `TEX_LOWER_ALPHA`; head is 23. The bake was right
and the label was wrong, and the log is what said so.

### Re-baking mid-session

Arrival re-reads the outfit, so a relog or a region crossing has never needed a
trigger — the gap was only the middle of a session. An outfit change happens in
inventory, on the grid, and nothing about it reaches a region: a client that
bakes for itself sends a new `AgentSetAppearance`, and one that does not has no
way to say anything at all.

`POST /appearance/refresh/<uuid>` (service token) re-reads the Current Outfit,
re-bakes, replaces the seeded appearance, and tells the other avatars. The
wearer is found by the agent id inside its own appearance, because a caller
cannot know whether it is keyed by circuit endpoint or session participant. The
serial is **incremented** — a viewer ignores an appearance whose serial it has
already seen, so a re-bake that reused it would be one nobody rendered. An
avatar that is not on this region answers 404 rather than an error: a grid
telling every region that a wearer changed clothes is right to, and all but one
will say exactly that.

Verified live, mid-session: with the alpha worn the bake reported slot 10 at
`1.000000` hidden; the COF link was removed and the endpoint called; the next
bake reported slot 10 at `0.000000` with every other slot identical, and the
serial went 1 → 2. The broadcast leg is **not** covered by that run — no other
avatar was connected, and the log said so (`"told":0`).

Nothing calls this automatically yet. The grid knows when a COF changes and
which region a wearer is on, so the grid is where the caller belongs.
