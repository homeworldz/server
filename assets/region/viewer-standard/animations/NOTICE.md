# Standard viewer animations

The 118 `.animatn` files in this directory are the Second Life standard
animations (stand, walk, sit, fly, the gesture library and the rest), named by
the UUID a viewer asks the simulator for.

**Copyright Linden Research, Inc.** They are unmodified copies of the files in
the Second Life viewer's `indra/newview/app_settings/static_assets/`, taken from
the Firestorm sources at commit `08e2450baf65bdcf64a901f27bbcf1edb5c603ff`,
where they are distributed under the GNU Lesser General Public License,
version 2.1 (Firestorm's `doc/LICENSE-source.txt` and `doc/LGPL-license.txt`).

They are distributed here under that same licence, a copy of which is
`LGPL-2.1.txt` beside this file. They are **not** covered by this repository's
MIT licence. Each file's bytes are its identity to the grid, so they are kept
exactly as published.

Why they are here: Firestorm carries its own copies and never fetches them, but
a viewer that does not — Aurora bundles only its four walk and run clips — asks
the grid. Without these, its avatars stand in a T-pose at login and freeze in a
walk pose when they stop.
