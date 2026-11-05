# Path-walk delta-compression experiments

This is an inventory of the experiments on `path-walk-prev-version`.
It distinguishes compression policies from correctness fixes and records
which measurements can be compared directly.

All sizes below are **packfile MiB**, excluding indexes and other side files.
The local comparisons use frozen snapshots of Fluent UI, Git, Node, Azure
DevOps Docs, and Linux, isolated object stores, eight threads, window 10, and
requested depth 50 unless stated otherwise. Original repositories are not
repacked. Reports retain pinned binaries and source patches.

## Consolidated pack-size results

All cells are whole-pack **MiB**. Blank means not measured or not yet
successfully validated. These are historical stages, not interchangeable
settings on one binary. `T` means trees, `B` blobs, `P` predecessor hints,
`S` skipping, and `R` improvement-only recompression. Unmentioned types
retain ordinary size ordering.

<!-- pack-size-results:start -->
<!-- This section is refreshed from the session's benchmark reports. -->
| Experiment / stage | Fluent UI | Git | Node | Azure DevOps Docs | Linux |
|---|---:|---:|---:|---:|---:|
| Original --path-walk baseline [A] | 154.75 | 208.43 | 829.43 | 1498.95 | 2346.17 |
| Baseline + tagged-tree correctness fix | 154.81 | 208.43 | 829.43 | 1498.95 | 2346.13 |
| Initial baseline, 1 thread [C] |  | 208.29 |  |  |  |
| Initial commit/T/B order [C] |  | 297.40 |  |  |  |
| Initial commit/T/B order + P [C] |  | 230.97 |  |  |  |
| Initial T/B order [C] |  | 248.03 |  |  |  |
| Initial T/B order + P [C] |  | 195.14 |  |  |  |
| Initial T/B order + P, 8 threads [A] | 181.28 | 195.15 | 905.39 | 1508.27 | 2331.44 |
| Size order + commit/T/B predecessors [H] | 150.54 | 197.28 | 829.46 | 1498.75 | 2118.46 |
| Historical best per repository [H] | 145.79 | 193.52 | 829.43 | 1498.75 | 2099.42 |
| Historical best + B order [H] | 181.25 | 194.91 | 917.04 | 1507.54 | 2268.38 |
| Fixed metadata: T order | 149.21 | 247.78 | 838.40 | 1502.53 | 2384.02 |
| Fixed metadata: T order + P | 148.81 | 200.55 | 836.00 | 1500.08 | 2204.22 |
| Fixed metadata: T/B order | 182.58 | 247.90 | 921.84 | 1511.04 | 2510.01 |
| Fixed metadata: T/B order + P | 181.72 | 194.96 | 904.29 | 1508.43 | 2335.60 |
| Depth 250: T/B order | 171.78 |  |  |  |  |
| Depth 250: T/B order + P | 164.09 |  |  |  |  |
| Skip study: T order, S off | 149.22 | 247.83 |  |  |  |
| Skip study: T/B order + P, S off | 181.72 | 194.97 |  |  |  |
| Skip study: T order, S=B | 149.17 | 247.45 |  |  |  |
| Skip study: T/B order + P, S=B | 173.38 | 193.08 |  |  |  |
| Hash-first diagnostic: T/B + P, S off | 170.82 |  |  |  |  |
| Hash-first: T/B + P, S=T/B | 163.66 | 184.82 | 853.56 | 1499.48 | 2175.08 |
| Current matched baseline: T + P, S=T/B | 147.82 | 193.19 |  |  |  |
| Recompression: T + P, S=R=T/B | 146.42 | 190.99 |  |  |  |
| Current matched baseline: T/B + P, S=T/B | 163.64 | 184.82 |  |  |  |
| Recompression: T/B + P, S=R=T/B | 163.51 | 184.65 | 853.04 | 1499.42 | 2172.91 |
| Window 50: T + P, S=T/B, R off | 137.80 | 170.20 |  |  |  |
| Window 50: T/B + P, S=T/B, R off | 159.41 | 177.26 |  |  |  |
| Size blobs + T/B P: T order, window 10, S=T/B | 144.85 | 186.25 |  |  |  |
| Size blobs + T/B P: T order, window 50, S=T/B | 136.99 | 167.73 | 803.92 | 1495.72 | 1959.35 |
<!-- pack-size-results:end -->

`[A]` predates the tagged-tree correctness fix. Its Linux primary packs omit
four reachable blobs, so those two Linux cells are historical, incomplete
results, **not valid compression wins**; they exclude the auxiliary repair
pack. The next baseline row includes the fix and all reachable objects.
`[C]` uses one thread, unlike the other rows. `[H]` belongs to the earlier
170-run sweep, before the predecessor-expansion invariant fix. Its winner
rows use each repository's own winning policy, potentially including commit
experiments. The detailed winning configurations are in section 2.

Baseline delta chains can exceed the requested depth 50; experimental
depth accounting enforces the cap. Depth-250 and window-50 rows explicitly
change those settings. Small repeated-run differences can come from threading.
The session-local `pack-size-results.csv` records exact bytes, settings,
completion state, and the source report for every cell. The generated table
refreshes when the active benchmark reports publish completed results.

All currently planned runs are complete. The strongest measured common
configuration is chronological trees, predecessor hints and skipping for
trees/blobs, size-ordered blobs, window 50, and no recompression. This is
not an ablation proving every component necessary, and its extra CPU cost
can be substantial.

## 1. Historical ordering and predecessor metadata

The API assigns increasing integer IDs in parent-before-child topological
order. A commit's predecessor is its first parent. Newly discovered root trees
inherit the introducing commit's ID and previous root tree; newly discovered
children inherit their tree's ID and the previous tree's same-name child.
Shared object IDs retain their first discovery metadata. These IDs are neither
timestamps nor positions in a single linear history for a path.

The consumer experiments with two independent uses of this information:

| Mechanism | What changes |
|---|---|
| Chronological ordering | Process older IDs first; tracked delta bases must have smaller positive IDs. |
| Predecessor hints | Try an eligible historical predecessor before searching the ordinary window. |

A predecessor is a hint, not a pinned delta base: the window may replace it
during that object's search. It must have been processed in the same packing
region. A missing, self, incompatible-type, or ineligible predecessor is not a
usable base.

The committed narrative after `path-walk-tree-tags` is:

| Commit | Experiment introduced |
|---|---|
| `f9ccf93188` | Optional API metadata, including cross-path predecessor expansion. |
| `9dc7511975` | Chronological tree/blob delta selection and depth bookkeeping. |
| `96f2f130e3` | Predecessor-first delta search. |
| `6458507a37` | Independent per-object-type selection of both mechanisms. |
| `7f717137e6` | Bounded skipping through selected delta bases. |
| `f58cd5c557` | Name-hash-first chronological fallback. |
| `c2c26dc12c` | Opt-in improvement-only recompression. |

These commits reconstruct the tested implementation option by option; each
builds and passes the pack-object and path-walk suites independently.
Correctness fixes are folded into their introducing commits rather than
leaving broken intermediate states. Historical reports retain their original
source IDs and patches, so their IDs need not match this rewritten series.

## 2. Independent object-type policies

Current experimental controls accept comma-separated subsets of
`commit,tree,blob`, or `none`:

| Control | Meaning |
|---|---|
| `GIT_TEST_PACK_PATH_WALK_ORDER` | Enable chronological selection for these types. |
| `GIT_TEST_PACK_PATH_WALK_PREVIOUS` | Enable predecessor hints for these types. |
| `GIT_TEST_PACK_PATH_WALK_SKIP` | Enable selected-delta-base skipping for these types; defaults to the predecessor types. |
| `GIT_TEST_PACK_PATH_WALK_RECOMPRESS` | Opt into improvement-only recompression for these types with `-f`; defaults to `none`. |

Predecessors can be enabled **without** chronological sorting, retaining normal
size ordering. Skipping can be enabled without either other control and does
not itself require historical metadata. Tags retain ordinary selection.

The earlier `GIT_TEST_PACK_PATH_WALK_VERSIONS` control supplies defaults: 0
disables ordering/predecessors, 1 orders trees and blobs, and 2 also enables
their predecessor hints. With the current implementation, mode 2 consequently
also enables their default skipping. Explicit per-type controls override these
defaults; use `GIT_TEST_PACK_PATH_WALK_SKIP=none` to reproduce pre-skipping
measurements.

The runner encodes configurations as `order_mask | (previous_mask << 3)`, with
commit=1, tree=2, blob=4. There were initially 64 combinations. A predecessor-only
case exposed self-predecessors and a writer stall; that defect was fixed before
continuing.

An earlier sweep tested all 32 combinations without blob ordering on all five
repositories, followed by winner confirmations with and without blob ordering
(170 repacks). **These preceded the later cross-path predecessor-expansion
fix**, so they are historical findings, not current recommended settings:

| Repository | Winning order types | Winning predecessor types | Confirmed MiB | Adding blob order |
|---|---|---|---:|---:|
| Fluent UI | tree | commit,tree,blob | 145.79 | +24.33% |
| Git | tree | commit,tree,blob | 193.52 | +0.71% |
| Node | none | none | 829.43 | +10.56% |
| Azure DevOps Docs | none | commit,tree,blob | 1498.75 | +0.59% |
| Linux | commit | commit,tree,blob | 2099.42 | +8.05% |

Commit ordering can materially affect results, especially Linux. Later sweeps
deliberately exclude experimental commit ordering and predecessor hints.
Commits still exist in the packs and supply topology metadata.

## 3. Correctness fixes, not compression options

| Fix | Why it matters |
|---|---|
| Tagged-tree path requeueing, `af9af0a0eb3` on `path-walk-tree-tags` | Prevents reachable objects from being omitted when an already consumed path appears through a tree tag. An incomplete pack is not a compression win. |
| Reject an object as its own delta base | Prevents self-predecessor metadata from creating recursive deltas. |
| Expand queued predecessor trees before their dependents, including across paths | Ensures earlier children are discovered before assigning new children's metadata. Sorting only the current path batch was insufficient for reused trees. |
| Preserve pruning/sparse behavior and break partial-history predecessor cycles | Keeps dependency expansion within the requested walk and prevents cyclic metadata dependencies. |
| Track path-pass depths and rebuild delta child links before fallback | Reserves depth for existing descendants when fallback deltifies their full base. |

Baseline, untracked path-walk packs in earlier measurements can exceed the
requested depth 50; the experimental depth
accounting enforces it. Baseline comparisons therefore do not always impose
identical effective depth constraints.

After fixing predecessor expansion, the narrowed sweep reran exactly four
policies, without commit experiments or delta skipping:

| Repository | Tree order | Tree order + prev | Tree/blob order | Tree/blob order + prev |
|---|---:|---:|---:|---:|
| Fluent UI | 149.21 | 148.81 | 182.58 | 181.72 |
| Git | 247.78 | 200.55 | 247.90 | 194.96 |
| Node | 838.40 | 836.00 | 921.84 | 904.29 |
| Azure DevOps Docs | 1502.53 | 1500.08 | 1511.04 | 1508.43 |
| Linux | 2384.02 | 2204.22 | 2510.01 | 2335.60 |

The API fix restored the intended metadata invariant but did not make
chronological blob compression universally effective.

## 4. Delta direction, window locality, and depth experiments

Fluent UI analysis used the same 342,728 blobs under three policies:

| Blob policy | Blob storage MiB | Full blobs |
|---|---:|---:|
| Size ordering | 102.49 | 45,529 |
| Chronological ordering | 135.86 | 72,759 |
| Chronological + predecessors | 135.40 | 73,804 |

Changelogs accounted for about 71% of the net blob-byte regression. Size
ordering often uses a larger, newer file as a shallow base for copy-only
backward deltas. Strict older-base selection forbids those choices.
Topological adjacency also does not imply first-parent/content adjacency:
predecessor hints can recover useful bases outside the window.

Depth is a real contributor, but **not the only explanation**. One changelog
changed from a 92-byte backward delta to a 338,712-byte full object because its
correct predecessor was at depth 50. Raising the cap to 250 recovered a
239-byte delta at depth 246. Across the repository, that higher cap reduced
chronological/predecessor blob storage to 119.35 MiB, still above size ordering.
These were diagnostics, not a recommendation to ship deeper chains.

A separate ancestor-skipping probe found direct delta payloads of 661 bytes,
14,418 bytes, and 16,643 bytes at resulting depths 50, 41, and 26 for that same
full object. Following selected delta-base links reached those candidates in
1, 10, and 25 hops; historical predecessor links required 3, 102, and 117 hops.

## 5. Delta-base skipping

The implementation follows the **selected delta graph**, starting
from a saturated predecessor or window candidate. It tries shallower ancestors
until a delta is accepted, 64 links are visited, or accumulated raw delta sizes
exceed one eighth of the target size. That sum is a search heuristic, not a
bound on the resulting compressed delta. Saturated window entries are retained
as hints to their ancestors. Region, processed-object, rank, and depth checks
remain in force.

Matched runs toggled **blob skipping only**, before the fallback sorting fix:

| Repository | Blob policy | Skipping off MiB | Skipping on MiB |
|---|---|---:|---:|
| Fluent UI | Size ordering | 149.22 | 149.17 |
| Fluent UI | Order + predecessors | 181.72 | 173.38 |
| Git | Size ordering | 247.83 | 247.45 |
| Git | Order + predecessors | 194.97 | 193.08 |

Skipping helps size-ordered search too, but the measured gains were small.
Chronological/predecessor packing took roughly 15-22% longer in these runs.

## 6. Second-pass candidate selection and name-hash sorting

The original fallback excluded already-deltified objects as **both targets and
candidate bases**. It did not break path-pass links: snapshots and actual-pack
checks found zero replacements or drops after the path pass in all three
Fluent UI policies.

However, the chronological comparator put **rank before filename hash** in the
global fallback, losing useful name-hash locality:

| Blob policy | Path-pass deltas | Deltas added by fallback |
|---|---:|---:|
| Size ordering | 268,114 | 29,076 |
| Chronological | 265,415 | 4,554 |
| Chronological + predecessors | 264,371 | 4,553 |

An isolated change to type/hash/rank fallback sorting preserved all 264,371
path-pass links and the older-base/depth restrictions, increased fallback blob
deltas to 18,143, and reduced the pack from 181.72 to 170.82 MiB. Skipping was
disabled in that comparison.

Among 43,078 blobs that were size-order deltas but chronological/predecessor
full objects, only 949 encountered a hard depth rejection or descendant-depth
block. Other misses involved rank restrictions, locality, and size heuristics.
Some size thresholds are depth-scaled even below the hard limit. A concrete
non-depth miss became an 886-byte delta instead of a 41,000-byte full changelog
solely by restoring hash locality; its original candidates were depth zero.

The fallback sorting correction is in the normal experimental
implementation, not just the diagnostic binary. The path pass remains
rank-first; only the global fallback becomes hash-first.

The completed all-repository rerun uses tree/blob ordering, predecessors, **and
tree/blob skipping**, with no commit experiments:

| Repository | Fixed fallback + skipping MiB |
|---|---:|
| Fluent UI | 163.66 |
| Git | 184.82 |
| Node | 853.56 |
| Azure DevOps Docs | 1499.49 |
| Linux | 2175.08 |

These are not an isolated comparison against the earlier no-skipping
four-policy sweep: both skipping and fallback sorting have changed. All five
packs are smaller than the earlier tree/blob order + predecessor packs, but
Fluent UI and Node still exceed their earlier tree-only results.

## 7. Improvement-only recompression prototype

The agreed objective is **strictly smaller compressed delta payloads with no
increase in chain depth**, only when recomputing deltas (`-f` /
`--no-reuse-delta`). Existing deltas must remain incumbents, not be discarded
before searching. No improved candidate means retaining the old base and data.

The prototype is opt-in through
`GIT_TEST_PACK_PATH_WALK_RECOMPRESS=tree,blob` (or another type subset). It
adds a separate improvement sweep after the ordinary fallback rather than
discarding or recomputing its results unconditionally. Existing deltas are
admitted as both targets and bases; full objects remain full.

The sweep snapshots every object's depth. Each replacement must point to a
strictly lower snapshot depth and must not increase the target's current
window depth. This certifies acyclicity and bounds final depths without
mutating shared child lists during the threaded search. A final whole-graph
check verifies that no object became deeper. This is conservative: a source
that becomes shallow during the sweep can still be excluded by its old depth.
Chronological rank restrictions remain active.

Candidate deltas are compressed before acceptance, including candidates whose
raw instruction streams are larger than the incumbent's. Equal compressed
sizes retain the incumbent, even if a candidate would shorten the chain.
Compressed payload excludes pack reference headers, so total pack bytes can
also vary with write order. Git's ordinary depth-adjusted raw-delta heuristic
alone does not enforce this objective.

The first matched benchmarks compare size-ordered and chronologically ordered
blobs on Fluent UI and Git using the same pinned binary. Tree ordering and
predecessors, plus tree/blob skipping, remain enabled in every row; commit
experiments remain disabled. Chronological blobs also enable blob predecessors.

| Repository | Blob policy | Off MiB | On MiB | Reduction | Off/on seconds |
|---|---|---:|---:|---:|---:|
| Fluent UI | Size order | 147.82 | 146.42 | 0.94% | 31.0 / 103.5 |
| Fluent UI | Order + predecessors | 163.64 | 163.51 | 0.08% | 42.8 / 153.3 |
| Git | Size order | 193.19 | 190.99 | 1.14% | 91.2 / 117.1 |
| Git | Order + predecessors | 184.82 | 184.65 | 0.09% | 95.0 / 121.4 |

This improves size ordering more than chronological ordering, rather than
closing Fluent UI's gap. All five chronological cases are complete:

| Repository | Off MiB | On MiB | Reduction |
|---|---:|---:|---:|
| Fluent UI | 163.64 | 163.51 | 0.0802% |
| Git | 184.82 | 184.65 | 0.0933% |
| Node | 853.56 | 853.04 | 0.0613% |
| Azure DevOps Docs | 1499.48 | 1499.42 | 0.0044% |
| Linux | 2175.08 | 2172.91 | 0.0996% |

Fluent UI/Git use fresh matched off runs; the other three use the earlier
hash-first/skipping baseline. Millions of additional window candidates yield
relatively small compressed-payload gains. Linux considered 92.8 million
window candidates and improved 138,929 tree/blob targets. Full anchors cannot
be improved by this conservative prototype. Packs are retained for further
blob-direction and chain analysis.

The window-50 comparison is complete, with recompression disabled:

| Repository | Blob policy | Window 10 MiB | Window 50 MiB | Reduction |
|---|---|---:|---:|---:|
| Fluent UI | Size order | 147.82 | 137.80 | 6.77% |
| Fluent UI | Order + predecessors | 163.64 | 159.41 | 2.59% |
| Git | Size order | 193.19 | 170.20 | 11.90% |
| Git | Order + predecessors | 184.82 | 177.26 | 4.09% |

The larger window helps size ordering more, reversing Git's winner between
these two policies. Window size changes all object types: blob bytes account
for about 96% of Fluent UI's size-ordered saving and 84% of Git's. This study
overlaps the later recompression repositories, so those wall times are not
isolated; the four matched recompression pairs above ran sequentially.

## 8. Delta-forest investigation and hybrid candidates

The retained Fluent UI packs reveal genuinely different graph shapes.
With size ordering, 80.79% of deltified objects use bases with multiple
dependents, versus 24.83% with chronological ordering. Mean depth is 7.41
versus 15.30. A size-ordered family spans 15,612 objects across 178 canonical
paths: shared anchors make independent, private-root accounting per path
misleading.

Growing changelogs favor content-superset bases and copy-only backward deltas.
Locks can favor edit lineage instead: chronological `yarn.lock` deltas save
6,921,907 payload bytes despite paying 1,047,711 more root bytes. Small
20-object trials also found reverse chronology could beat both alternatives;
this is not evidence for a filename-based policy.

Validated forest simulations, **not whole-repository repacks**, found:

* Rank-preserving root grafts saved 1,138,423 compressed payload bytes;
  relaxing rank saved 3,623,108. Most tempting grafts instead create cycles.
* A complete 396-object family rewrite saved 384,938 payload bytes, with
  maximum depth 49, by retaining already-paid external bases. Its new edges
  require relaxing chronological rank.
* A 463-object rewrite needed a shared checkpoint promotion to meet depth 50,
  yielding a net 162,664-byte saving. Blind checkpoint insertion lost space.

These mutations enforce acyclicity and maximum depth, not the current
recompression prototype's stronger per-object nonincreasing-depth guarantee.
Candidate edges were borrowed from an already-computed size forest, so their
discovery cost and production implementation remain open.

The existing-knob experiment keeps blobs size-ordered but enables their
predecessor hints: order `tree`, previous `tree,blob` (code 50). With
tree/blob skipping enabled and recompression disabled, the matched results are:

| Repository | Window | Without blob P MiB | With blob P MiB | Reduction |
|---|---:|---:|---:|---:|
| Fluent UI | 10 | 147.82 | 144.85 | 2.01% |
| Git | 10 | 193.19 | 186.25 | 3.59% |
| Fluent UI | 50 | 137.80 | 136.99 | 0.59% |
| Git | 50 | 170.20 | 167.73 | 1.45% |

Almost all savings are in blobs. The window-50 extension completed for Node
at 803.92 MiB, Azure DevOps Docs at 1495.72 MiB, and Linux at 1959.35 MiB.
Linux is 16.49% smaller than its correctness-fixed baseline of 2346.13 MiB,
but packing took 52.2 minutes versus that baseline's 9.7 minutes.
The Linux runs were serialized to avoid concurrent memory-heavy repacks.
The other repositories do not yet have matched window-50 baselines
without blob hints, so their gains cannot be attributed to hints alone.
Longer-term
options are bounded competing trial forests, growth/inclusion and lineage
sampling, and transactional rewrites with fixed external boundaries.

## Reports and remaining questions

Raw reports are session-local artifacts, not tracked repository files. Paths
below are relative to the session's `files/path-walk-benchmark/` directory:

| Artifact directory | Scope |
|---|---|
| `per-type-no-blob-order/` | Earlier 170-repack sweep and winner confirmations. |
| `per-type-predecessor-order/` | Completed 20-case sweep after the metadata invariant fix. |
| `fluentui-delta-analysis/`, `fluentui-depth250/` | Retained packs, object/chain analysis, and depth diagnostics. |
| `delta-skipping-off/`, `delta-skipping-on/` | Matched eight-repack blob-skipping comparison. |
| `fluentui-phase-analysis/` | Phase snapshots, rejection reasons, and isolated hash-first fallback comparison. |
| `fallback-hash-prev-skip/` | Completed five-repository hash-first + tree/blob skipping rerun. |
| `recompression-off/`, `recompression-on/` | New recompression comparisons and retained packs. |
| `recompression-analysis/` | Blob representation comparisons and direct bidirectional delta probes. |
| `window50/` | Wider-window comparison for size-ordered and chronological blobs. |
| `size-blob-predecessors-window10/`, `size-blob-predecessors-window50/` | Size-ordered blob predecessor-hint comparisons. |
| `forest-study/` | Branching, directional probes, small trial forests, and validated mutation simulations. |

These directories contain JSON/CSV results and pinned source provenance;
diagnostic directories additionally retain their instrumentation and packs.
Small size differences between repeated runs can arise from threaded fallback.

Not yet measured: matched window-size baselines for the other three
repositories, and whether allowing newer bases would outperform
strict chronological direction. No full
reverse-chronological policy sweep has been run. A separately supplied
commit/tree-only workload is not locally available and is not included in the
reproducible tables above.
