ALTER TABLE assets DROP COLUMN is_bake;

DELETE FROM schema_metadata WHERE version = 35;
