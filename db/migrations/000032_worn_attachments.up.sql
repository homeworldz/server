-- What each avatar is wearing, so it survives a relog into a different region.
--
-- Region-local state cannot hold this: an attachment is parented to an avatar
-- entity that exists only while its wearer is present, and the region a wearer
-- returns to is usually not the one they left. The record has to live where the
-- inventory it names lives.
--
-- Only the item and the point are stored, not the object asset. The region
-- resolves the item through inventory when it re-attaches, so a worn item whose
-- contents changed comes back as it is now, and a worn item that no longer
-- exists resolves to nothing instead of to a stale asset.
--
-- Keyed by (user_id, item_id) rather than by point: several items can share one
-- attachment point, but wearing the same item twice moves it rather than
-- doubling it, which is exactly this key.
CREATE TABLE worn_attachments (
    user_id          uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    item_id          uuid NOT NULL,
    -- The viewer's attachment point with ATTACHMENT_ADD already stripped. Zero
    -- means "wherever the item says", which is a request, not a state: nothing
    -- is ever worn on point zero, so it is refused here rather than stored and
    -- puzzled over later.
    attachment_point smallint NOT NULL CHECK (attachment_point BETWEEN 1 AND 127),
    worn_at          timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, item_id)
);

INSERT INTO schema_metadata (version) VALUES (32);
