-- Rectangular provisioned regions (ADR 0036).
--
-- A region's footprint becomes size_x by size_y tiles, presented to viewers as
-- a row of square facets whose edge is the shorter dimension. The shape rule
-- is the facet rule: the shorter dimension must be a proven square size
-- (1, 2, or 4 tiles) and must divide the longer, so the largest square tiles
-- the shape exactly. LEAST/GREATEST keeps the rule orientation-free.
ALTER TABLE provisioned_regions
    ADD COLUMN size_x integer NOT NULL DEFAULT 1,
    ADD COLUMN size_y integer NOT NULL DEFAULT 1;

UPDATE provisioned_regions SET size_x = size, size_y = size;

ALTER TABLE provisioned_regions
    ADD CONSTRAINT provisioned_regions_shape CHECK (
        LEAST(size_x, size_y) IN (1, 2, 4)
        AND GREATEST(size_x, size_y) % LEAST(size_x, size_y) = 0
    );

-- The no-overlap exclusion already built its two ranges independently; it
-- only needs to read the two new columns instead of one.
ALTER TABLE provisioned_regions
    DROP CONSTRAINT provisioned_regions_no_overlap;
ALTER TABLE provisioned_regions
    ADD CONSTRAINT provisioned_regions_no_overlap
    EXCLUDE USING gist (
        int4range(grid_x, grid_x + size_x, '[)') WITH &&,
        int4range(grid_y, grid_y + size_y, '[)') WITH &&
    );

ALTER TABLE provisioned_regions DROP COLUMN size;

-- Names for facets beyond the first. Facet 0 keeps the region's own name, so
-- every existing square region is untouched; a rectangle with N facets names
-- facets 1..N-1 here, in map-coordinate order (eastward for a horizontal row,
-- northward for a vertical one). To a viewer each facet is an ordinary region,
-- so facet names share the grid's region-name uniqueness rules: the index
-- below enforces facet-vs-facet, and the store checks facet-vs-region in the
-- same validation that already checks region-vs-region.
CREATE TABLE provisioned_region_facets (
    region_id   uuid NOT NULL REFERENCES provisioned_regions(id) ON DELETE CASCADE,
    facet_index integer NOT NULL CHECK (facet_index >= 1),
    name        text NOT NULL CHECK (length(name) BETWEEN 1 AND 128),
    PRIMARY KEY (region_id, facet_index)
);

CREATE UNIQUE INDEX provisioned_region_facets_name_ci_idx
    ON provisioned_region_facets (lower(name));

INSERT INTO schema_metadata (version) VALUES (33);
