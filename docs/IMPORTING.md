# Importing a character from Character Creator

Upload the `.fbx` and the server does the rest ([ADR 0035](adr/0035-server-side-source-format-import.md)):
it stores your file untouched, converts it, and puts one object per mesh in your
inventory. A Character Creator body arrives as ten to fifteen items — body, eyes,
teeth, tongue, and each garment and hair piece separately — because that is how
the character was built and it is how a Second Life-lineage viewer wants it.

Most of the export dialog does not matter. Four settings do, and one of them is
easy to get wrong in a way that looks like it worked.

## The settings that matter

**Character → Use T-Pose As Bind Pose — check it.**

This is the one. There is a separate `Default Pose: T-Pose` above it, and setting
*that* alone is not enough: it poses the character while leaving the skin bound
in Character Creator's own A-pose. Skinning follows the bind, not the pose, so
the body imports with its arms about 30° low and every animation you play on it
lands 30° lower still. Nothing about the file looks wrong.

`homeworldz-fbx-diag` reports both, so you can check before uploading:

    pose:     arm 0.013 degrees below horizontal - T-pose
    bind:     arm 0.013 degrees below horizontal - THE BIND POSE IS T

If the second line says the bind is A-pose, only the first setting was applied.

**General → Axis: Y.** The importer converts into glTF's frame, which is Y-up.

**Texture Settings → Embed Textures — check it.** Character Creator writes
texture paths as absolute paths on your machine, so a file without embedded
textures arrives with none of them. Embedded is also the default and what every
export tested here has used.

**Texture Settings → Max Texture Size — worth setting to 2048 or 4096.** Not
required: the limit is three quarters of the per-asset file cap, and a 19 MB
atlas passes. But every texture becomes an asset the world serves to everyone
who sees you, and 8K skin maps cost far more than they show.

## What the importer does that you do not have to

- **Retargets the rig.** `CC_Base_*` joints map onto the Bento skeleton — all 85
  a Character Creator skin binds, onto 60 Bento joints. Twist bones fold into the
  limb they twist, five toe bones into the one Bento has, and an accessory bone
  onto whatever it hangs from.
- **Keeps your proportions.** The body is not squeezed onto Linden's skeleton;
  its own joint positions ride along as overrides, which is the same mechanism
  every non-Linden-shaped mesh body uses.
- **Prunes influences.** Character Creator weights up to seven joints per vertex
  and the viewer takes four; the heaviest four survive, renormalized.
- **Composites opacity.** Lashes, brows and hair carry opacity as separate
  images, which glTF has no room for; they are merged into the base colour's
  alpha so the transparency survives.

## What it does not do yet

- **OBJ and DAE**, and archives of a source plus its external textures.
- **Fix an A-pose bind.** Checking the box above is the whole answer today.
