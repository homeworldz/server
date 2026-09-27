-- Which assets are avatar bakes (ADR 0029, "Announcing the bakes").
--
-- A viewer fetches a server-baked texture from the grid's appearance service
-- with a plain GET and no credentials: the address is published at login and
-- the path names only an avatar, a bake slot and a texture id. That route is
-- therefore public, and it must not become a way to read any vaulted asset by
-- uuid — notecards and scripts are vaulted too. It serves only rows marked
-- here.
--
-- A column on the asset rather than inferred from anything already stored.
-- The creator does not tell a bake apart (bakes and bundled library assets
-- are both system-created), and the bytes cannot be trusted to say what they
-- are for.
--
-- Not part of the asset's immutable binding: a registration that says bake
-- latches it on, as a location's origin flag does, and nothing turns it off.
ALTER TABLE assets ADD COLUMN is_bake boolean NOT NULL DEFAULT false;

INSERT INTO schema_metadata (version) VALUES (35);
