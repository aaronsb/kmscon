# Development process

How a change to the fork goes from an idea to `main`: where it is decided, where it is written, how it is checked, who checks it, and how a libtsm change reaches kmscon. It is written for a new contributor and for a fresh agent session. Every `make` target it names is defined in kmscon's `Makefile` and `fork/*.mk` or in libtsm's `Makefile`. The harness itself is described in [conformance.md](conformance.md); this page says when to run it and what to do with the results.

## The two repositories and the pin

The fork is two repositories, each a fork of its upstream:

| Repository | Upstream | Holds |
|---|---|---|
| [aaronsb/kmscon](https://github.com/aaronsb/kmscon) | kmscon/kmscon | the console, its renderers and backends, tiers 2 and 3, the decision records, these docs |
| [aaronsb/libtsm](https://github.com/aaronsb/libtsm) | kmscon/libtsm | the escape parser and screen model, tier 1 and its goldens |

kmscon builds libtsm as a static meson subproject from `subprojects/libtsm.wrap`, whose `revision` pins one libtsm commit. The pin is what makes kmscon's results repeatable: tiers 2 and 3 judge kmscon against exactly that libtsm.

A libtsm change therefore lands in two PRs: the change itself in libtsm, then a kmscon PR that moves `revision` to the merged libtsm commit and nothing else. kmscon PRs #35 and #41 are examples of the second kind.

To build kmscon against a libtsm checkout that is not the pin, such as a libtsm branch under review, pass its path:

```sh
make test LIBTSM=../libtsm-<topic>
```

`LIBTSM` bind-mounts that tree read-only over `subprojects/libtsm` inside the dev container. Nothing is committed to kmscon by doing this.

### Stale and mixed libtsm builds (#38, open)

Two hazards remain until [#38](https://github.com/aaronsb/kmscon/issues/38) lands:

- Meson never refreshes an existing `subprojects/libtsm` checkout. After the pin moves, a plain `make test` keeps building whatever revision was fetched first.
- `LIBTSM=` swaps the source tree under an existing build, and ninja rebuilds by modification time. A tree whose files are older than the last build is not recompiled, and kmscon links a mix of old and new libtsm objects. A struct layout change built this way segfaulted in libtsm's own suites.

Until then, in a kmscon checkout that already has `subprojects/libtsm`, reset that checkout to the pin after the pin moves (a bump you made or one you pulled), and start a clean build, from the root of the checkout:

```sh
docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v $PWD:/src -w /src kmscon-dev \
    meson subprojects update --reset libtsm
make clean
make test
```

The `docker run` line is the Makefile's `RUN` with the default image. `git -C subprojects/libtsm log --oneline -1` then shows the pinned commit. In a fresh worktree there is no checkout yet: the update reports `Not used` and exits non-zero, and the first `make test` fetches the pin. Run `make clean` as well whenever `LIBTSM=` points at a different tree than the last build did. In a fresh worktree, a `LIBTSM=` run leaves `subprojects/libtsm` behind as an empty directory owned by root (docker's mount point), and the next plain build then fails with "Subproject exists but has no meson.build file"; remove it with `rmdir subprojects/libtsm` first.

## One increment, start to finish

The sections after this one give the detail for verification, goldens and reviews.

### 1. Decide

A change that sets direction (a new capability, a different design, a change to the process) starts as a decision record in `docs/architecture/`, through the ADR tool:

```sh
docs/scripts/adr new <domain> "<title>" --verb add --capability <capability> --agent <name> --model <model>
docs/scripts/adr consider <N> --said "<the operator's words>" --via "<where they said it>"
docs/scripts/adr accept <N>
docs/scripts/adr lint
```

`docs/scripts/adr domains` lists the domains (render, terminal, fork) and `docs/architecture/adr.yaml` the capabilities. A record lands through its own PR, like code.

An accepted record is edited in place where that keeps it correct and readable, so its text says what is decided now. Where an edit changes a decision, the edited passage carries a dated note giving the operator's words, or the option they chose, and quoting what the text said before, as ADR-103's store rule does after its 2026-10-24 change. Appended dated sections remain the place for evidence, corrections and amendments that add to a record rather than replace part of it, such as ADR-102's "Corrections from the prototype (2026-10-22)" or ADR-301's "Amendment, 2026-10-20". Git keeps every earlier version.

A fix or a step already covered by an accepted record needs no new record; its PR cites the record and step it implements, such as "ADR-102 step 4b".

### 2. Branch in its own worktree

Every branch gets its own `git worktree` beside the main checkout, so several branches can build at once without disturbing each other. From the main libtsm checkout:

```sh
git fetch origin
git worktree add ../libtsm-<topic> -b fix/<topic> origin/main
```

kmscon worktrees are `../kmscon-<topic>`. Branch names follow the change: `fork/<topic>` for fork work, `fix/<topic>` for fixes, `docs/<topic>` for docs, `adr-<N>-<topic>` for a decision record. Each worktree has its own `fork/build`, so the first `make test` in it is a full build.

### 3. Commit

Commit messages follow the conventions below. A golden that changes goes in a commit of its own (see [Goldens are the judge](#goldens-are-the-judge)). Before opening the PR, the author runs the gates in [Verification before merge](#verification-before-merge) that apply to the change, starting with `make test` in the worktree, and records each result for the PR body. `make test` also runs `make lint`, after the build, the clang-format and meson format checks the GitHub workflow runs, at the workflow's versions (pinned in `fork/lint.mk`); `make format` rewrites what they flag.

### 4. Open the PR

Push the branch and open the PR against the fork, always naming the repository and base, because `gh`'s default for a fork is the upstream repository:

```sh
git push -u origin fix/<topic>
gh pr create --repo aaronsb/libtsm --base main
```

The PR body says what changed and why, which goldens moved and why, and which gates ran, each with its result. libtsm #13 and #14 and kmscon #33 show the shape. Anything left untested is listed under its own heading, and anything filed from review is listed by issue number.

### 5. Verify

Someone other than the author verifies the PR before it merges: see [Verification before merge](#verification-before-merge).

### 6. Review

Every PR gets a code review, and every finding is fixed or filed: see [Reviews](#reviews).

Some PRs also need the operator before they merge: a visual review on a real VT ([The visual review before merge](#the-visual-review-before-merge)), or a discussion of a security change or a change in approach ([Changes the operator decides](#changes-the-operator-decides)).

### 7. Merge

The PR merges with a regular merge commit, which keeps the branch's commits (the decision, the code, the goldens, the review fixes) in `main`'s history:

```sh
gh pr merge <N> --repo aaronsb/libtsm --merge --delete-branch
```

### 8. Pin the libtsm change in kmscon

For a libtsm PR, a kmscon PR moves the pin to the merge commit. From the main kmscon checkout:

```sh
git fetch origin
git worktree add ../kmscon-libtsm-<topic> -b fork/libtsm-<topic> origin/main
cd ../kmscon-libtsm-<topic>
git -C ../libtsm pull -q && git -C ../libtsm rev-parse HEAD   # the merge commit's full hash
# set revision in subprojects/libtsm.wrap to that hash
make test                                   # a fresh worktree fetches libtsm at the new pin
git -C subprojects/libtsm log --oneline -1  # must show the merge commit
git commit -am "build: pin libtsm <short hash>: <what the libtsm change does for kmscon>"
git push -u origin fork/libtsm-<topic>
gh pr create --repo aaronsb/kmscon --base main
```

The commit body names the libtsm PRs it brings in and the test count against the pin, as in `b55fdd0` and `1e4cc25`. The bump PR changes the one `revision` line. If the new libtsm moves a kmscon golden (the libtsm PR says so), the bump PR also carries that golden in its own commit, read and explained like any other, and runs `make tier3 JOBS=2` when the libtsm change alters rendered output. If kmscon needs code changes to use the new libtsm, they go in a separate kmscon PR after the bump (kmscon #33 followed the bump in #41).

The bump PR is verified and reviewed like any other: `make test` from a clean build at the new pin, and the tier 2 goldens unchanged unless the libtsm PR said they would move. Every other kmscon checkout, the main one included, needs the reset in [Stale and mixed libtsm builds](#stale-and-mixed-libtsm-builds-38-open) once the bump merges.

### 9. Clean up

After the merge, remove the worktree and the local branch, and prune the remote ones:

```sh
git worktree remove ../libtsm-<topic>
git branch -d fix/<topic>
git fetch --prune
```

`fork/build` is gitignored, so `git worktree remove` takes it with the worktree.

### 10. Close the session

Increments merge one by one; when the session's last one has merged and `main` is green (the bring-home point), the session closes with the upstream sync, when upstream has moved, then one ledger run, each landed as its own PR, then the handoff. See [The conformance record](#the-conformance-record).

## Verification before merge

Verification is done by whoever merges, not by the PR's author, in the PR's own tree (a worktree at the PR's head). The author's gate list in the PR body says what to expect; the verifier runs the gates again and compares.

| Gate | Where | When |
|---|---|---|
| `make test` | the PR's tree | every PR |
| `make asan` (ASan and UBSan; the valgrind leak control is meant to report EXPECTEDFAIL) | the libtsm PR's tree | every libtsm PR |
| `make clean` then `make test LIBTSM=../libtsm-<topic>` | a kmscon worktree at `origin/main` | every libtsm PR |
| `make golden REF=v4.8.0` and `make golden REF=<PR head commit>`, then `git status` | the libtsm PR's tree | every libtsm PR |
| a planted defect seen red, then green | the PR's tree | every PR that adds or changes a check or the code a check judges |
| every changed golden read | the PR's diff | every PR that changes a golden |
| `make tier3 JOBS=2` | the kmscon PR's tree | renderer, backend or packaging changes |

### The golden regenerations

Both regenerations rewrite the goldens of one set from a reference build, so `git status` afterwards shows what the PR's goldens disagree with:

- `make golden REF=v4.8.0` rewrites the upstream-sourced goldens from upstream libtsm v4.8.0. Nothing should change.
- `make golden REF=<PR head commit>` rewrites the fork-sourced goldens and all of `golden-draw2/` and `golden-frame/` from the PR's code. Nothing should change, since the PR's committed goldens were produced the same way.

Any change is either explained in the PR or a finding. Discard the regenerated files afterwards with `git checkout -- test/conformance`.

`make golden` runs git inside the dev container with only the checkout mounted, and a worktree's `.git` file points at the main repository, which the container cannot see. In a worktree, mount the main repository at its own path by overriding `RUN`, as libtsm's [test/conformance/README.md](https://github.com/aaronsb/libtsm/blob/main/test/conformance/README.md) documents:

```sh
M=/path/to/main/libtsm
make golden REF=v4.8.0 RUN="docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v $PWD:/src -v $M:$M -w /src libtsm-dev"
```

### A planted defect

A check is trusted only after it has been seen to fail. Plant the defect the check exists to catch, see it red, restore the file, see it green:

```sh
cp src/file.c src/file.c.bak
# edit src/file.c to plant the defect
make test                       # must fail, in the check you meant
mv src/file.c.bak src/file.c
touch src/file.c
make test                       # must pass again
```

The `touch` matters: `mv` restores the backup's older modification time, ninja then sees nothing newer than the planted build and skips the rebuild, and the restored tree tests the planted binary. A plant that stays red after the restore is the sign of a skipped rebuild. Don't restore with `git checkout` or `git stash` in a tree with uncommitted work: they discard that work with the plant.

The PR body lists each planted defect and the check that caught it, as libtsm #14 and kmscon #33 do.

### Reading changed goldens

Every changed golden is read: a tier 1 golden line by line in the diff, a tier 2 or tier 3 golden as an image beside the old one. The reader names what moved in each changed row or region and checks it against the commit message that changed it. `make tier2` writes each screen's capture to `fork/build/tier2/actual/<screen>.ppm` and a diff to `fork/build/tier2/diff/<screen>.ppm`, beside the golden in `tests/goldens/tier2/`. `make tier3` writes `fork/build/tier3/<variant>/<case>.png` and `<case>.diff.png`, beside `fork/tier3/golden/<variant>/<case>.png`. Any image viewer opens them.

### Tier 3

Tier 3 runs the installed package in a KVM guest and needs KVM and QEMU on the host; [fork/tier3/README.md](../../fork/tier3/README.md) has the requirements. `make tier3-image` provisions the guest once. `make tier3 JOBS=2` builds the package and runs the 2D variants two at a time. It runs locally only, and the GL variant is outside the default run until #17 is fixed.

## Goldens are the judge

The goldens decide whether a change is correct, so they change under stricter rules than the code they judge:

- A golden changes only in its own commit, never with the code that moved it. The commit message explains every changed row or pixel region: which cells, what they were, what they are now, and the change of behaviour that moved them. libtsm #13's `prog-tmux` move is an example.
- Upstream-sourced goldens (libtsm's `test/conformance/golden/`, for every case not in `fork-goldens.list`) come only from upstream libtsm v4.8.0, through `make golden REF=v4.8.0`. `make-golden.sh` refuses any other upstream reference.
- A case whose behaviour the fork changes on purpose is listed in libtsm's `test/conformance/fork-goldens.list` with a one-line reason, and its golden comes from a fork commit through `make golden REF=<fork commit>`. Moving a case into the list is a change to the judge and goes in the golden commit.
- `golden-draw2/` and `golden-frame/` are wholly fork-sourced: only the fork's libtsm has those views.
- Tier 2 goldens are regenerated with `make tier2-golden`, and tier 3 goldens with `make tier3-bless` after a `make tier3` run. Both rewrite every case they ran; keep only the cases meant to change and discard the rest (`git checkout -- <path>`). A case that changed without being meant to is a finding.
- A golden that would record a defect is not blessed. Fix the code first, then regenerate.

## Reviews

- Every PR gets a code review, by a person or a reviewing agent who did not write the change. The ledger PR is the exception: it changes no code, and its contents are checked instead (see [The conformance record](#the-conformance-record)).
- Each finding is fixed on the branch before merge, or filed as an issue that gives the reason it was deferred and a done condition. The PR body lists filed findings by number (kmscon #33 filed #40, #43, #44 and #45).
- Each round of fixes is posted as a PR comment that names the commits and the gates run on them (libtsm #12's "Review round 1, fixed in a1b9be8..e4eaed1").
- A review that finds nothing to fix still records what it checked, so the next reader can tell a clean review from an absent one.
- From the third fix round on a branch, a fresh author takes over: a person or agent session that did not write the earlier rounds.

## Defects inherited from upstream

A defect the fork inherited from upstream is fixed in the fork as its own `fix(...)` commit, with a regression test that fails before the fix and passes after it. libtsm #12 is a set of such fixes, one test each.

Both repositories are public. Commit messages, PR bodies and issues describe an inherited defect by its behaviour and its fix: no exploit narrative, and no paths or inputs that reproduce it against an unfixed build.

## Clean room

[ADR-300](../architecture/fork/ADR-300-write-from-protocol-specs-and-permissive-sources-never-from-kitty-s-code.md): nobody working on the fork, person or agent, opens kitty's source (GPL-3.0). Kitty's published protocol documentation may be read. The permissively licensed implementations ADR-300 lists (Ghostty, WezTerm, foot and libsixel, all MIT) may be consulted as reference. Anything adapted from one is attributed in the adapted file and in the commit, as ADR-300 requires, naming the file and function, as libtsm #13 cites Ghostty's `Screen.blankCell` and foot's `erase_cell_range`.

## The human check: the VT demo

The conformance tiers compare against goldens; the [local VT demo](demo.md) is where a person looks at the fork on real hardware, page by page, and records a judgement of each page that the agent reads back with `fork/demo/feedback.sh` (#47).

`make vt-demo [VT=N] [DEMO_ARGS='...']` is the one command to hand the operator: it builds and stages the dev build, runs it on a free VT through sudo, blocks until the operator presses q, then prints that run's answers and a health check of kmscon's log.

`fork/demo/run-vt.sh` starts the demo with a cleared environment, so a dev build's answers always go to the default `/tmp/kmscon-demo/feedback.tsv`. `DEMO_FEEDBACK` applies only when `demo.sh` is run directly.

A change that adds something visible adds its demo section in the same PR. [demo.md's "Adding a section"](demo.md#adding-a-section) gives the rules for a section file.

### The visual review before merge

The operator asked to look at changes live before they merge: "I'd still like to be involved in a visual review before merge (viewing live in the vtty - not the bad apple demo but our regular feedback review)". The review is the regular demo feedback run (`make vt-demo` with the PR's build staged), not the showcase (#75); the run's answers are read back, and a page rated wrong or unsure is resolved before the merge. The PR body names the demo sections to look at and what right and wrong look like.

Asked how far the review reaches, the operator answered: "I think having a comprehensive review is valuable. we should have some way to pass a page (an I don't know but more like "I'm not paying attention to this at this time")". The demo's `s` key is that pass: the page is recorded as `skipped`, apart from unsure.

How the steering session applies that answer, its own reading rather than the operator's words:
- A skipped page does not block the merge.
- The PR body records which pages were skipped, and the next visual review's PR body names them among the sections to look at. The PR body carries them from one review to the next: `feedback.sh` and the end of `make vt-demo` list the skipped pages of one run only.
- "Comprehensive" means every PR that can change what appears on a VT takes the review. That includes kmscon's render, font and video code, libtsm changes to screen behaviour, and refactors meant to draw the same pixels, where regressions hide. PRs that cannot change what appears merge after code review alone: libtsm code kmscon does not call yet (the ADR-103 image store and sixel decoder before they are wired in), the conformance harness and other tooling, and documentation. The first PR that puts such work on screen takes the review.

### Changes the operator decides

The operator asked to discuss two kinds of change before they merge, whatever they draw: "if there's a security change or a change in approach (necessary because limitations, opportunity, or archictechture [sic] I'd like to discuss)".

How the steering session applies that, its own process rather than the operator's words:
- A security change includes anything that changes what runs as root, what a program in the session can make kmscon read, write or execute, a trust boundary, a privilege, or a resource limit that guards against abuse.
- A change in approach includes a departure from an accepted ADR, or from the plan the PR set out with where no ADR covers it, made because of a limitation found on the way, an opportunity, or the architecture. A choice an ADR leaves open is raised when it changes behaviour other terminals or users would notice.
- Each is raised with the operator as a numbered question before the merge, not left as a note in a PR body. Where the answer changes an accepted ADR, the ADR is edited to say what is now decided, with a dated note giving the operator's words, or the option they chose, and quoting what it said before.

## The conformance record

A committed ledger run (`make sheet NOTE=...` and its entry in [LEDGER.md](../conformance/LEDGER.md)) happens once per working session, at its bring-home point: when the session's workstreams and the upstream sync have merged and `main` is green, measured from the head of `main` at that point. Never per PR, and never during the work.

A working session runs as a loop: increments merge one by one, then the bring-home point, then the upstream sync, then the ledger run, then the handoff to the next session.

### The upstream sync

Every bring-home pulls upstream in just before the ledger run, so each ledger row measures the fork with upstream's latest work in it (operator's decision, 2026-10-22). Fetch both upstreams and see what is new:

```sh
git -C ../libtsm fetch upstream && git -C ../libtsm log --oneline origin/main..upstream/main
git fetch upstream && git log --oneline origin/main..upstream/main
```

When either has new commits, it lands as a sync PR before the ledger run, libtsm first:

1. libtsm: a branch `sync/upstream-<date>` in its own worktree, `git merge --no-ff upstream/main` (a real merge, never cherry-picks, so `git log` keeps upstream's commits and authors), and a PR with `--repo aaronsb/libtsm --base main`. Where an upstream commit fixes behaviour, a unit or tier 1 case for it is written first and seen red on the fork before the merge and green after it. A tier 1 golden that the merge moves cannot be regenerated from v4.8.0, which still has the old behaviour, so its case moves into `fork-goldens.list` with the upstream commit named, and its golden comes from the merge commit, in a commit of its own after the merge (libtsm #15 did this for `prog-vim`).
2. kmscon: a branch `sync/upstream-<date>` merging `upstream/main` the same way, carrying the libtsm pin bump when step 1 merged (with the reset in [Stale and mixed libtsm builds](#stale-and-mixed-libtsm-builds-38-open)). PR with `--repo aaronsb/kmscon --base main`.

Conflicts are resolved to keep the fork's behaviour and upstream's intent. Where upstream solves a problem the fork already solved another way (upstream's `src/font/blocks.c` against the fork's `font_box.c`), the fork's code stays, upstream's is deleted in the merge commit if nothing calls it, and the merge message says so. An upstream change the fork has no equivalent for is taken on its merits. A sync that moves tier 2 or tier 3 goldens re-blesses them in a separate commit after the merge commit, with every changed image read beside its old golden and described, as in [Goldens are the judge](#goldens-are-the-judge); tier 3 is re-blessed for the 2D variants only. When a sync removes a difference that `fork/conformance/upstream-verdicts.txt` explains, that line is updated in the same PR.

The sync PR runs `make test`, `make tier2`, `make tier3 JOBS=2` and `make sheet-preview`, is reviewed and merged like any other, and the ledger run then measures the merged `main`.

### The ledger run

The ledger run is its own small PR, separate from any feature PR: a branch from that head of `main` (for example `docs/ledger-<date>`) carrying only the run page and its LEDGER.md entry, opened with `--repo aaronsb/kmscon --base main`, and merged once its contents are checked (the measured commits match `main`'s head and the page renders). It changes no code, so it needs no code review.

```sh
git fetch origin
git worktree add ../kmscon-ledger-<date> -b docs/ledger-<date> origin/main
cd ../kmscon-ledger-<date>
make tier3                              # optional: includes tier 3 in the run; a new worktree first needs
                                        # fork/vm/base.qcow2 and fork/vm/images linked from the main checkout
make sheet NOTE="what this run marks"
git add docs/conformance
git commit -m "docs(conformance): ledger run at <short hash of main>: <what this run marks>"
git push -u origin docs/ledger-<date>
gh pr create --repo aaronsb/kmscon --base main
```

`make sheet` runs tiers 1 and 2, draws the contact sheet and adds the ledger entry. It refuses a tree with uncommitted changes, so every entry names the commits it measured.

The exception is comparative analysis that ongoing branch work needs, such as a before-and-after of a renderer change: it uses `make sheet-preview`, which draws the run under `fork/build/`, uncommitted, as often as that analysis needs.

In progress: the ledger's format is being changed to one markdown page per run, and the generator is gaining a before/after comparison page that sets upstream kmscon against the fork. [conformance.md](conformance.md) describes the format in use.

## Conventions

- Commit messages say why the change is made. Subjects use the area as a prefix (`fix(vte):`, `feat(screen):`, `build:`, `docs(demo):`, `render/bbulk:`). No attribution trailers (`Co-Authored-By` and the like).
- Always pass `--repo aaronsb/<repo> --base main` to `gh pr create`. Both repositories are GitHub forks, and without `--repo` the PR targets the upstream project.
- Issues go where the work is: libtsm parser and screen work on [aaronsb/libtsm](https://github.com/aaronsb/libtsm/issues), kmscon and harness work on [aaronsb/kmscon](https://github.com/aaronsb/kmscon/issues). A kmscon issue cites a libtsm one as `aaronsb/libtsm#N`.
- Docs are written one line per paragraph, with no hard wrapping.
- Docs say what is unfinished: a gap, a stale golden or an open issue is named where the reader would otherwise assume it works.
- Gate results in a PR body name the commit they ran on, and a gate that was not run is listed as not run.
- In a loop that runs per pixel or per cell per frame, copy every value the loop doesn't change into a const local before the loop. Choose formats, depths and modes once, outside the loop: a switch with one short loop per case, with only the varying store repeated and the shared arithmetic in a static inline helper. Keep a loop compact when it runs once per glyph, per picture, or less, or when a benchmark (a micro-bench, or S1–S6) shows the specialised version within 5%. Every specialisation lands with its benchmark numbers in the PR and a test that its output is byte-identical.

## Working with coding agents

The fork's code is written by AI coding agents under the operator's direction. A steering session briefs each agent with the files to read, the gates to run and the shape of the report it returns; the agent works in its own worktree and opens the PR. The steering session verifies each PR itself, as above, has it reviewed, and merges. Agents never merge.

Agents push their own feature branches to aaronsb/kmscon and aaronsb/libtsm and open PRs without asking each time; the operator approved this on 2026-10-23. Pushes to `main`, force pushes, history rewrites, release tags and any write to a repository the operator does not own are still asked for each time.

The rules on this page apply to agents unchanged.

## What is unfinished

- [#38](https://github.com/aaronsb/kmscon/issues/38): `make test` can build against a stale or mixed libtsm; the manual reset above stands in for it.
- [#11](https://github.com/aaronsb/kmscon/issues/11): no CI runs the tiers. Every gate on this page runs locally, and the repository's GitHub workflow is upstream's plain build and format checks.
- [#17](https://github.com/aaronsb/kmscon/issues/17): tier 3's GL variant is outside the default run.
- The ledger's change to one page per run is in progress, so its format is not described here.
- The rules for reviews and fresh authors are applied by the people and sessions involved; nothing in the repositories enforces them.
