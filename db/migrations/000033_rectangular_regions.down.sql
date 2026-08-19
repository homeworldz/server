DROP TABLE provisioned_region_facets;

ALTER TABLE provisioned_regions
    ADD COLUMN size integer NOT NULL DEFAULT 1 CHECK (size IN (1, 2, 4));

-- A rectangle cannot survive the trip back; collapse to the facet edge, which
-- is the largest square the viewer ever saw.
UPDATE provisioned_regions SET size = LEAST(size_x, size_y);

ALTER TABLE provisioned_regions
    DROP CONSTRAINT provisioned_regions_no_overlap;
ALTER TABLE provisioned_regions
    ADD CONSTRAINT provisioned_regions_no_overlap
    EXCLUDE USING gist (
        int4range(grid_x, grid_x + size, '[)') WITH &&,
        int4range(grid_y, grid_y + size, '[)') WITH &&
    );

ALTER TABLE provisioned_regions
    DROP CONSTRAINT provisioned_regions_shape;

ALTER TABLE provisioned_regions
    DROP COLUMN size_x,
    DROP COLUMN size_y;

DELETE FROM schema_metadata WHERE version = 33;
