# Unify Language Specification

This describes the Unify language (`.ufy`) and its engine's run-time
semantics as implemented in `combine/modules/unify` today. It is derived by
reading the parser (`src/vault-unify-parser.hpp`,
`src/vault-unify-parser.cpp`), the solver (`src/vault-unify-solvejob.cpp`),
the binding model (`src/vault-unify-unifycontext.cpp`,
`src/vault-unify-term-*.cpp`), the core data model
(`include/vault-unify.hpp`), and the builtins
(`src/vault-unify-clause-builtin*.cpp`) — not from any prior design note.
Every normative statement cites the file/function it was read from.
Passages marked **QUIRK** are behavior that looks surprising, inconsistent,
or unintentional, documented as-is, not endorsed.

`test/conformance/*.ufy` exercises one rule per file; each file's header
comment records the output its query(ies) should print, per this reading.
No `.expected` golden file is checked in for them yet (no Boost toolchain
available here to build `unify-run`, same as the four pre-existing sample
programs — see `test/golden/README.md`); `run-golden-test.sh` reports
`SKIP` for a missing golden file.

Contents: 1. Lexical elements · 2. Program structure · 3. Terms ·
4. Desugarings · 5. Execution model · 6. Unification ·
7. Negation-as-failure · 8. Builtins · 9. Deviations and quirks.

---

## 1. Lexical elements

Grammar: `vault::unify::PrologParser::ClauseParser`
(`src/vault-unify-parser.hpp:ClauseParser::ClauseParser`). Skipping
(`ufy_skipper`, same file): ASCII whitespace, `/* ... */` block comments
(no nesting), `// ...` line comments — all skippable between any two
tokens, as used throughout `test2.ufy` and `pathfinder.ufy`.

**Identifiers/variables**: `m_ruleId %= qi::char_("$a-zA-Z_") >>
*qi::char_("a-zA-Z_0-9");`. Whether a name is a variable or a
functor/atom name is decided purely by its first character during AST
construction: `if ('$'==ch) { ...VarTerm... } else { ...ConsTerm... }`
(`src/vault-unify-parser.cpp:AnyTermFactory::operator()(const ConsTermInput&)`).
So `$x`, `$_anything`, `$__1000` are variables; anything else is a
0-or-more-arity atom/functor. All occurrences of one variable name within a
clause/query share one `VarTerm` object (`ClauseContext::m_mapSymbols`
lookup-or-create, `src/vault-unify-parser.hpp:ClauseContext`); the table is
reset after each clause/query (`clauseContext.reset()`,
`src/vault-unify-runtime-context.cpp:RuntimeContext::parseExecuteSegment`),
so variable scope is exactly one clause body or one top-level query.

**Numbers**: `m_ruleNumber %= + qi::char_("0-9");` — unsigned digit runs
only; no floats, no literal negative numbers (the `-` prefix does something
else entirely, section 4). A number reaches the same `m_ruleAtom`
alternative as an identifier or string and becomes an ordinary 0-arity
`ConsTerm` — **numbers are not a distinct term kind** (sections 6, 9).

**Strings**: `m_unescapedString %= qi::lit('"') >> qi::no_skip[
*(m_unescapedChar | "\\x" >> qi::hex | qi::char_ - '"' - '\\') ] >>
qi::lit('"');`. Escapes (`m_unescapedChar` table, same location): `\a \b
\f \n \r \t \v \\ \' \"`, plus `\xHH`. The unescaped content becomes the
atom's name via the same `m_ruleAtom` alternation as identifiers/numbers:
```
m_ruleAtom %= ( m_ruleId >> qi::attr(1) )
            | ( m_ruleNumber >> qi::attr(1) )
            | ( m_unescapedString >> qi::attr(1) );
```
so `"red"` and bareword `red` become the *same* atom name (section 9).

---

## 2. Program structure

A file is a sequence of independently-parsed **events**, each a clause or a
top-level query: `m_ruleEvent %= (m_ruleQuery) | (m_ruleClause);`.
`RuntimeContext::parseExecuteSegment`
(`src/vault-unify-runtime-context.cpp`) parses one event at a time; each
clause is appended to the root execution state immediately, each query
spawns a `SolveJob` immediately (section 5) — so later events only ever see
earlier clauses (plus the four builtins, registered up front by
`World::init`, `src/vault-unify-world.cpp`).

**Facts/rules**: `m_ruleClause %= (m_ruleConsTerm >> '{' >> m_ruleGoal >>
'}') | (m_ruleConsTerm >> ';');`. A fact is a bare cons term plus `;`
(`color( red );`); a rule adds a `{ ... }` body. Both become a
`StandardClause` (`src/vault-unify-parser.cpp:Context::createClause`); a
fact's goal is `NULL`, and `StandardClause::isTerminal()` is true exactly
when the goal is `NULL`/empty
(`src/vault-unify-clause-standard.cpp:StandardClause::isTerminal`) — this
is what lets the solver treat a fact as having no further sub-goal
(section 5).

A cons term's argument list, if present, requires **at least one** element
(`'(' >> (m_ruleAnyTerm % ',') >> ')'`, Spirit's `%` never matches zero) —
`foo()` does not parse; a 0-arity call must omit the parens (`foo`), as
`test2.ufy` does throughout. Map (`{ ... }`) and array (`[ ... ]`) literals
use the same `%` operator for their contents, so `{}` and `[]` also do not
parse, even though `MapTerm` itself supports zero entries when built
programmatically (`MapTerm::MapTerm`, `include/vault-unify.hpp`).

**Top-level queries**: `m_ruleQuery %= (m_ruleQueryGoal >> '?');` where
`m_ruleQueryGoal %= qi::eps >> (m_ruleQueryStatement % ',');` and
`m_ruleQueryStatement %= m_ruleIfStatement | m_ruleSingleGoal;`. A query is
one or more statements (each an `if` or a bare single goal, **no**
`;` terminator) separated by `,` and followed by `?`, e.g. the one real
example in the existing samples (commented out in `test/mapsyntax.ufy`,
still written in the pre-fix `;`-separated form there since it's inert):
`point( $objPoint ), print( $objPoint) ?`. All of a query's statements
flatten into one `Goal` (`Context::createGoal`,
`src/vault-unify-parser.cpp`) — one flat conjunctive term list, solved as a
single goal chain (section 5); a rule body uses the separate `m_ruleGoal`
grammar (`m_ruleGoal %= qi::eps >> +(m_ruleAnyStatement);` with
`m_ruleAnyStatement %= m_ruleIfStatement | m_ruleSingleGoal >> ';';`), whose
statements stay `;`-terminated and has no trailing `?`.

Queries and clause/rule bodies deliberately use two different separators
now. Until 2026-08-20 both a top-level query and a `;`-terminated fact were
parsed by the same `m_ruleGoal`/`m_ruleAnyStatement` rules, and
`m_ruleEvent %= (m_ruleQuery) | (m_ruleClause);` tries the query
alternative first: since a fact `f(a);` is itself a valid `;`-terminated
statement, any run of facts immediately preceding a `?` anywhere later in
the file was swallowed whole into one giant query goal, leaving zero
clauses parsed from those facts — a grammar ambiguity, not a semantic
choice. The fix gives queries their own `m_ruleQueryGoal`/
`m_ruleQueryStatement` rules built on `,` instead of `;`, so a trailing `;`
after a top-level term is now unambiguously a clause/fact, while `,` or `?`
after one is unambiguously part of a query; the old `goal; goal; ?` form no
longer parses (a parse error: it now reads as one or more facts followed by
a dangling `?`).

**`if` statement**: `m_ruleIfStatement %= qi::lit("if") >> qi::lit("(") >>
m_ruleSingleGoal >> qi::lit(")") >> qi::lit("{") >> m_ruleGoal >>
qi::lit("}");` — a `SingleGoal` condition (optional leading `!` plus an
infix term) and a brace-delimited body. See section 4 for what this
compiles to (one of the most important **QUIRK**s here, section 9).

**Prefix `!`**: `m_ruleSingleGoal %= -qi::char_('!') >> m_ruleInfixTerm;` —
applies to whatever the rest of the statement evaluates to (sections 4, 7).

---

## 3. Terms

Three term kinds under `AbstractTerm` (`include/vault-unify.hpp`):
`ConsTerm`, `MapTerm`, `VarTerm`. No separate number/string kind.

**Cons terms** (`ConsTerm`, `include/vault-unify.hpp`): an `Atom` name plus
a fixed-size argument vector. `m_ruleConsTerm %= (m_ruleAtom >> -('(' >>
(m_ruleAnyTerm % ',') >> ')'));`. A bare atom is arity 0; `f(a,b,c)` is
arity 3. Facts, rule heads, and every predicate call are cons terms (or
desugar to one, section 4).

**Map terms**: `m_ruleMapPair %= (m_ruleAtom >> ':' >> m_ruleAnyTerm);
m_ruleMapTerm %= ('{' >> (m_ruleMapPair % ',') >> '}');`, e.g.
`{ x: 10, y: 20 }` (`test/mapsyntax.ufy`). Built into a `MapTerm`
(`include/vault-unify.hpp`) keyed by `std::string` in a sorted
`std::map<...>` (`MapTerm::MapTermMap`) — the sort order matters for
unification (section 6) and for arrays below.

**Array terms**: `m_ruleArrayTerm %= ('[' >> (m_ruleAnyTerm % ',') >>
']');`, e.g. `[a, b, c]`.
`AnyTermFactory::operator()(const ArrayTermInput&)`
(`src/vault-unify-parser.cpp`) desugars this straight into a `MapTerm`
keyed by stringified 0-based indices ("0","1","2",...). **Arrays are maps
with numeric-looking string keys, not a distinct term kind** (section 9) —
those keys sort lexicographically once an array has 10+ elements
("10" < "2"), invisible when comparing two same-length arrays (both sides
build identical keys in identical order) but a trap for anything
inspecting the `MapTerm` directly.

**Variables** (`VarTerm`, `include/vault-unify.hpp`): a process-wide unique
`VarTermId` (`VarTerm::VarTerm`, `src/vault-unify-term-var.cpp`, a plain
counter) plus an optional original source name for display. A `VarTerm` by
itself carries no value; bindings live per-`UnifyContext` at solve time
(section 6).

---

## 4. Desugarings performed by the parser

All in `AnyTermFactory` (`src/vault-unify-parser.cpp`), the visitor that
turns the Spirit AST into the real `AbstractTerm`/`Goal`/`Clause` tree.

**`lhs -> rhs`** (member deref): both `->` and a bracket form parse to
`InfixTermsInput` with an operator char (`'\0'` for `->`, since `qi::lit`
produces no attribute; `'['` for the bracket form) —
`m_ruleArrayDeref %= (m_ruleAnyConsTerm >> (-(qi::char_("[") >>
m_ruleAnyConsTerm)));` and `m_ruleAssignmentPart %= (m_ruleArrayDeref >>
(-(qi::lit("->") >> m_ruleArrayDeref)));`
(`src/vault-unify-parser.hpp:ClauseParser::ClauseParser`).
`AnyTermFactory::operator()(const InfixTermsInput&)` handles `op=='\0'` by:
allocating a fresh `VarTerm` named via `ClauseContext::nextAnonVarName()`
(`"$__"` + a counter starting at 1000, shared per clause/query,
`src/vault-unify-parser.hpp:ClauseContext`); pushing a pre-goal
`__builtin_member_deref(lhs, rhs, freshVar)` onto the *same* flat term list
the enclosing goal is built from; and returning the fresh var as the
expression's value, substituted wherever it appeared. Because the pre-goal
is pushed before the statement's own term (`Context::createGoal`,
`src/vault-unify-parser.cpp`), `print($p->x)` compiles to two consecutive
goal terms: `__builtin_member_deref($p, x, $__N)` then `print($__N)` — the
deref always runs immediately before its result is used, and its failure
(bad key, non-map left side — section 8) fails the surrounding goal
silently. Consequence, not itself a quirk: `lhs -> rhs` written as a bare
statement (not a call argument, not assigned via `=`) leaves a **bare
`VarTerm`** as a goal term, which can never unify with anything
(`SolveJob::startUnification` requires a `ConsTerm`, section 5) — every use
in `test2.ufy` instead feeds `->` into a call argument or an `=`.

**`lhs = rhs`** (infix unify): `m_ruleInfixTerm %= (m_rulePrefixTerm >>
(-(qi::char_("=") >> m_rulePrefixTerm)));`. `case '=':` sets `atomName =
"unify"`, building `ConsTerm("unify", lhs, rhs)`
(`src/vault-unify-parser.cpp:AnyTermFactory::operator()(const InfixTermsInput&)`),
resolved by `UnifyBuiltinClause` (section 8).

**`lhs[rhs]`** (bracket deref) — **QUIRK, unimplemented**: `case '[':`
sets `atomName = "__builtin_array_deref"` (same function). No clause of
that name is ever registered anywhere in this module (checked by
repository search; `World::init`, `src/vault-unify-world.cpp`, only
registers `unify`, `print`, `emit`, `__builtin_member_deref`). The syntax
parses but the goal can never match anything by name, so it **always fails
silently**. Use `->` instead.

**Prefix `!` is not handled by the "prefix" rule**: `m_rulePrefixTerm %=
-qi::char_('-') >> m_ruleAssignmentPart;` — only `'-'` is ever grammatically
reachable here. `AnyTermFactory::operator()(const PrefixTermInput&)`'s
`case '!': atomName = "__builtin_not";` is dead code (also never
registered as a clause). The `!` that actually negates a goal is parsed one
level up, in `AnyTermFactory::operator()(const SingleGoalInput&)`: if the
statement's operator is `'!'` and its term is a `ConsTerm`, it calls
`pConsTerm->setNegated(true)` (`ConsTerm::setNegated`,
`include/vault-unify.hpp`); if the term is *not* a `ConsTerm` (e.g. it
desugared to a bare `VarTerm`), negation is silently dropped with only a
debug log — no parse or runtime error surfaces.

**Prefix `-` — QUIRK, not numeric negation**:
`AnyTermFactory::operator()(const PrefixTermInput&)` contains
```cpp
switch( op ) {
case '!': { atomName = "__builtin_not"; break;
default: break;
} }
```
`default:` is lexically nested inside `case '!':`'s block, but C++ still
treats it as an ordinary label of the enclosing `switch`. Since `'-'` is
the only reachable operator here, every real invocation takes `default:`,
leaving `atomName` empty; the function then builds an arity-1 `ConsTerm`
**with an empty-string name** wrapping the operand. So `-5` is not negative
five — it is an anonymous unary wrapper that only unifies with another
`-`-prefixed term built the same way. Looks like an abandoned/incomplete
feature; no existing sample program uses prefix `-`, and it is not covered
by a dedicated conformance test (documenting it here was judged more
valuable than a ninth program for an effectively-dead operator).

**`if (cond) { body }` — QUIRK, condition discarded**:
`AnyTermFactory::operator()(const IfStatementInput&)` never reads
`ifStatementInput.lhs` (the parsed condition) anywhere — confirmed by
inspecting every line of the function; only `.rhs` (the body) is used.
What it does instead: synthesizes a new 1-argument clause (e.g.
`__if__3($__N)`, via `ClauseContext::nextAnonClauseName`/
`nextAnonVarName`), whose body is the original `{ }` body with the *same*
fresh variable `$__N` **prepended as a goal, in place of the discarded
condition**; registers the clause on the world's root state; and returns a
call `__if__3($__N)` spliced into the enclosing goal where the `if`
statement was. Because `$__N` is never bound by anyone (the real condition
was thrown away, and the call site passes that same fresh, still-unbound
variable), the synthesized body's first goal is a bare, unbound `VarTerm`
— which fails immediately in `SolveJob::startUnification` (section 5) for
every candidate clause, every time. **An `if` statement's body never
executes today, regardless of the condition** — see
`test/conformance/if-statement.ufy` for a concrete demonstration with both
a "true-looking" and a "false-looking" condition (both behave identically:
the body's `print` never runs, and neither does anything queued after the
`if` in that query, since the whole goal chain fails there). This is the
most significant functional gap this reading found; `ROADMAP.md` does not
currently flag it, and no existing sample program uses `if`.

---

## 5. Execution model

**Clause database**: one `ExecutionState` per `World`
(`World::m_rootState`, `include/vault-unify.hpp`). `World::init()`
(`src/vault-unify-world.cpp`) appends, in order, `UnifyBuiltinClause`,
`PrintBuiltinClause`, `EmitBuiltinClause`, `MemberBuiltinClause`, before any
source-file clause is parsed. `ExecutionState::appendClause` always
`push_back`s (`src/vault-unify-execution-state.cpp`), so **clause order
within one predicate name is exactly source definition order** (builtins
first, then facts/rules top-to-bottom) — see
`test/conformance/clause-order.ufy`. `ExecutionState::fork()` and the
resulting parent-state fallback in `ExecutionState::ClauseIterator`
(`include/vault-unify.hpp`, walks `m_pParent` once the current state's
list is exhausted) exist but `fork()` is never called anywhere in this
module (checked by search) — every clause and query run against the single
root state, so the fallback is currently dead in practice.

**One `SolveJob` per query**: `RuntimeContext::parseExecuteSegment`
enqueues one per parsed query; `unify-run`'s own comments
(`tools/unify-run.cpp`) note jobs run strictly FIFO on one worker thread and
a job's `performSlice()` runs to completion in one call under the default
target state — so side effects appear in file order across queries.
`SolveJob::startJob` builds one root `GoalPart` (the query's flat `Goal`),
one parentless root `UnifyContext`, one root `SolveContext` pushed onto
`m_stackContext`.

**Depth-first search, explicit stack**: `SolveJob::performSlice()`
(`src/vault-unify-solvejob.cpp`) loops while `m_stackContext` (a
`std::list<SolveContext*>`) is non-empty, always looking at `.back()`:
1. If the top's goal cursor is exhausted, the current bindings are a
   solution (`emitSolution`; any `print`/`emit` already happened), then pop
   — backtracking into whoever pushed this context.
2. If the top's clause iterator has no more candidates, this branch is
   exhausted: pop (more backtracking).
3. Otherwise take the next candidate clause, build a fresh child
   `UnifyContext`, and call `startUnification` (sections 6, 7). On
   `UnifyNot`, discard it and continue. On success, push a **new**
   `SolveContext` — either continuing at the next term of the *same* goal
   chain (fact/no-continuation match) or diving into the matched rule's
   right-hand `Goal` (linked back via `GoalPart::m_itNextTermInParent`,
   `include/vault-unify.hpp`, so control resumes after the call site once
   the callee's chain is exhausted). Either way the current context's
   clause iterator advances **immediately**, so when the just-pushed child
   eventually finishes and pops, its parent resumes by trying its *next*
   candidate at the same position — this is how multiple facts/rules of
   one name each get their own solution branch, tried in clause order,
   fully depth-first (a pushed child always runs to exhaustion before its
   parent's next candidate is attempted).

No recursion-depth or step limit exists (`ROADMAP.md` notes this as a
future-work gap); a non-terminating query hangs.

**Side effects are not rolled back**: `print`/`emit` write to stdout
directly inside `startUnification`
(`src/vault-unify-clause-builtin-print.cpp`,
`src/vault-unify-clause-builtin-emit.cpp`), i.e. exactly when the search
passes through that goal position — including once per backtracked
alternative — and the output is **not** undone if a later goal in the same
chain subsequently fails. See `test/conformance/if-statement.ufy`, where a
"before" print survives even though the rest of that query deliberately
fails afterward.

---

## 6. Unification

Double dispatch: `UnifyContext::unifyTerms`
(`src/vault-unify-unifycontext.cpp`) calls `pMyTerm->unifyTerm(...)`, whose
default implementation on each concrete class re-dispatches to
`pOther->unify<Kind>Term(...)` (e.g. `ConsTerm::unifyTerm` calls
`pOther->unifyConsTerm(...)`, `src/vault-unify-term-cons.cpp`) so the
concrete runtime type pair picks the real comparison. `UnifyResult`
(`include/vault-unify.hpp`): `UnifyError=-1`, `UnifyNot=0`, `UnifyLast=1`,
`UnifyNotLast=2`; `Unifies(r)` is true for `UnifyLast`/`UnifyNotLast`. Every
concrete `unify*Term` in this module only ever returns `UnifyNot` or
`UnifyLast` (checked by search) — `UnifyNotLast` is defined but never
produced, and reads identically to `UnifyLast` everywhere it's consumed
(`SolveJob::performSlice`).

**Cons vs. cons** (`ConsTerm::unifyConsTerm`,
`src/vault-unify-term-cons.cpp`): identical pointer unifies trivially; else
name (`Atom::operator==`) and arity must both match, else `UnifyNot`;
otherwise every argument pair is unified in order, failing as soon as one
pair fails. This is why `pair(a,b,c)` never matches head `pair(a,b)`
(arity 2 vs. 3) — see `test/conformance/unification-basics.ufy`.

**Cons vs. map**: always `UnifyNot` both ways
(`ConsTerm::unifyMapTerm`/`MapTerm::unifyConsTerm`) — the two kinds never
unify with each other.

**Map vs. map** (`MapTerm::unifyMapTerm`, `src/vault-unify-term-map.cpp`):
identical pointer unifies trivially; else sizes must be **exactly equal**
(no subset/partial match — an extra or missing key always fails the whole
comparison, however many keys are shared); if equal (including both
empty), both maps are walked in parallel in their sorted-key order, keys
compared at each position (sufficient to detect any key-set difference
since sizes already match) and values unified — any mismatch fails the
whole thing. See `test/conformance/map-unify.ufy` (a match, a value
mismatch, a size/key-set mismatch).

**Var vs. anything** (`UnifyContext::genericUnifyVarWithKnown`,
`src/vault-unify-unifycontext.cpp`, reached via each kind's
`unifyVarTerm`/`unifyConsTerm`/`unifyMapTerm` overloads for `VarTerm`): a
variable is identified by `AssignmentId` = (owning `UnifyContext` id,
`VarTermId`) (`include/vault-unify.hpp`), looked up by walking the current
context's parent chain (`findVarBinding`/`findVarInstance`,
`src/vault-unify-unifycontext.cpp`) so a binding made earlier in the same
chain, or by a caller, is visible to callees. Unbound ⇒ bind to the other
term, always `UnifyLast`. Already bound ⇒ unify the *bound* value against
the new term instead (recursively). Var-vs-var
(`VarTerm::unifyVarTerm`, `src/vault-unify-term-var.cpp`) additionally
merges whichever side(s) already have bindings. See
`test/conformance/unification-basics.ufy`,
`test/conformance/infix-unify.ufy` (through `=`, section 4/8).

**Map member lookup** (`->`, not itself a unification rule):
`MapTerm::getValue` (`src/vault-unify-term-map.cpp`) is an exact key
lookup; a missing key returns `NULL`, and `__builtin_member_deref`
(section 8) fails outright — no default/partial value — see
`test/conformance/deref.ufy`.

---

## 7. Negation-as-failure

Prefix `!` sets `ConsTerm::m_isNegated` at parse time (section 4). At solve
time, `SolveJob::startUnification` (`src/vault-unify-solvejob.cpp`) reads
it **per candidate clause**, not once per predicate call:
```cpp
if( pConsTerm->isNegated() ) { pUCStackTop->setNegated( true ); }
if( pClauseTerm->getName() == pConsTerm->getName() ) {
    isMatchName = true;
    if( pClauseTerm->getArity() == pConsTerm->getArity() ) {
        pUCStackTop->setFoundClause( true );
    }
}
```
`isMatchName` (name only) gates whether this candidate's
`startUnification` runs at all; the arity check right below only sets
`m_foundClause` and does **not** itself block the call (section 9 — the
real arity gate for ordinary unification is one level down, in
`ConsTerm::unifyConsTerm` or a builtin's manual check).

`UnifyContext::getUnificationResult()`
(`src/vault-unify-unifycontext.cpp`) applies negation to that candidate's
raw result:
```cpp
if( !m_isNegated || UnifyError==m_unificationResult ) return m_unificationResult;
if( Unifies( m_unificationResult ) ) return UnifyNot;     // matched -> negation fails
if( m_foundClause ) return UnifyLast;                     // same name+arity, args differed -> negation succeeds
return UnifyNot;                                          // no clause of that name+arity at all -> negation fails
```
So per candidate sharing the negated goal's name: an actual argument match
makes that candidate fail the negation; a same-name-and-arity candidate
whose arguments *don't* match makes it succeed; a candidate that doesn't
even share the arity (or share the name at all) makes it fail too. Because
`SolveJob::performSlice` tries **every** same-named candidate, each with
its own fresh, discarded-on-failure `UnifyContext` (section 5), a negated
goal can succeed once per clause whose arguments happen not to match — not
once overall for "nothing proves this". `test/conformance/negation.ufy`
demonstrates all three outcomes against one fact `fruit(apple)`:
`!fruit(apple)` fails (it does hold); `!fruit(banana)` succeeds (same
name/arity, argument mismatch); `!nosuchpredicate(x)` **fails** (no clause
anywhere shares that name, so no candidate ever reaches the
"found-clause-but-mismatched" success path).

That last case is the headline **QUIRK**: it is the opposite of textbook
negation-as-failure ("nothing proves Goal, so `\+ Goal` succeeds") — here,
negating a call to an entirely undefined predicate *fails*. A related,
untested-for-determinism-reasons consequence: if several clauses share the
negated goal's name/arity, a negated call can succeed via one clause's
mismatch even while a sibling clause of the same name genuinely matches —
negation here is per-candidate, not "no clause anywhere proves this call",
and it does not implement "commit once, don't backtrack" either (a negated
goal is walked with the same backtracking machinery as an ordinary one and
can yield more than one solution branch). Unbound variables under negation
start fresh for every candidate (a failed candidate's `UnifyContext`, and
any bindings it made, is discarded), so `!` does not protect a variable
from being bound by whichever candidate does end up contributing
`UnifyLast`.

---

## 8. Builtins

All four appended to the root state, in this order, before any user clause
(`World::init`, `src/vault-unify-world.cpp`).

**`unify($a, $b)`** — `UnifyBuiltinClause`
(`src/vault-unify-clause-builtin-unify.cpp`). Head declared arity 2.
`startUnification` requires the goal to be a `ConsTerm` of arity exactly 2
named `unify` (an explicit manual check), then calls
`pTermLeft->unifyTerm(..., pTermRight)` — a thin wrapper around section 6's
double dispatch, nothing more. This is what `lhs = rhs` desugars to
(section 4). Never produces a continuation goal.

**`print($x)`** — `PrintBuiltinClause` (`OutputBuiltinClause` base,
`src/vault-unify-clause-builtin-print.cpp`,
`src/vault-unify-clause-builtin-output.cpp`). Head declared arity 1, but
`OutputBuiltinClause::startUnification` only checks the goal's **name**,
then loops over however many arguments the *goal* actually has, joining
each one's `toContextString(pUCStackTop, pUCOriginal)`
(`PrintBuiltinClause::convertTerm`) with commas — **`print` is effectively
variadic in practice** (section 9). It always reports success (the real
unify-against-declared-head code is present but disabled behind `#if 0`)
and always calls `PrintBuiltinClause::output`:
```cpp
std::cout << "print: " << outputString << std::endl;
```
Every reached `print(...)` writes one `print: ...` line — variables shown
resolved to their bound value if any, else their raw name. This exact
prefix/format is what `test/golden/*.expected` diffs against
(`test/run-golden-test.sh`).

**`emit($x)`** — `EmitBuiltinClause`
(`src/vault-unify-clause-builtin-emit.cpp`). Structurally identical to
`print` (same variadic-in-practice, always-succeeds behavior), but converts
arguments via `toJSON(true, ...)` and writes `"emit: " + outputString`,
then also forwards that string to `pEngine->emitEvent(...)` for any
registered `UserEventListener` — used by the REST frontend, not exercised
by stdout-only conformance tests here.

**`__builtin_member_deref($map, $key, $out)`** — `MemberBuiltinClause`
(`src/vault-unify-clause-builtin-member.cpp`). What `lhs -> rhs` desugars
to (section 4); never written directly in source. Head declared **arity
2** — inconsistent with the arity 3 every real call site actually uses
(QUIRK, section 9). `startUnification` manually requires arity exactly 3;
dereferences argument 0 via `AbstractTerm::getBoundTerm`
(`src/vault-unify-terms.cpp` — resolves a `VarTerm` to its current binding,
or fails if unbound) and requires the bound value to be a `MapTerm`
(arrays included, section 3); requires argument 1 to be a 0-arity
`ConsTerm` (the key); looks it up via `MapTerm::getValue` and, on a hit,
unifies the value against argument 2. A missing key, non-map left side, or
non-atom key all fail the whole goal silently — see
`test/conformance/deref.ufy`.

---

## 9. Known deviations and quirks

All verified by direct code reading, collected here for quick reference.

- Bracket deref `lhs[rhs]` parses but always fails at run time — no
  `__builtin_array_deref` clause is ever registered (section 4).
- Prefix `!`'s handling inside `PrefixTermInput` (`__builtin_not`) is dead
  code; the grammar only ever feeds `'-'` there. The `!` that matters is
  parsed at the statement (`SingleGoalInput`) level (section 4).
- Prefix `-` does not implement negation: due to `case`/`default` label
  nesting, it always builds an anonymous, empty-named wrapper `ConsTerm`
  around its operand rather than negating a number (section 4).
- `if (cond) { body }` discards `cond` entirely and gates `body` behind an
  always-unbound, unsatisfiable variable goal — the body never executes,
  regardless of the condition (section 4). The most significant functional
  gap found; not flagged in `ROADMAP.md`, unused by any existing sample.
- Strings, bareword atoms, and digit runs all fold into the same atom
  representation by spelling; a quoted string and a bareword atom (or
  number) with the same spelling are the same term (sections 1, 6). No
  arithmetic exists anywhere in this module as a consequence.
- `SolveJob::startUnification`'s per-candidate prefilter gates on **name
  only**; the neighboring arity check only feeds `m_foundClause`
  (negation, section 7) and does not itself block a wrong-arity
  candidate's `startUnification` from running — the real arity gate for
  ordinary unification is one level down, in `ConsTerm::unifyConsTerm` or a
  builtin's own manual check (sections 5, 7).
- `MemberBuiltinClause`'s declared head arity (2) doesn't match the arity
  (3) every real call site and its own manual check actually use — harmless
  for ordinary `->` use (the manual check saves it) but means
  `m_foundClause` is never set for it via the standard prefilter
  (section 8).
- `print`/`emit` declare an arity-1 head but never check arity at run time;
  they print/emit however many arguments the goal literally has, comma
  joined (section 8).
- Builtin predicate names (`unify`, `print`, `emit`,
  `__builtin_member_deref`) are appended before any user clause; a user
  rule reusing one of those names would not shadow the builtin — both would
  each contribute a solution branch, since every same-named candidate is
  tried (section 5).
- `ExecutionState::fork()` and the resulting parent-state clause fallback
  are fully implemented but never invoked anywhere in this module today
  (section 5) — not a bug, just currently-unused machinery.
- `UnifyResult::UnifyNotLast` is defined and handled identically to
  `UnifyLast` wherever it's read, but no `unify*Term` implementation ever
  returns it (section 6) — reserved for functionality that doesn't exist
  yet.
