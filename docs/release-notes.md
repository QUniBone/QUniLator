# Release notes

A release note is the only part of a release most people read. It is written
for an operator who runs a board and wants to know, in one screen, what this
version gives them and what it changes under them.

## Where they come from

The top stanza of `packaging/debian/changelog` is the source. `release-deb`
renders it into the GitHub Release body — the stanza body verbatim, `  * `
bullets turned into `- `, plus a line naming the artifacts and the image base —
so the text in the changelog is the text the world sees. There is no second
place to write, and nothing to copy between them.

That also means the stanza is written for two readers at once: `apt changelog`
shows it as plain text, and the release page renders it as Markdown. Anything
that only works in one of the two does not belong in it.

## Shape

**A summary paragraph, then one bullet per change.**

The summary is two to four sentences with no bullet marker, and it is what
somebody deciding whether to upgrade reads. It names the theme of the release,
what an operator gets, and anything that moves under them on upgrade — a
renamed image, a migrated configuration, a service that comes back configured
differently. It does not list the bullets in prose.

Each bullet opens with a **short topic phrase closed by a period**, then says
what the change does:

    * Development kit in one command. "sudo qunilator-devkit" installs what a
      build needs - compiler, headers, the TI PRU code generation tools - and
      fetches the repository into /root at the tag the installed package
      carries.

The topic phrase is what makes the list scannable and it is what keeps every
entry from opening with "A board…". Order the bullets by what an operator meets
first: what the release adds, then what behaves differently, then what was
fixed. Fold small related fixes into the bullet they belong to rather than
giving each its own line; a stanza past eight or nine bullets is asking to be
grouped.

## Voice

- **Present tense, declarative.** The version does this; it does not "will" do
  it and nobody "has added" it.
- **Name the subject.** `compile.sh`, `qunilator-devkit`, DCOK and POK, the
  first-run dialog. Commands, paths and parameters are written as they are
  typed.
- **Say what it means for the operator**, not what was edited. "An idle board
  costs about 1% of its CPU" is the change; "the panel worker now probes before
  polling" is the implementation, and belongs after it if at all.
- **Numbers where something was measured**, and only measured ones.
- **A fix says what went wrong**, in one clause, so a reader recognises the
  symptom they had. Past tense is right there, and only there.
- **No project history.** No PR or issue numbers, no "as discussed", no
  rejected alternatives, no "we". The commit and the ticket already hold that.
- **No marketing.** Nothing is powerful, seamless or blazing.
- **Vary the openings.** Two bullets in a row starting with the same word read
  as a template; the topic phrase makes that easy to avoid.

## Mechanics

- Wrap at 79 columns. Bullets are `  * `, continuation lines four spaces.
- A blank line before the ` -- name <email>  date` trailer, and none between
  the bullets.
- The trailer names whoever cuts the release, with the date it is cut, in
  RFC 2822 form.
- The version in the stanza header is what the tag must say: `v1.20.0` against
  `1.20.0-1`. `release-deb` checks this and fails the release if they disagree.
- Rewording a stanza after its tag is pushed means updating the GitHub Release
  body too — regenerate it from the changelog the way the workflow does, and
  check first that nobody has edited the body by hand.
