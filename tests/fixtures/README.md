# Test fixtures

Content used to exercise the server against something real, kept here when it
is ours to commit and kept out when it is not.

## What is here

- `a1f4a000-0000-4000-8000-000000000021.clothing` — an Alpha wearable
  (`type 16`) whose only texture is `TEX_LOWER_ALPHA` (21) set to
  `IMG_INVISIBLE`. Wearing it should make the lower-body bake fully
  transparent and leave every other slot alone.

  It is **not** in `assets/region/default-avatar/`, deliberately: everything in
  that directory is imported and served by every region, and this is a test
  article, not default content anyone should receive.

  Texture 21 is the *lower* alpha. Head is 23. The first version of this file
  masked 21 and was named for the head; the bake was right and the name was
  wrong, and only the bake's own hidden-fraction reporting said so.

## What is deliberately not here

The rigged-mesh bodies — the Second Life reference body, MakeHuman exports —
live outside the repository, by default in a sibling `fixtures/` directory, and
`scripts/check-fixtures.sh` finds them through `HOMEWORLDZ_FIXTURES`. They are
not ours to redistribute. That script refuses loudly rather than passing when it
finds nothing, which is the failure mode worth guarding against here too: a
fixture check that quietly reports success having checked nothing.

## Installing the alpha wearable on a test grid

The region imports UUID-named files from its asset directory at startup, so the
wearable becomes a real, grid-registered asset by being copied there:

```bash
sudo install -o homeworldz -g homeworldz -m0644 \
  a1f4a000-0000-4000-8000-000000000021.clothing \
  /opt/homeworldz/region/assets/region/default-avatar/
sudo systemctl restart homeworldz-region@welcome
```

Then give a test account an inventory item pointing at that asset (asset type 5,
inventory type 18, flags 16) and a Current Outfit link to that item (asset type
24, its `assetId` naming the item). Log in a client that does not bake for
itself and read the region's bake line: the masked slot reports
`"hiddenBySlot":{"10":1.000000}` and no other slot changes.

A client that bakes for itself — Firestorm — will not exercise this. It
composites its own bake and the region relays it untouched; the server bake is
only seeded for a client that supplies none.
