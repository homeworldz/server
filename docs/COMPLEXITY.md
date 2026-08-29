# Avatar rendering complexity

How a viewer computes the number it calls **complexity** (historically ARC,
avatar rendering cost), and which inputs actually move it.

Read from the Firestorm source at `../firestorm`, `Firestorm_Beta_7.2.5.81336-32-g08e2450baf`,
on 2026-08-29. The two functions that matter both carry this banner:

> This calculation should not be modified by third party viewers, since it is
> used to limit rendering and should be uniform for everyone.

So this is not Firestorm-specific behaviour. Treat it as the shared rule across
the Second Life-lineage viewers [ADR 0016](adr/0016-firestorm-compatibility-target.md)
targets, and expect the numbers to agree between them.

## Why this matters here

Complexity is what makes a viewer refuse to draw an avatar. Above
`RenderAvatarMaxComplexity` the avatar is replaced with a flat untextured
impostor — a "jellydoll" — and **the viewer stops requesting its textures
entirely**. From the server side that presents as an avatar that renders grey or
black with no texture fetches and no errors on either side, which is
indistinguishable from a bake that failed. It is not a server fault and no
amount of looking at the bake pipeline will find anything.

The per-attachment clamp is `MaxAttachmentComplexity`, default `1.0e6`.

## The whole formula

```
complexity = body_parts
           + Σ over attachments of
                 clamp(volume_cost + texture_cost + children_cost,
                       0, MaxAttachmentComplexity)
```

`LLVOAvatar::calculateUpdateRenderComplexity` and
`LLVOAvatar::accountRenderComplexityForObject` in `indra/newview/llvoavatar.cpp`.

### Body parts are not geometry

200 (`COMPLEXITY_BODY_PART_COST`) per **visible baked slot**. The loop walks the
bake slots and adds the cost when the slot's texture is defined and is not
`IMG_INVISIBLE`. The skirt slot counts only when a skirt is worn.

A whole body is therefore about 1000–1200, whatever its shape or the resolution
of its bakes. Body geometry contributes nothing. Everything interesting is in
the attachments.

## Per prim: the base cost

`LLVOVolume::getRenderCost` in `indra/newview/llvovolume.cpp`.

```cpp
shame = num_triangles * 5.f;    // floor of 2
```

`num_triangles` is **not the raw triangle count**. It is
`LLMeshCostData::getRadiusWeightedTris(radius)`, with `radius = getScale().length() * 0.5`
— half the diagonal of the object's bounding box.

That function (`indra/newview/llmeshrepository.cpp`) blends all four LOD
triangle counts, weighting each by the screen area it would occupy over a 512 m
range:

```
dlowest = min(radius/0.03, 512)      lowest_area = π·dlowest²  (capped)
dlow    = min(radius/0.06, 512)      low_area    = π·dlow²
dmid    = min(radius/0.24, 512)      mid_area    = π·dmid²
                                     high_area   = π·dmid²
```

The areas are made disjoint, clamped into `[1, 102944]` (102944 m² being the
area of a circle enclosing a region), normalised, and used as weights over
`mEstTrisByLOD[0..3]`.

**The consequence is that physical size drives cost.** The same mesh scaled up
costs more, because its high LOD covers more screen area before the viewer
switches down. In practice this is a bigger lever than the triangle count
itself: fixing an object's LOD chain or reducing its scale moves complexity
further than decimating the high LOD does.

Animated objects (animesh) take a different branch, proportional to
`getEstTrisForStreamingCost()` so that ARC stays in step with streaming cost.

If the count comes out at zero it is treated as 4.

### Mesh size and vertex count are not inputs

`gMeshRepo.getMeshSize()` is called, but only to detect that a mesh is rigged
and to return 0 — "user should know their content isn't render-free" — when the
mesh is unknown. The adjacent comment reading `weighted attachment - 1 point for
every 3 bytes` is stale: the branch sets a flag and nothing more. Vertex counts
appear nowhere.

## Per prim: the multipliers

Each is applied **once per prim**, not once per face — one alpha face anywhere
in the prim multiplies the whole prim. They compound.

| Condition | Multiplier |
| --- | --- |
| Flexible prim | 5 |
| Any face in the alpha pool | 4 |
| Animated texture (`face->mTextureMatrix`) | 4 |
| Shiny | 1.6 |
| Glow > 0 | 1.5 |
| Bump map | 1.25 |
| Invisiprim (primary format `GL_ALPHA`) | 1.2 |
| Rigged / weighted mesh | 1.2 |
| Planar texgen | 1.0 |

A rigged mesh with one alpha face is 4 × 1.2 = 4.8× its geometry cost.

## Per prim: the flat adders

Applied after the multipliers, so they are not amplified.

| Condition | Cost |
| --- | --- |
| Each media-enabled face | +1500 |
| Prim produces light | +500 |
| Particle source | `min(particles, 2048) × average particle size × 1` |

Particle count is `mBurstPartCount × ceil(mMaxAge / mBurstRate)`, capped at 2048.

## Textures

`LLVOVolume::getTextureCost`, charged per **unique** texture in the linkset:

```cpp
texture_cost = 256 + 16 * (height/128 + width/128);
```

| Resolution | Cost |
| --- | --- |
| 256² | 320 |
| 512² | 384 |
| 1024² | 512 |
| 2048² | 768 |

The uniqueness set is cleared **per attachment**, not per avatar, so one texture
shared across two attachments is paid for twice.

This is a floor that cannot be decimated away. Twenty attachments carrying one
1024² texture each are 10240 before a single triangle is counted.

## What actually moves the number

In rough order of leverage, for content that is over budget:

1. **Texture resolution and count.** Flat, unavoidable, and usually the largest
   single contributor on a modern mesh outfit. Halving a dimension is worth more
   than it looks — cost is linear in width plus height, not in area.
2. **The multipliers.** An unnecessary alpha face, a flexi, or an animated
   texture multiplies everything else. These are the cheapest fixes because they
   are usually accidents.
3. **Physical scale and the LOD chain**, through the radius weighting above.
4. **Triangle count**, last — and only at the LODs the weighting actually
   reaches.

## Bearing on server work

- An avatar that draws as a flat untextured shape with **no texture requests**
  is over the viewer's limit, not un-baked. Check complexity before the bake
  pipeline.
- Complexity is computed entirely from what the viewer already holds: the bake
  slots it sees defined, and the attachments it has rezzed. Nothing the region
  sends names a cost, and there is no cap or message through which a region can
  influence, report, or override it.
- Imported content is the usual source of surprise. A conversion that preserves
  triangles faithfully can still land far over budget on texture resolution
  alone, and an export whose LOD chain is a single level pays the high LOD's
  count at every distance.
