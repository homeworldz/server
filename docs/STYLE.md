# Documentation and Naming Style

## The project name

The name is one word, written **`Homeworldz`** in prose — an ordinary proper
noun, capitalized because it begins a sentence or because it is a name, and not
otherwise. There is no internal capital and no accepted abbreviation.

| Context | Form | Example |
| --- | --- | --- |
| Prose, headings, titles | `Homeworldz` | "Homeworldz is a virtual world server." |
| Wordmark, logo, domain | lowercase | `homeworldz.com` |
| Namespaces, binaries, roles, hostnames, package names | lowercase | `homeworldz::physics::World`, `homeworldz-region`, the `homeworldz` database role |
| Service identities | lowercase, dotted | `homeworldz.library` |
| Environment variables and macros | upper snake | `HOMEWORLDZ_TEST_DATABASE_URL` |

**`HomeWorldz` is not used.** The camel form invites an abbreviation the project
does not want and adds a distinction prose does not need: the logo already
separates the two halves of the name with color, and prose does not have to
reproduce that.

Identifiers follow their language's own conventions and are lowercase for
ordinary reasons, not as a stylization of the name. They are not user-facing and
need no coordination with the prose form.

## The client

The program a user runs is just **Homeworldz**, optionally with its platform —
"run Homeworldz", "Homeworldz for Windows", "Homeworldz in the browser". There
is no category noun for it.

Where the distinction from server software matters, it is the **Homeworldz
client**. See [ADR 0030](adr/0030-client-architecture.md).

Two words are reserved and should not be used loosely:

- **viewer** means a third-party Second Life-lineage viewer — Firestorm and its
  peers — which [ADR 0016](adr/0016-firestorm-compatibility-target.md) targets
  for compatibility. It never means the Homeworldz client.
- **frontend** means a rendering layer over the engine-neutral client core, in
  the sense ADR 0030 defines: the Godot, native WebGPU, and browser frontends.

## Line endings

Every text file in this repository is LF, in the repository and in every
working tree — Windows included. `.gitattributes` states it once with
`* text=auto eol=lf`, rather than leaving the checkout to each machine's
`core.autocrlf`, which is how a tree ends up half CRLF without anyone choosing
it.

This paragraph used to say native line endings, CRLF on Windows. It had been
untrue since `.gitattributes` was written: no Markdown file in the tree was ever
CRLF, so the rule described nothing and only misled anyone who followed it.

The exemptions are in `.gitattributes` and are about content addressing, not
style: a vault asset's identity is the SHA-256 of its bytes, so normalising a
carriage return out of a `.bodypart` renames the asset.

## Reporting a live observation

State **when** it was taken and **what version of the subject** it was taken
against. "The region publishes `rigged: false`" and "the region published
`rigged: false` at 17:40 UTC, on the binary from `4e365b3`" look equally factual
and have entirely different shelf lives.

A live finding reported bare is indistinguishable from a standing fact, and the
two go stale at completely different rates. This is not pedantry about
timestamps: on 2026-08-08 both sides of the client/server boundary got caught by
it within hours of each other, in opposite directions. A measurement of the
running grid was accurate when taken and false by the time it was read, because a
deploy landed in between; and a claim here that inventory support was "not close"
cited a document that had been accurate when written and was not when it was
quoted. Neither was a careless check. Both were correct observations of a subject
that moved.

The corollary is the useful part, and it belongs in prose as much as in code:
**when a live check disagrees with what another party reports, the first question
is which of the two is older, not which is wrong.**

Dates already appear throughout this repository's comments and ADRs — `Corrected
2026-08-08, from 71`, `measured on the reference body` — and that habit is why
those notes are still readable. Extend it to anything observed rather than
decided.
