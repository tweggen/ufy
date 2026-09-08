# E7 — Structured solution values

An implementation roadmap, written 2026-09-08 to be executed by agents one
phase at a time.

Every claim below carries a `file:line`. They were read at `765ac31` and
**none of the code described here has been written**; treat each anchor as
verified-by-reading and each design decision as a decision, not a discovery.

---

## 0. What is wrong today, in one paragraph

`vault-unify-session.hpp:98` defines a full value tree — `Atom`, `Int`,
`Float`, `Str`, `Var`, `Cons`, `Array`, `Map`, plus `truncated` — and
[SESSION-API.md](SESSION-API.md) §3 says it "mirrors the engine's term kinds
one-for-one, which makes the `LocalSession` conversion mechanical". Nothing
fills it. `SolveJob::getSolutionList()` calls `toString()` on each bound term
(`vault-unify-solvejob.cpp:151`) into a `map<string,string>`
(`vault-unify-solvejob.hpp:92`), and `LocalSession::onJobFinished()` wraps
that text as a `Str` leaf (`vault-unify-local-session.cpp:600-602`). So every
binding crosses the boundary as a string, whatever it really is.

This is declared rather than hidden — `Capabilities::structuredSolutions` is
`false` (`vault-unify-local-session.cpp:415`), one contract case skips on it
(`unify/test/session/contract-suite.cpp:953`), and `LocalSession` carries a
14-line comment saying so (`vault-unify-local-session.cpp:72-84`). It is
still a boundary promise that is not kept, and it has started costing:
lens's spec files are flat facts because a nested term would arrive as a
string the front end would have to re-parse
(`lens/test/spec/interaction.ufy`, the section headed "WHY FLAT FACTS").

**Done means:** `structuredSolutions` is `true` for `LocalSession`, the
contract suite's truncation case runs and passes for the `local` subject,
and [ACCEPTANCE.md](ACCEPTANCE.md) G5.4's stated floor is raised.

---

## 1. The one discovery that shapes everything

`resolveTermGrounded()` already exists, is already public
(`vault-unify.hpp:1126`, defined `vault-unify-terms.cpp:251`), and already
does the hard half. Given a term, the solved `UnifyContext`, and the term's
scope, it returns **a fresh tree with every variable resolved and nothing
pointing into the job's arena**. Its contract says so
(`vault-unify.hpp:1112`):

> every node returned is a fresh allocation, safe to outlive `pUCStackTop`'s
> own arena

And it is already used in exactly the shape E7 needs: `findall` grounds a
nested job's solutions and then lets that job's whole arena die
(`vault-unify-solvejob.cpp:856-869`).

Two consequences, and they are the reason this item is much smaller than it
looks:

1. **The term → `Value` walker needs no `UnifyContext` at all.** It walks an
   already-grounded tree. That makes it a pure function of a term, testable
   on hand-built terms with no engine, no query, no threads.
2. **The lifetime problem is solved by a function that already exists.**
   The comment at `vault-unify-local-session.cpp:578-582` — "EVERYTHING must
   be copied here" — stays true and stays satisfied.

Do **not** design around `deref()`. It is compiled out:
`vault-unify.hpp:104` reads `#define VAULT_UNIFY_USE_VIRTUAL_DEREF 0` and
every declaration sits inside that `#if`. `AbstractTerm::getBoundTerm()`
(`vault-unify.hpp:877`) is the live single-step equivalent, and
`resolveTermGrounded()` is the recursive one.

---

## 2. The decision this item cannot avoid: leaf typing

**The engine has no types at term level.** There are exactly four concrete
term kinds — `ConsTerm`, `MapTerm`, `ArrayTerm`, `VarTerm`
(`vault-unify.hpp:1331`, `:1133`, `:1546`, `:1246`), asserted as exhaustive
at `vault-unify-terms.cpp:230` and in `SPEC.md:186`. A number, a bareword
and a quoted string all parse to the **same** thing: a 0-arity `ConsTerm`
whose `Atom` name is the raw text. The grammar
(`vault-unify-parser.hpp:690-703`) gives all three alternatives the same
attribute type, and `AtomInput` (`vault-unify-parser.hpp:49`) carries only
`std::string name; int line;`. `1`, `red` and `"red"` are byte-identical
afterwards (`SPEC.md:70`).

So `Kind::Int` cannot be *recovered*; it can only be *decided*. The engine
already makes this decision, lazily and by string predicate, wherever it
cares: `parseInt64()` in `vault-unify-clause-builtin-arith.cpp:48-65`, used
by `evaluateArith` (`:163-172`) and by comparison (`:227-237`).

**The decision for E7, to be implemented exactly as stated:**

| Term | `Value::Kind` | Note |
| --- | --- | --- |
| `ConsTerm`, arity 0, name parses as `int64` by the *same* rule as `parseInt64` | `Int` (`i` set, `name` empty) | Reuse `parseInt64`; do not write a second rule |
| `ConsTerm`, arity 0, otherwise | `Atom` (`name` set) | |
| `ConsTerm`, arity > 0 | `Cons` (`name` = functor, `args`) | |
| `ArrayTerm` | `Array` (`args`) | |
| `MapTerm` | `Map` (`pairs`, in `std::map` key order — `vault-unify-term-map.cpp:222`) | |
| `VarTerm` | `Var` (`name` = `getOriginalVarName()`, or `VT<id>` when empty) | |
| anything else | must not happen; assert and produce `Atom` with the `toString()` text | |

`Kind::Float` is **never produced**: there are no floats in the language.
`m_ruleNumber` is `+ qi::char_("0-9")` and `SPEC.md:50` confirms "unsigned
digit runs only; no floats". `Kind::Str` is **never produced** either, since
quoting is lost at parse time. Both stay in the wire format for remote cores
and for the fake session, which does produce them.

Write this table into the walker's header comment. A future reader will
otherwise assume the missing kinds are a bug.

Note the two engine defects already recorded in `unify/ROADMAP.md` are
downstream of this same design: a negative literal loses its sign, and `[]`
does not parse. Neither is E7's to fix, and E7 must not paper over either.

---

## 3. Phases

Each phase is one agent's task. Each is independently reviewable, and each
leaves the tree green — `ctest` on `build/unify` and `build/lens` both at
100%, zero compiler warnings, `check-layering.sh` clean.

```
  E7.0 ───────────────────────────────────────► (independent, ship anytime)

  E7.1 ──┐
         ├──► E7.3 ──► E7.4 ──► E7.5 ──► E7.6
  E7.2 ──┘
```

E7.1 and E7.2 can run in parallel; they touch disjoint files.

---

### E7.0 — Nested variables resolve in the strings we already ship

**Independent of everything else. One line of code. Do it first.**

`SolveJob::getSolutionList()` renders each bound term with `toString()`
(`vault-unify-solvejob.cpp:151`), which has no `UnifyContext` and therefore
cannot dereference variables *inside* the bound term. A solution binding
`$x = f( $y )` where `$y` is itself bound today renders as `f( VT17 )`.
`toContextString( pUCStackTop, pUCTerm )` exists for exactly this
(`vault-unify.hpp:1017`) and is what `print` uses
(`vault-unify-clause-builtin-print.cpp:46`).

**Change:** at `vault-unify-solvejob.cpp:151`, replace

```cpp
std::string value = pTerm->toString();
```

with the context-aware form, passing `uc` as the stack top and
`spInstance->getUnifyContext()` as the term's scope — the same two arguments
`VarTerm::toContextString` threads (`vault-unify-term-var.cpp:21-61`).

**Test:** a new case in `unify/test/engine/` (new file, e.g.
`solution-values-test.cpp`, registered in `unify/test/engine/CMakeLists.txt`
alongside `unify-engine-provenance`): define `p( f( $y ) ) :- ...` such that
a query binds `$x` to a term containing a bound variable, run it through
`LocalSession`, and assert the delivered string contains the resolved value
and **not** `VT`.

**Blast radius:** `getSolutionList()` has exactly two callers —
`LocalSession::onJobFinished()` (`vault-unify-local-session.cpp:586`) and
`unify-repl.cpp:384`. `unify-run.cpp` deliberately does not call it
(`unify-run.cpp:74-76`), so **no engine golden file can change**. Verify
that claim by running the golden corpus and diffing.

**Also fix while you are in there** (both are in the three lines you are
touching, both are cheap, neither changes behaviour):

- `vault-unify-solvejob.cpp:140` leaves `InstanceId iid;` uninitialised.
  Every other call site writes `= 0` (`vault-unify-terms.cpp:61`,
  `vault-unify-term-var.cpp:40`).
- `vault-unify-solvejob.cpp:142`'s `if( res >= 0 )` is vacuous:
  `findVarBinding` returns only 1 or 0 (`vault-unify-unifycontext.cpp:139`).
  The real guard is line 149. Say so in a comment or drop the test — do not
  leave a reader thinking it is doing something.

---

### E7.1 — The walker: a grounded term becomes a `Value`

**New files.** `unify/src/vault-unify-term-value.hpp` / `.cpp`.

This is adapter code, not core: it is the only place that sees both
`vault-unify.hpp` and `vault-unify-session.hpp`, exactly as
`vault-unify-local-session.cpp` does. It must **not** be included by any
core translation unit, and gate G0.1's isolation target must stay green
(`unify/test/session/isolation-tu.cpp`).

```cpp
/** Convert a GROUNDED term tree into a session Value. */
vault::unify::session::Value toSessionValue( const AbstractTerm* pTerm );
```

Requirements:

- No `UnifyContext` parameter. The input is already grounded (E7.2 guarantees
  it). A `VarTerm` reaching this function is an *unbound* variable and
  becomes `Kind::Var`.
- Implement §2's table exactly. Reuse `parseInt64` — lift it out of the
  anonymous namespace in `vault-unify-clause-builtin-arith.cpp:48` into a
  shared internal header, or expose it; **do not copy it**, or the engine
  will have two definitions of "is a number" that can drift.
- Walk with `getArity()`/`getTermAt(i)` for `ConsTerm`, `size()`/
  `getElementAt(i)` for `ArrayTerm`, `getEntries()` for `MapTerm`
  (`vault-unify.hpp:1481`, `:1615`, `:1191`). Do **not** use
  `applyVisitor` — it is pre-order over a flat visit and gives you no
  structure. If you use `abstractTermIterator()` anywhere, `delete` the
  result: it is a factory (`vault-unify.hpp:360-368`), and a missing delete
  there is exactly the leak this project already fixed once.
- **Hard depth cap.** Neither `applyVisitor` nor `resolveTermGrounded` has a
  cycle guard, and a self-referential binding infinite-recurses. Cap the
  walk (suggested: 1000) and mark the node `truncated` at the cap rather
  than recursing further. This is defence in depth, not the budget —
  `applyBudget` is a separate, later stage (see E7.3).
- No truncation logic. `applyBudget` already runs downstream.

**Test:** in `unify/test/engine/solution-values-test.cpp` (the file E7.0
created). Build terms by hand — `ConsTerm`, `ArrayTerm`, `MapTerm`, `VarTerm`
constructors are all public and in-header — and assert the resulting `Value`
tree. Cover every row of §2's table, plus: a nested `Cons` inside an `Array`
inside a `Map`; a `ConsTerm` named `"-7"` (arithmetic produces these,
`vault-unify-clause-builtin-arith.cpp` comment at `:37-47`) becoming `Int`
with `i == -7`; an unbound `VarTerm` with and without an original name.

Free every hand-built term with `collectTermTree`/`deleteTermTree`
(`vault-unify.hpp:1082-1083`) so the test is ASan-clean — CI gates on leaks
being zero (`.github/workflows/unify-ci.yml`, "Leak check (must stay at
zero)").

---

### E7.2 — Grounded solutions out of `SolveJob`

**File:** `unify/src/vault-unify-solvejob.hpp` / `.cpp`.

`m_listUnifySolutions` is private (`vault-unify-solvejob.hpp:256`) and should
stay private: it holds non-owning pointers into the job's arena, and the
destructor's own comment warns that walking it would double-free
(`vault-unify-solvejob.cpp:1474-1477`). So the job grounds its own solutions
and hands out something that owns itself.

Add, next to `getSolutionList()`:

```cpp
/**
 * One solution's bindings as freshly-cloned, arena-independent terms.
 *
 * Move-only, and it frees what it holds -- so a caller cannot leak these
 * and cannot be tempted to keep raw arena pointers instead.
 */
class GroundedSolutions { ... };

GroundedSolutions getGroundedSolutions() const;
```

Requirements:

- Collect the goal's variables exactly as `getSolutionList()` does —
  `CollectVarTermsVisitor` at `vault-unify-solvejob.cpp:92-105` over
  `m_pGoal->applyVisitor()`. **Lift that visitor out of the function** so
  both methods use one copy; two copies of the collection rule is how the
  two methods drift.
- For each solution `UnifyContext* uc` and each variable, resolve exactly as
  today (`:139-149`) — note `AssignmentId aid( 0, ... )`, scope hard-coded
  to the top-level query, and the rationale at
  `vault-unify-solvejob.cpp:804-809` — then call
  `resolveTermGrounded( pTerm, uc, spInstance->getUnifyContext() )`.
- `GroundedSolutions`'s destructor frees every tree it holds via
  `collectTermTree` into one `std::set` and then `delete`, the pattern
  `~SolveJob` uses at `vault-unify-solvejob.cpp:1534-1544`. **One set per
  solution, not per term**: terms can alias
  (`vault-unify.hpp:1067`), and a per-term `deleteTermTree` would
  double-free.
- Keep `getSolutionList()` working and unchanged in shape. E7.3 switches
  `LocalSession` over; `unify-repl.cpp:384` keeps the string form until
  someone chooses to move it, which is not this item.
- **Do not** retain the `boost::shared_ptr<Job>` to extend the arena. It
  would work and it would reintroduce the `m_lsZombieJobs` leak the ownership
  work removed (`vault-unify-engine.cpp:149-163`).

**Test:** extend `unify/test/engine/solution-values-test.cpp`. Run a real
query through `RuntimeContext`, take `getGroundedSolutions()` inside the
`onFinished` callback, let the job die, and *then* read the terms — that is
the whole point, and it is the assertion that would have caught a
use-after-free. Run it under ASan.

---

### E7.3 — Wire the adapter, and flip the flag

**Files:** `unify/src/vault-unify-local-session.cpp`,
`unify/test/session/contract-main.cpp`.

1. Replace the flattening at `vault-unify-local-session.cpp:594-604` with
   `getGroundedSolutions()` + `toSessionValue()`. Keep `isInternalVarName`
   filtering (`:67-70`, applied at `:597`) exactly as it is.
2. Set `caps.structuredSolutions = true` (`:415`) and rewrite the note at
   `:72-84`, which currently explains why solutions are thin.
3. **In the same commit**, implement `d.scriptDeepGoal` in
   `contract-main.cpp:371-379`. It is a `UT_FAIL` stub today, guarded by the
   capability — the moment the flag flips, `contract-suite.cpp:584` and
   `:962` call it and the suite fails by construction. Model it on
   `scriptCountingGoal` (`contract-main.cpp:337-341`) and `definePredicate`
   above it: define a predicate whose solution binds a variable to a term
   nested `n` deep, and map the goal onto it.
4. Un-skip and pass `contract-suite.cpp:950` ("truncation marks the node it
   cut and inspect can undo it") for the `local` subject. This is the
   acceptance test for the whole item.

**What starts happening on its own, and must be checked rather than
assumed:** `applyBudget` is *already* applied to every emitted binding
(`vault-unify-local-session.cpp:1032-1040`) and to `inspect`'s reply
(`:1215-1227`), but today only its `maxStringBytes` rule can fire, because
`childCount` returns 0 for a `Str`
(`vault-unify-session-value.cpp:20-31`). The moment values are trees, the
`maxDepth = 8` and `maxNodes = 512` defaults (`vault-unify-session.hpp:137`)
start biting. `inspect` also starts having something to expand, because
`QueryState::solutions` stores the **untruncated** values. Decide
deliberately whether 8/512 is right for a transcript line and record the
decision; neither `session-bridge.cpp:75-83` nor `spec.cpp:75-81` sets a
budget today.

---

### E7.4 — Unbound variables become `Var` rows

**Separate phase because it changes binding cardinality**, and that is
visible in the UI.

Today an unbound variable is *silently omitted* from the solution map
(`vault-unify-solvejob.cpp:154-160` logs and falls through). So
`LocalSession` never emits a `Kind::Var` binding and never emits a row for
an unbound variable at all. `SESSION-API.md:157-159` says the opposite is
required:

> `Var` carries the variable's display name for unbound bindings — a Prolog
> front end that loses this is unusable for the debugging cases that matter
> most.

Emit them. Then:

- `contract-suite.cpp:1012-1019`'s `Kind::Var` assertion stops being vacuous
  — it is written and currently unreachable.
- `lens/src/model/model.cpp:178-186`'s `"yes"` fallback for an
  empty-bindings solution stops firing for goals with unbound variables.
  Check `lens/test/golden/transcript-output.expected` line 7.
- `renderBindings` output widens; see E7.5.

---

### E7.5 — Front-end fallout

**Three known breaks, all found by reading, none hypothetical.**

1. **`lens/src/app/spec.cpp:98` breaks silently.** It reads
   `binding.second.name` with no kind check, into a
   `map<string,string>`. Under E7 an `Int` binding has an empty `name` and
   its value in `.i`, so `asInt` (`spec.cpp:114`) fails and **the spec runner
   rejects its own spec file** — every `$step`, `$times`, `$w`, `$h` and
   `moved` distance. Give `Row` a kind-aware flattener. `spec.cpp` links the
   engine, so `us::toDisplayString` is available — but note it *quotes*
   `Str` (`vault-unify-session-value.cpp:181-240`), which is wrong for a
   key name; a local ten-line flatten is probably better. `check-spec-runner.sh`
   is the test that catches this either way.
2. **`lens/src/model/transcript.cpp:38-47`, the `Cons` arm, renders a
   truncated compound wrongly.** It prints the args only `if( !value.args.empty() )`,
   so a `Cons` whose args were all cut by the budget renders as a bare atom
   — a *wrong term shown as a whole one*, which `SESSION-API.md:154-158`
   calls the load-bearing failure. `toDisplayString` gets this right
   (`vault-unify-session-value.cpp:197`: `if ( !value.args.empty() || value.truncated )`).
   Copy that condition.
3. **`renderValue`'s `Str` comment becomes false.** `transcript.cpp:28-36`
   says "when E7 lands, `Str` becomes rare and quoting it becomes right".
   Per §2, `Str` becomes *never* — `LocalSession` cannot produce one. Update
   the comment; do not start quoting.

Goldens: `lens/test/golden/transcript-define-query.expected:7-9` shows
`$x = red`. `red` becomes `Kind::Atom` and `renderValue` prints
`value.name` for both `Atom` and `Str`, so **it should not change** — verify
rather than assume, and if it does change, the change is the finding.
No `unify/test/golden/*.expected` can change: they are `unify-run` stdout,
which never touches the session value model.

Also note the divergence nothing pins: `renderValue` marks truncation with
`" …"` (`transcript.cpp:74`), `toDisplayString` with `"..."`
(`vault-unify-session-value.cpp:239`). Pick one.

---

### E7.6 — The payoff: nested spec cases in lens

Only after E7.3–E7.5 are green.

`lens/test/spec/interaction.ufy` explains at length why it is flat facts.
With structured values, add the nested form the file says it wanted:

```prolog
case( reversal, "reversing direction takes effect on the first press", [
    step( "F1", [ panel( "Help" ) ] ),
    repeat( 25, "Down", [ visible ] ),
    step( "Up", [ moved( cursor, up, 1 ) ] )
] );
```

`lens/src/app/spec.cpp` grows a term-shaped reader beside the flat one; the
flat form keeps working, and the existing suite keeps passing unchanged
while the nested form is added case by case. The empty-list and
negative-literal defects (`unify/ROADMAP.md`) still apply, so `step/1`
without an expectation list stays, and `moved(cursor, up, 1)` keeps its
direction word.

Delete the "WHY FLAT FACTS" section only when the flat form is actually
gone. Until then it is still true.

---

## 4. Things to leave alone

- **`unify-repl.cpp:384`.** It uses `getSolutionList()` and its string form.
  Moving it is a separate, optional change; doing it inside E7 mixes an
  engine item with a REPL cosmetics change.
- **The `Str` and `Float` kinds.** They stay in the wire format. The fake
  session produces structured values including `Int`
  (`unify/test/session/fake-session.cpp:85-88`), and a remote core may have
  real types. Removing them would be narrowing the boundary to today's
  engine, which is the mistake the boundary exists to avoid.
- **`TraceEvent::goal`** (`vault-unify-session.hpp:468`) — a `Value` that is
  never populated and never read. Filling it is engine item E13's business,
  not E7's, even though it will become easy once the walker exists.
- **The two ROADMAP defects** (negative literal, empty list). They live in
  the parser; E7 lives after it.

## 5. Definition of done

- [ ] `LocalSession::describe().structuredSolutions == true`.
- [ ] `unify/test/session/contract-suite.cpp:950` runs and passes for the
      `local` subject — no skip.
- [ ] `contract-suite.cpp:1015`'s `Kind::Var` assertion is reached (E7.4).
- [ ] A new engine-item test covers §2's table term by term.
- [ ] `ctest` green on `build/unify` and `build/lens`; zero warnings; the
      ASan leak gate still reports zero; TSan still clean.
- [ ] Every engine golden byte-identical. Any lens golden that moves has its
      diff explained in the commit message.
- [ ] [ACCEPTANCE.md](ACCEPTANCE.md) G5.4's floor is raised, and the
      `LocalSession` note at `vault-unify-local-session.cpp:72-84` no longer
      describes a degradation that has been fixed.
