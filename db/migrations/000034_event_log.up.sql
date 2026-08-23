-- The grid event log: one row per thing that happened, with the time it
-- happened as the index.
--
-- Everything before this recorded state and not history. The users table knows
-- how many accounts exist but not whether anyone used one this month; presence
-- knows who is in-world right now and forgets the moment they leave. Neither
-- can answer "how many people logged in over the last thirty days", which is
-- the question a public grid statistics page exists to answer.
--
-- One table rather than one per event kind: the questions are all of the same
-- shape ("how many of X since T", "how many distinct users did X since T"), and
-- a new kind is then a new constant in Go rather than a migration.
--
-- kind is free text under a length cap rather than an enum. An enum would have
-- to be migrated to add a kind, and an event the grid could not record because
-- its type was unknown is a worse failure than a kind nothing yet counts. The
-- kinds in use are the constants in grid/internal/eventlog.
CREATE TABLE event_log (
    id          bigserial PRIMARY KEY,
    occurred_at timestamptz NOT NULL DEFAULT now(),
    kind        text NOT NULL CHECK (length(kind) BETWEEN 1 AND 32),
    -- ON DELETE SET NULL: deleting an account must not delete the record that
    -- a login happened, but it must not leave the account named either. A
    -- distinct-user count therefore ignores null user ids rather than counting
    -- every orphaned row as one more person.
    user_id     uuid REFERENCES users(id) ON DELETE SET NULL,
    -- Deliberately not a foreign key. A region row is a lease and comes and
    -- goes; the log outlives it, and a teleport to a region that has since
    -- been deleted still happened.
    region_id   uuid,
    detail      text CHECK (detail IS NULL OR length(detail) <= 512)
);

-- The two shapes every query has: "recent events of a kind" and, for the
-- distinct-user counts, the same restricted to rows that name a user.
CREATE INDEX event_log_kind_occurred_at_idx ON event_log (kind, occurred_at DESC);
CREATE INDEX event_log_kind_user_occurred_at_idx
    ON event_log (kind, user_id, occurred_at DESC) WHERE user_id IS NOT NULL;

-- Registrations are backfilled because the fact is already recorded: a user
-- row carries the moment the account was created, so the first thirty-day
-- registration figure is true on the day this migration runs rather than
-- climbing from zero for a month. Nothing else is backfilled — no other event
-- kind has a record anywhere to derive one from, and inventing rows to make a
-- chart look populated would make the log a worse witness than the empty one.
INSERT INTO event_log (occurred_at, kind, user_id)
SELECT created_at, 'registration', id FROM users;

INSERT INTO schema_metadata (version) VALUES (34);
