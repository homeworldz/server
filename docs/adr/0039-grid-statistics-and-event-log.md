# ADR 0039: Grid Statistics and the Event Log

Status: Accepted

This ADR records **current expectation and intent**, not a commitment, and is
expected to be revised as evidence arrives.

**Implementation state (2026-08-23).** Built: migration 34 (`event_log`), the
`eventlog` package, `stats.Collector`, the extended `stats.csv`, the public
`GET /v1/stats`, and the management site's statistics column and page. Proven
against the local grid and database on the day it was written; the login,
logout and transit figures are zero until a grid running this build sees its
first ones, which is the honest starting point rather than a defect.

## The problem

Every grid publishes the same handful of numbers, and people compare grids by
them: how many avatars exist, how many of them used the grid in the last thirty
and sixty days, how many regions there are, how many are up, how long the grid
has been running. OSGrid's `infos_grid_result` page is the shape of what is
expected.

Homeworldz could answer none of the interesting half. Its storage records
**state**, not history:

| Store | Knows | Cannot say |
| --- | --- | --- |
| `users` | how many accounts exist | whether anyone used one this month |
| `presence` | who is in-world this second | who was in-world yesterday |
| `regions` | which regions hold a lease now | when one was last up |

"Active users over 30 days" is not derivable from any of them at any later
date. The information is not hidden; it was never written down. The daily
`stats.csv` row (three counts a day) was the whole of the grid's memory, and
counting a month's distinct people from a daily snapshot is not possible even
in principle: a snapshot cannot distinguish forty visits by one person from one
visit each by forty.

## Decision

**Write down what happens, once, in one table.** `event_log` holds one row per
event: when it happened, what kind it was, and optionally which user and which
region it concerned. Every published figure is then a `COUNT` or a
`COUNT(DISTINCT user_id)` over a window trailing the moment the figure is
taken.

One table rather than one per kind, because every question has the same two
shapes ("how many of X since T", "how many distinct people did X since T"), and
a new kind should be a Go constant rather than a migration. The `kind` column
is free text under a length cap for the same reason: an event that could not be
recorded because its type was unknown is a worse failure than a kind nothing
yet counts.

The kinds recorded now: `login`, `logout`, `registration`, `teleport`,
`crossing`, `transit` (a transit whose origin the region did not state), and
`grid_start`.

### Where each kind is recorded, and why there

- **`login`** — viewer login, at the point where the login has resolved to a
  region. Every failure before that revokes the session, and a login that never
  reached a region is not one a person made.
- **`logout`** — the region clearing presence. That call is the grid's only
  account of an avatar leaving, and the region makes it both for a viewer that
  logged out and for a session that was retired without one.
- **`registration`** — the website API creating an account. Migration 34
  backfilled one row per existing user from `users.created_at`, because that
  fact was already recorded; nothing else is backfilled, because nothing else
  has a record to derive one from.
- **`teleport`** / **`crossing`** — transit preparation, split by a new `kind`
  field on the request. Teleports and border crossings are the same request
  with different reasons, and a busy border would otherwise read as heavy
  teleport traffic. A region binary older than the field records `transit`
  instead of being guessed at.
- **`grid_start`** — the grid service starting, before it listens. Uptime is
  measured from the most recent one, which is why it lives in the log rather
  than in a process variable: the website API that publishes uptime is a
  different process from the grid whose uptime it is.

### Recording never fails the thing it records

Every call site goes through `eventlog.Note`, which logs a failed write and
returns. A login that succeeded must not be failed because a statistics row
could not be written. `Note` also detaches the context: the events worth
recording are usually recorded as a request finishes, and an insert cancelled
halfway is a lost event rather than a slow one.

### One collector, two publishers

`stats.Collector` assembles a `Snapshot` from five stores (users, provisioned
regions, region leases, presence, event log). The grid's daily CSV row and the
website API's `GET /v1/stats` both render that one struct, so the page and the
chart cannot disagree about what a column means.

The endpoint caches a reading for 30 seconds. It answers on every load of the
login page — the busiest unauthenticated page a public grid has — and half a
minute is invisible against windows measured in days. A *failed* reading is not
cached.

### Nothing is ever defaulted

A source that fails takes the whole snapshot with it: no row is recorded and no
JSON is served. A zero that means "the database was unreachable" charts as an
exodus. For the same reason a figure with no answer yet — uptime on a grid that
has not restarted since this shipped — is **absent** from the JSON and **empty**
in the CSV, never zero, and the statistics column omits the row rather than
printing a placeholder.

`stats.csv` gains columns and never reorders or repurposes them, so a chart
built against the old file keeps working. A file written by an older build is
upgraded in place the first time a row is due: the header becomes the current
one and existing rows are padded with **empty** fields, which says "this grid
did not measure that yet" rather than claiming it measured nothing.

## Consequences

- The interesting figures start empty and fill in as the grid runs. Thirty-day
  active users is only true thirty days after this is deployed; until then it
  is a true count of a shorter history.
- The log grows with traffic, and border crossings are the noisy kind — one row
  per avatar per border step. Nothing prunes it yet. A retention policy (drop
  or roll up rows older than a year) is the obvious follow-up and is deliberately
  not written before there is a volume to observe.
- `event_log.user_id` is `ON DELETE SET NULL`, so deleting an account keeps the
  history of what happened and drops the name. Distinct-user counts ignore rows
  naming nobody, so a deleted account stops being counted as a person rather
  than becoming an anonymous one.
- The event log is a grid-operations record, not a user-facing activity feed.
  Nothing published from it names a user: every figure is an aggregate.

## Alternatives considered

**Derive activity from the daily snapshot.** Not possible, as above: a
snapshot cannot distinguish visit counts from visitor counts.

**A `last_login_at` column on `users`.** Answers "active in the last 30 days"
and nothing else — not sixty days, not logins per day, not teleports, and not
one question asked after the column was added. The whole reason for a log is
that the next question is not known yet.

**One table per event kind.** Every count would then be a new migration and a
new query shape, for tables whose columns are identical.
