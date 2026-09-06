# `unify::session` — the core boundary

Companion to [`../lens-text-mode-environment.md`](../lens-text-mode-environment.md).
Deliverable D1: the complete surface between any Unify front end and any
Unify core — **15 requests, 12 events, one value model**.

Design brief (plan requirement (c)): lean and small, and shaped so that
replacing the core with a BEAM implementation, or putting a network between
the two, changes an implementation and not a single caller.

> **Implementation note (2026-09-06).** The header is
> `unify/include/vault-unify-session.hpp` and its namespace is
> `vault::unify::session`, not the `unify::session` written below. The rest
> of the codebase lives in `vault::unify`, and a second top-level `unify`
> namespace beside it would be a genuine ambiguity trap for any code inside
> `namespace vault`. Nothing else in this document changes: the request
> list, the event list and the value model are implemented as specified.
> `solve()` is worth one clarification it did not spell out — it returns a
> `QueryId` rather than a `RequestId`, so it emits no `Started`; its
> acknowledgement is its first `QueryStatus`, which with `initialDemand = 0`
> is the only event until a `demand` arrives.

> **Revision note (2026-09-06, after architecture review).** The first draft
> claimed 13 requests and 7 events and was not closed over its own
> acceptance gates: it had no debug/trace event, no way to expand a
> truncated value, no query-lifetime verb, and no query attribution on
> `Output`/`Diagnostic` — the last of which is unfixable-without-a-break
> once a BEAM core runs two queries at once. Those four holes are closed
> below, at the cost of two requests and five events. Better a 15-request
> API that closes than a 13-request one that has to break.

---

## 1. The five invariants

Every element below serves one of these. Anything serving none does not
belong in the boundary.

1. **Values only.** No engine type, no pointer into engine memory, ever
   crosses. Handles are opaque integers, session-scoped.
2. **Asynchronous and correlated.** Requests return an id and nothing else;
   answers arrive as events. No call blocks.
3. **Pull, not push, for every unbounded producer.** Solutions *and* trace
   steps are demand-driven. Program output cannot be (nobody demands a
   `print`), so it is budgeted and coalesced instead — see §4.1.
4. **One ordered event stream**, with a monotonic `seq`, plus a per-query
   subsequence so a front end can order one query's events without
   depending on the global order surviving a concurrent core.
5. **Generation-stamped.** Every event carries the `worldGeneration` it was
   produced under, so a view knows it is stale without asking.

## 2. Interface

```cpp
namespace unify::session {

using RequestId = uint64_t;   // opaque, session-scoped, monotonic
using QueryId   = uint64_t;   // opaque, session-scoped
using ModuleId  = uint64_t;   // opaque, session-scoped
using Seq       = uint64_t;   // global event sequence, monotonic from 1
using Gen       = uint64_t;   // world generation (engine mutation counter)

enum class Stream { Solutions, Trace };

class Session {
public:
    virtual ~Session() = default;

    // -- lifecycle -----------------------------------------------------
    virtual Capabilities describe() const = 0;          // the one sync call
    virtual void         close()          = 0;
    virtual void         subscribe( EventSink&, Seq resumeFrom = 0 ) = 0;

    // -- program -------------------------------------------------------
    virtual RequestId define   ( std::string text, Origin origin,
                                 OverwritePolicy policy ) = 0;
    virtual RequestId undefine ( PredicateKey key, ModuleId scope ) = 0;
    virtual RequestId listing  ( ListingFilter filter ) = 0;
    virtual RequestId source   ( PredicateKey key ) = 0;

    // -- queries -------------------------------------------------------
    virtual QueryId   solve    ( std::string goalText, QueryOptions opt ) = 0;
    virtual RequestId demand   ( QueryId, Stream, uint32_t n ) = 0;
    virtual RequestId cancel   ( QueryId ) = 0;
    virtual RequestId release  ( QueryId ) = 0;
    virtual RequestId inspect  ( QueryId, uint64_t solutionIndex,
                                 ValuePath path, ValueBudget budget ) = 0;

    // -- image ---------------------------------------------------------
    virtual RequestId save     ( std::string path, SaveOptions opt ) = 0;
    virtual RequestId load     ( std::string path ) = 0;
    virtual RequestId insert   ( std::string path, OverwritePolicy policy ) = 0;

    // -- debug (single verb, behind a capability flag) -------------------
    virtual RequestId debug    ( DebugCommand cmd ) = 0;
};

} // namespace unify::session
```

`describe()` is the sole synchronous call, and only because it must be
answerable before any request is legal — a proxy answers it from the
handshake it completed while connecting, not from a round trip.

`subscribe` takes `resumeFrom` so reconnection is part of the interface
rather than a proxy-private trick (§5).

### 2.1 Why each request exists

| Request | Serves | Why it is not something else |
| --- | --- | --- |
| `define` | (b) insert, and all editing | The single ingestion path. A transcript line, a file, an edited predicate and an `insert` are one operation with different `Origin`/`OverwritePolicy`; collapsing them is most of what keeps the API small |
| `undefine` | editing, `ReplacePredicates` | "Define nothing for this key" cannot express deletion. Engine item **E3** (predicate-scoped retract) |
| `listing` | browser | Catalogue *metadata* only. The browser shows thousands of keys and the source of none of them |
| `source` | source panel | One definition's text is a rarer, larger payload than `listing`. Splitting them is the difference between a browser that opens instantly over a socket and one that does not |
| `solve` | queries | `QueryOptions` carries `initialDemand` (first screen in one round trip), the value `budget`, and `trace` |
| `demand` | invariant 3 | Takes a `Stream` so solutions and trace steps share one back-pressure mechanism instead of needing two verbs |
| `cancel` | partial failure | Best-effort in v1 — plan §6 |
| `release` | **added in revision** | Demand-driven delivery means the core retains undelivered solutions and (for `inspect`) their term trees. Without an explicit release, a session leaks one retained query per query run. `close` releases all. Engine item **E15** |
| `inspect` | **added in revision** | Expands a `truncated` value node by path. Re-running the query to see a term is wrong: queries are nondeterministic and may have side effects. Needs the solution term trees retained past job completion — engine item **E15**, since `~SolveJob` frees them today |
| `save`/`load`/`insert` | requirement (b) | Paths resolve **core-side** (§7) |
| `debug` | debugging | One verb over a `DebugCommand` variant (`SetTrace`, `AddBreakpoint`, `RemoveBreakpoint`, `Step`, `StepOver`, `Continue`, `StackAt`) rather than seven methods. Behind `Capabilities.debug` |
| `describe`/`close`/`subscribe` | lifecycle | |

Rejected, deliberately: any request returning a value directly; anything
named `eval`; a "wait until idle" call (see
[ARCHITECTURE.md](ARCHITECTURE.md) §3); and any access to `World`,
`Clause`, `UnifyContext` or `SolveJob` in any form.

## 3. The value model

A self-contained tree — no interning, no back-references, no pointers into
a world — because on BEAM it is a copied message and over a socket it is
bytes.

```cpp
struct Value {
    enum class Kind { Atom, Int, Float, Str, Var, Cons, Array, Map };
    Kind                              kind;
    std::string                       name;   // Atom | Str | Var | Cons functor
    int64_t                           i = 0;
    double                            f = 0;
    std::vector<Value>                args;   // Cons | Array
    std::vector<std::pair<std::string,Value>> pairs;  // Map
    bool                              truncated = false;
};
```

It mirrors the engine's term kinds (`ConsTerm`, `VarTerm`, `MapTerm`,
`ArrayTerm`, atoms, numbers) one-for-one, which makes the `LocalSession`
conversion mechanical and the BEAM mapping obvious (tuple, integer, float,
binary, unbound marker, list, map).

`truncated` is load-bearing. A binding may be enormous; a 40-row screen
cannot show it and a socket should not carry it. The core truncates at the
depth/size budget carried in `QueryOptions` and marks the node; the front
end renders an ellipsis and calls `inspect(q, index, path, budget)` to go
deeper. `Var` carries the variable's display name for unbound bindings —
a Prolog front end that loses this is unusable for the debugging cases that
matter most.

### 3.1 Two interim degradations, both stated as such

**Structured solutions.** Until ROADMAP Phase 1's open item lands,
`SolveJob::getSolutionList()` yields `map<string,string>`
(`vault-unify-solvejob.hpp:92`), so `LocalSession` emits `Kind::Str` leaves
and `inspect` answers `Failed{"structured solutions unavailable"}`. An API
difference this is not; only the adapter and the inspector's richness
change. Gate G5 pins this as the floor so it cannot be mistaken for done.

**Demand.** `performSlice()` runs a query to completion in one call and
solutions materialise only at `onFinished`, so until ROADMAP Phase 3's
bounded slices *and* a per-solution emission hook (engine item E11) land,
`demand` drains a buffer the core already computed in full. The API is
right; the back-pressure is not yet real. This is the same class of
half-truth as `cancel`, and it gets the same treatment: the status line
says "buffered" rather than implying flow control, and G0.6 asserts the
observable contract without claiming the producer was throttled.

## 4. Events

```cpp
struct EventHeader {
    Seq  seq;                 // global, monotonic, gapless
    Gen  worldGeneration;
    std::optional<QueryId> query;   // set on every query-attributable event
    uint64_t querySeq = 0;          // per-query subsequence when query is set
};

// request envelope
struct Started    { RequestId req; };
struct Failed     { RequestId req; std::string reason; bool fatal; };

// program
struct Defined    { RequestId req; ModuleId module;
                    std::vector<PredicateKey> added, replaced, removed;
                    uint32_t errorCount; };
struct Listing    { RequestId req; std::vector<CatalogueEntry> entries; bool more; };
struct SourceText { RequestId req; PredicateKey key; std::string text; Origin origin; };

// queries
struct Solution   { uint64_t index;
                    std::vector<std::pair<std::string,Value>> bindings; };
struct QueryStatus{ enum State { Running, Exhausted, Complete,
                                 Failed, Aborted, Blocked } state;
                    uint64_t produced; bool retained; std::string detail; };
struct Expanded   { RequestId req; ValuePath path; Value value; };

// debug
struct TraceEvent { enum Kind { Call, Exit, Redo, Fail, BreakpointHit } kind;
                    uint32_t depth; PredicateKey key; Value goal;
                    std::vector<StackFrame> stack;   // on BreakpointHit only
                    bool halted; };

// ambient
struct Output     { std::string stream; std::string text;
                    uint64_t droppedBytes; };
struct Diagnostic { std::optional<RequestId> req; Severity sev;
                    std::string file; int line, column;
                    std::string message, sourceLine; };
struct WorldChanged { Gen generation; uint32_t clauseCount, predicateCount; };
```

Twelve kinds. The five added in revision are `Expanded`, `TraceEvent`, and
the `query`/`querySeq`/`droppedBytes` attribution fields that turn `Output`,
`Diagnostic` and `QueryStatus` into things a concurrent core can emit
truthfully.

Requirements, not commentary:

- **Query attribution is on `EventHeader`, not on individual events.**
  Today a single FIFO worker makes "the output that just arrived belongs to
  the query that is running" accidentally true. On BEAM, where a query is a
  process, two concurrent queries interleave their `print` output and it
  becomes unattributable. Adding `QueryId` after the fact would change the
  wire format of the two highest-volume events, so it goes in now — one
  field, and the reason the API is worth having.
- `Diagnostic` carries file, line, column **and the offending source line**.
  The engine formats exactly this today but only as text on stderr
  (`reportParseError`, `vault-unify-runtime-context.cpp:99-102`) and returns
  callers nothing but an error *count*; runtime `UnifyError`s keep only a
  count and the last message (`getErrorCount()`/`getLastError()`,
  `vault-unify-solvejob.hpp:161,167`). Producing these structurally is real
  engine work — item **E10**, not adapter work, and the first draft was
  wrong to imply otherwise.
- `Output` exists because of engine item E4. A remote engine's `print`
  must arrive here; sent to the engine process's stdout it is lost on
  another machine. Note that today `print` writes `"print: " << s` to
  `std::cout` (`vault-unify-clause-builtin-print.cpp:28`); the default sink
  must reproduce that byte-for-byte or the engine goldens break.
- `QueryStatus::retained` tells the front end whether `inspect` is still
  possible and whether `release` is owed.
- `QueryStatus::Blocked` is reserved for ROADMAP 5.3's async builtins.
  Defining it now costs nothing; a v1 core never emits it.
- `WorldChanged` lets every view invalidate on one signal instead of each
  panel re-querying after every `define`.

### 4.1 Output is budgeted, not demanded

`print` in a loop is the likeliest runaway producer in the system, and
nobody can "demand" output. So the core applies a rate/size budget per
query, coalesces adjacent writes into one event, and reports what it
discarded in `droppedBytes`; the front end renders `… 4.2 MB elided`. The
first draft exempted `Output` from invariant 3 and said nothing — which
would have made the single most likely flood the one unhandled case.

## 5. Ordering, and an honest note about it

`seq` is globally monotonic and gapless within a session; `querySeq` orders
one query's events independently.

**The single ordered stream is a front-end simplicity choice, not a
consequence of BEAM.** The first draft argued the opposite — that
pairwise-only message ordering on BEAM *implies* one stream — and that is
backwards: a gapless total order across concurrent query processes requires
funnelling everything through one serialising process, which is the
unbounded-mailbox shape the same document warns against. The real
justification is that a tiled terminal UI folding one ordered stream is
dramatically simpler to test and reason about than one merging N channels,
and `querySeq` means a future core may relax the global order to
per-query order without breaking any front end that only relies on the
latter. Head-of-line blocking is accepted at v1 volumes and mitigated by
`Listing.more` paging and §4.1's output budget.

Reconnection: `subscribe(sink, resumeFrom = seq)` replays from the core's
bounded buffer, or answers `Failed{"resume point expired"}`, after which
the front end does a full `listing` refresh. Both paths are exercised by
`FakeSession`, which drops the connection mid-query in every gate's tests.

In-process all of this costs a counter increment. That is the point: the
expensive-looking parts are free locally and load-bearing remotely, so
nobody deletes them for local performance.

## 6. Errors

Three kinds, kept distinct because the environment presents them
differently and merging them is the usual mistake:

| Kind | Carried by | Presented as |
| --- | --- | --- |
| **User error** — parse error, unknown predicate, `UnifyError` | `Diagnostic` (+ `Defined.errorCount`) | Diagnostics panel, navigable to source |
| **Query outcome** — no solutions, exhausted, cut short | `QueryStatus` | Transcript status, unalarming |
| **Infrastructure** — connection lost, engine crashed, request rejected | `Failed` (`fatal` distinguishes "this request" from "this session") | Status line; session marked degraded |

A no-solutions query is not an error; a dropped socket is not a diagnostic.
`UnifyError` maps to `Diagnostic{Severity::Error}` and increments the
query's error count without aborting it — matching what `unify-run` already
does with `getErrorCount()`.

## 7. Paths, and the daemon's trust boundary

`save`/`load`/`insert` paths resolve **on the core's filesystem**. With
`--connect`, an image saved from lens lands on the engine's machine; the
image panel labels the path `local` or `remote:<host>`. Streaming file
contents across the boundary instead would add a file-transfer concern to
the API and make `save` on a large world a front-end memory problem.
Moving images between machines is `scp`'s job.

That makes `unify-lensd` a service that accepts `define` — which can
`import` any file the daemon can read, and, in the vault lineage this
engine comes from, eventually drive actuators. So, stated as a requirement
rather than left to be discovered:

- **Binds to loopback by default.** A non-loopback bind requires an
  explicit `--listen` and refuses to start without a credential.
- **A shared-secret token** on connect, in `describe()`'s handshake.
- **TLS is out of scope for v1**; the supported remote story is a
  loopback bind plus an SSH tunnel, and the daemon says so in `--help`.
- G7 gates all three. A network daemon with an `eval`-equivalent and no
  authentication statement is not something to leave implicit.

## 8. Contract suite (D7)

One suite, three subjects: `LocalSession`, `FakeSession`, `ProxySession`.
A boundary feature exists when it passes on all three. Representative
obligations:

- `define` of a bad program yields `Diagnostic`s *and* a `Defined` with a
  non-zero `errorCount` — never silence, never only one of the two.
- `solve` with `initialDemand = 3` on a 10-solution goal yields exactly 3
  `Solution`s, then `QueryStatus::Running`, and nothing more until `demand`.
- Events are strictly increasing and gapless in `seq`; `querySeq` is
  gapless per query; no `Solution` ever follows its query's terminal
  `QueryStatus`.
- Every event that could have been caused by a query carries `query`.
  Asserted by running two queries concurrently against `FakeSession` and
  checking every `Output` and `Diagnostic` is attributed.
- `cancel` always produces a terminal `QueryStatus`, even when the core
  cannot actually stop the work.
- `release` after a terminal status makes `inspect` answer `Failed`, and
  the core's retained-query count returns to zero (asserted via
  `describe()`'s diagnostics counters).
- A 500 ms delayed reply hangs nothing and fires no front-end assertion.
- After an injected disconnect mid-query, resubscription resumes at `seq`
  or reports expiry; the front end reaches a consistent world view either
  way.
- On a core built without the xdebug backend, `describe()` reports
  `debug: false` and `debug(...)` answers `Failed` — never crashes.
- **Concurrent request safety:** `listing` and `define` issued while a
  query is running are answered, in bounded time, without corrupting the
  catalogue. See [ARCHITECTURE.md](ARCHITECTURE.md) §3.1 — this is the
  obligation that forces a subset of ROADMAP 5.1 into scope.
