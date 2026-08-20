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
only; no floats, no literal negative numbers written by hand (the leading
`-` prefix rule does something else entirely, section 4). A number reaches
the same `m_ruleAtom` alternative as an identifier or string and becomes an
ordinary 0-arity `ConsTerm` — **numbers are not a distinct term kind**
(sections 6, 9). That said, an *evaluated* arithmetic result (section 4.1)
can be negative — its atom text simply carries a leading `-` (e.g. `"-7"`),
and both arithmetic evaluation and comparison accept that leading `-` when
reading a number back in.

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
earlier clauses (plus the six builtins, registered up front by
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

**Top-level queries**: `m_ruleQuery %= qi::lit("query") >> '{' >>
m_ruleGoal >> '}';` — the keyword `query` followed by a brace-delimited
block of `;`-terminated statements, reusing the very same `m_ruleGoal`
grammar a rule body uses (`m_ruleGoal %= qi::eps >> +(m_ruleAnyStatement);`
with `m_ruleAnyStatement %= m_ruleIfStatement | m_ruleSingleGoal >> ';';`):
each statement is an `if` or a bare single goal terminated by `;`, e.g.
```
query {
    color( $x );
    print( $x );
}
```
All of a query's statements flatten into one `Goal` (`Context::createGoal`,
`src/vault-unify-parser.cpp`) — one flat conjunctive term list, solved as a
single goal chain (section 5), exactly as for a rule body. A file may
contain any number of `query { ... }` blocks, each spawning its own
`SolveJob` in order (section 5).

**`query` is a reserved word for a zero-argument top-level head**:
`m_ruleEvent %= (m_ruleQuery) | (m_ruleClause);` tries the query
alternative first. A clause head is a cons term whose argument list is
*optional* (`m_ruleConsTerm %= m_ruleAtom >> -('(' >> ... >> ')');`), so
`query { ... }` is *also* syntactically a valid rule definition — an
atom `query` with no args, followed by a `{ ... }` body — under
`m_ruleClause`. Because `m_ruleQuery` is tried first and (for exactly this
input shape) always succeeds, it always wins: **a rule literally named
`query` with no arguments can no longer be defined**; the bare atom `query`
at the start of a top-level form is unconditionally read as a query block.
A clause headed by the atom `query` remains expressible as soon as it
takes at least one argument, e.g. `query( a ) { ... }` or `query( a );`,
since `m_ruleQuery` requires `{` to immediately follow the `query` keyword
with no `(` in between; for that input `m_ruleQuery` fails (no word
boundary is needed: `qi::lit("query")` matches the literal text, then the
next expected token `{` fails to match `(`, or, for an identifier like
`query23(...)`, fails to match `2`), the whole alternative backtracks, and
`m_ruleClause` reparses the same input from the start, this time consuming
`query23` (or `query`, followed by its parenthesized args) as an ordinary
atom via `m_ruleId`.

Queries and clause/rule bodies deliberately share one `;`-terminated
statement grammar (`m_ruleGoal`) now, distinguished only by the `query { }`
wrapper. A comma-separated, `?`-terminated query form (`g1, g2 ?`, with no
`;` inside) existed only transiently on 2026-08-20, introduced to fix a
prior ambiguity where queries and facts were indistinguishable (see below)
and then immediately superseded, the same day, by the current
`query { ... }` block form for a more C/Java-like feel — consistent `;`
statement termination and `{ }` blocks everywhere in the language, and one
query syntax instead of two. Before that transient fix, both a top-level
query and a `;`-terminated fact were parsed by the same `m_ruleGoal`/
`m_ruleAnyStatement` rules with no distinguishing keyword or wrapper, and
`m_ruleEvent %= (m_ruleQuery) | (m_ruleClause);` tried the query
alternative first: since a fact `f(a);` is itself a valid `;`-terminated
statement, any run of facts immediately preceding a `?` anywhere later in
the file was swallowed whole into one giant query goal, leaving zero
clauses parsed from those facts — a grammar ambiguity, not a semantic
choice. The `query { ... }` keyword-block form fixes this the same way the
transient comma form did (a query is no longer parseable as a prefix of
plain facts) while additionally giving the language a single, uniform
statement/block syntax.

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

### 4.1 Arithmetic and comparison expressions (ROADMAP Phase 2)

Grammar precedence chain, each level inserted between the pre-existing
`m_rulePrefixTerm` (leading `-`, section 4 above) and `m_ruleInfixTerm`
(`=`, below), narrowest-binding first:

| level              | operators                          | associativity   |
|---------------------|------------------------------------|-----------------|
| `m_ruleMultiplicative` | `*` `/`                          | left            |
| `m_ruleAdditive`       | `+` `-`                          | left            |
| `m_ruleComparison`     | `==` `!=` `<=` `>=` `<` `>`      | none (single-shot) |
| `m_ruleInfixTerm`      | `=`                               | (unchanged, section 4) |

`m_ruleComparison`'s operator alternative tries the two-character operators
before the one-character ones (`qi::string("==") | "!=" | "<=" | ">=" |
"<" | ">"`), so `a <= b` cannot half-match as `a < ...` followed by a
dangling `=`; conversely `!=` cannot be confused with the *prefix* `!`
(section 4 above), since that only ever matches at the very start of a
`SingleGoal`, never mid-expression. There is no parenthesized grouping
(`(...)`) for sub-expressions — the only place `(` is grammatically valid
is immediately after an atom, as a call's argument list
(`m_ruleConsTerm %= m_ruleAtom >> -('(' >> ... >> ')')`) — so precedence can
only be overridden by the table above, not by parentheses.

**Binary `-` trace (why it does not conflict with existing hyphens)**:
`test2.ufy`/`pathfinder.ufy` contain hyphenated text like `"m1-sc01-ro01"`
and `"ozw0x0184e7b70x05"`; every such hyphen, without exception (checked by
searching every `.ufy` file in this module), appears **inside a quoted
string literal**. A string is consumed as one atomic token by
`m_unescapedString` (section 1) — `qi::char_ - '"' - '\\'` accepts a bare
`-` exactly like any other non-quote, non-backslash character — entirely
inside `m_ruleAtom`, several precedence levels below where a bareword's
individual characters would ever reach the new multiplicative/additive
rules. The grammar never even *tries* to interpret a hyphen as an operator
until after the enclosing string's closing `"` has already been consumed
whole. The only two other hyphen appearances in this module's `.ufy`
sources are `->` (member deref, its own two-character `qi::lit` token,
matched before `m_rulePrefixTerm`'s lone `-` ever gets a chance — section
4) and Prolog-style `:-` in `pathfinder.ufy`'s single (already
non-conforming, non-`.ufy`-grammar) rule head, unrelated to this grammar's
`-` handling entirely. So introducing binary `-` changes nothing about how
either sample program parses; `+ * /` and `-` are all implemented in v1.

**`lhs OP rhs`** (comparison goal, e.g. `$x < 5;`):
`AnyTermFactory::operator()(const CompareTermInput&)` builds
`__builtin_compare(opAtom, lhs, rhs)` (`opAtom` a 0-arity `ConsTerm` holding
the operator text, e.g. `"<"`), resolved by `CompareBuiltinClause`
(section 8). No operator present (the common case — every ordinary term
still flows through this level) just returns `lhs` unchanged, exactly like
`InfixTermsInput`'s existing single-operand fast path.

**`lhs + rhs` / `lhs * rhs` / ...** (arithmetic sub-expression):
`AnyTermFactory::operator()(const ArithTermInput&)` folds a
multiplicative/additive chain left-to-right into nested
`__builtin_arith(opAtom, lhs, rhs)` `ConsTerm`s, e.g. `2 + 3 * 4` becomes
`__builtin_arith("+", 2, __builtin_arith("*", 3, 4))`. This alone is inert
— nothing matches `__builtin_arith` as a goal name — it only ever appears
as a sub-term inside `__builtin_eval`'s or `__builtin_compare`'s arguments.

**`lhs = rhs` with arithmetic** — dual behavior of `=`:
`AnyTermFactory::operator()(const InfixTermsInput&)`, `case '=':`, now
checks whether either already-built side is a `__builtin_arith` `ConsTerm`
(arity 3, name `"__builtin_arith"`). If so, it desugars to
`__builtin_eval(arithSide, otherSide)` instead of `unify(lhs, rhs)`
(`ArithEvalBuiltinClause`, section 8) — evaluating the arithmetic side and
unifying the *result* with the other side, since plain structural `unify`
could never match an unevaluated `__builtin_arith` tree against a number. A
plain `=` between two ordinary terms (neither side arithmetic) is
completely unchanged — still `unify(lhs, rhs)`. If *both* sides happen to
contain arithmetic (e.g. `$x + 1 = $y + 2`), the left side is evaluated and
the right side is unified, unevaluated, against the result — an unlikely
construct no sample program or conformance test uses, deliberately not
over-engineered further.

**Evaluation semantics**: int64 only (no floats); `evaluateArith()`
(`vault-unify-clause-builtin-arith.cpp`, shared by both builtins below)
resolves a `VarTerm` via `AbstractTerm::getBoundTerm()` (an unbound
variable is a `UnifyError`), evaluates a `__builtin_arith` node bottom-up,
and otherwise requires a 0-arity `ConsTerm` atom whose text parses fully as
an int64 (`strtoll` plus a full-string check, so e.g. `"12abc"` is rejected,
not silently truncated) — a leading `-` is accepted when reading (section
1). `/` is C++ integer division (truncates toward zero); dividing by zero
is a `UnifyError`, which — like any other `UnifyError` — fails that one
candidate (there is exactly one `__builtin_eval`/`__builtin_compare`
clause, so the whole goal position then fails) and is recorded on the
`SolveJob` (`getErrorCount()`/`getLastError()`), which `unify-run` surfaces
via its exit code (ROADMAP Phase 1) — see
`test/conformance/arithmetic.ufy`'s division-by-zero query for a
demonstration (no stdout, but the run does not exit clean).

**Comparison semantics**: each side is resolved the same way arithmetic
operands are (`resolveCompareSide()`), additionally evaluating a
`__builtin_arith` side to an int64 first. If *both* resolved sides are
integers (whether from a plain numeric atom or an evaluated arithmetic
expression), the operator applies to the int64 values; otherwise both
sides' raw atom text is compared lexicographically (`std::string::compare`)
— so `"abc" < "abd"` works, and mixing a numeric side with a non-numeric
one falls back to comparing their textual forms. Succeeds (`UnifyLast`) or
fails (`UnifyNot`) like any other goal; an unresolved/unbound operand is a
`UnifyError`, same as evaluation. See `test/conformance/comparison.ufy`.

**Ownership of the evaluated result**: `ArithEvalBuiltinClause` is the
first builtin in this module to allocate a genuinely *new* term at solve
time (the freshly computed numeric result) rather than reusing one already
in the goal/clause tree. Nothing reaches it structurally (only a `VarTerm`
binding does, and `VarTerm::abstractTermIterator()` returns `NULL`, so the
existing `collectTermTree()`-based sweeps — `World::~World()`,
`~SolveJob()` — never see it), so it needs its own ownership:
`UnifyContext::adoptTerm()` (`include/vault-unify.hpp`) registers it, and
the new `~UnifyContext()` frees every adopted term with a plain `delete`
(never a recursive `collectTermTree()` walk — every adopted term today is
always exactly one leaf 0-arity atom, never a subtree). Every `UnifyContext`
is itself always one of `SolveJob`'s own per-job arena entries
(`m_arenaUnifyContexts`, `vault-unify-solvejob.hpp`), so the adopted term's
lifetime rides along with the job's existing arena cleanup in
`~SolveJob()`.

---

**`if (cond) { body }` — soft-if, `( cond, body ; true )`**:
`AnyTermFactory::operator()(const IfStatementInput&)` builds `cond`
(`ifStatementInput.lhs`, a single goal) and `body`
(`ifStatementInput.rhs`, a goal's worth of statements) as term trees using
the *enclosing* clause/query's own variable scope (`m_clauseContext`), the
same way any other goal in that context would be built (including any
nested pre-goal, e.g. from a `->` used inside `cond` or `body`). It then
collects the distinct free `VarTerm`s occurring anywhere in `cond`+`body`
and synthesizes a two-clause auxiliary predicate with a unique name (via
`ClauseContext::nextAnonClauseName("if")`, e.g. `__if__3`), appended to
`World`'s root `ExecutionState` in this order:
```
__if__3( V1..Vk ) { cond; body...; }   // then-branch, tried first
__if__3( V1..Vk );                     // fallback, always succeeds
```
and returns a call `__if__3( origV1..origVk )` spliced into the enclosing
goal chain where the `if` statement stood — `origV1..origVk` are the very
same `VarTerm` objects already in use by the enclosing clause/query, so
this call site is simply part of its term tree; `V1..Vk` are a **fresh**
set of variables, generated independently for each of the two synthesized
clauses (via `cloneTermTree`, `include/vault-unify.hpp`/
`src/vault-unify-terms.cpp`), so neither synthesized clause shares a
single term node with the enclosing clause/query or with each other.

Semantically this is Prolog's `( cond, body ; true )`: if `cond` succeeds,
`body` runs (once per solution of `cond`, on backtracking); if `cond`
fails, the enclosing goal chain simply continues after the `if`, with
whatever bindings (if any) `cond`/`body` made along the way. Because
`ExecutionState::ClauseIterator` tries every candidate clause of a
predicate in definition order (section 5) and this engine backtracks to
find *every* solution (not just the first), both synthesized clauses are
genuinely tried whenever `cond` can succeed at all — see the QUIRK below
and `test/conformance/if-statement.ufy` for what that means in practice.

---

## 5. Execution model

**Clause database**: one `ExecutionState` per `World`
(`World::m_rootState`, `include/vault-unify.hpp`). `World::init()`
(`src/vault-unify-world.cpp`) appends, in order, `UnifyBuiltinClause`,
`PrintBuiltinClause`, `EmitBuiltinClause`, `MemberBuiltinClause`,
`ArithEvalBuiltinClause`, `CompareBuiltinClause` (section 8, the last two
ROADMAP Phase 2), before any source-file clause is parsed.
`ExecutionState::appendClause` always
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
chain subsequently fails. See `test/conformance/if-statement.ufy`'s first
query: its `if` condition succeeds, but because this engine backtracks to
find every solution and `if` has no cut yet (section 4/9), the line after
the `if` is reached — and printed — twice: once continuing from the
then-branch, once continuing from the fallback branch that is still tried
afterward.

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

All six appended to the root state, in this order, before any user clause
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

**`__builtin_eval($expr, $out)`** — `ArithEvalBuiltinClause`
(`src/vault-unify-clause-builtin-arith.cpp`, ROADMAP Phase 2). What
`$y = <expr>;` desugars to whenever `<expr>` (or the other side of `=`)
contains an arithmetic operator (section 4.1). Head declared arity 2,
checked manually like `unify`. Evaluates argument 0 (`evaluateArith()`,
same file) and unifies the result — a fresh, `UnifyContext::adoptTerm()`-
owned 0-arity `ConsTerm` atom holding the decimal (possibly `-`-prefixed)
value — against argument 1. An unbound variable, a non-numeric atom, or
division by zero anywhere in the expression is a `UnifyError` (section
4.1).

**`__builtin_compare($op, $a, $b)`** — `CompareBuiltinClause`
(`src/vault-unify-clause-builtin-arith.cpp`, ROADMAP Phase 2). What a
comparison goal (`$x < 5;` and the other five operators) desugars to
(section 4.1). Head declared arity 3, checked manually. Resolves argument 0
to an atom (the operator text) and arguments 1/2 via the same resolution
`__builtin_eval` uses (evaluating a `__builtin_arith` side, if present);
numeric (int64) comparison if both resolved sides are integers,
lexicographic string comparison otherwise. Unlike every builtin above, this
one can genuinely fail *without* being a syntax/name/arity mismatch — a
false comparison is `UnifyNot`, same as any other failed goal.

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
- `if (cond) { body }` is a soft-if (`( cond, body ; true )`, section 4)
  with no cut yet (`ROADMAP.md` Phase 2): the synthesized fallback clause
  is tried on backtracking even after `cond` already succeeded once, so
  whatever follows the `if` in the same goal chain can run again via the
  fallback path — see `test/conformance/if-statement.ufy` (its first query
  prints the line after the `if` twice for exactly this reason).
- Strings, bareword atoms, and digit runs all fold into the same atom
  representation by spelling; a quoted string and a bareword atom (or
  number) with the same spelling are the same term (sections 1, 6). This
  is also why a numeric-looking atom that arrived via a quoted string
  (`"5"`) is just as usable in arithmetic/comparison as a bareword digit
  run (section 4.1) — both are the same atom.
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
  `__builtin_member_deref`, `__builtin_eval`, `__builtin_compare`,
  `__builtin_arith`) are appended before any user clause (the last one is
  never a clause name — it only ever appears as a sub-term, section 4.1); a
  user rule reusing one of those names would not shadow the builtin — both
  would each contribute a solution branch, since every same-named candidate
  is tried (section 5).
- `ExecutionState::fork()` and the resulting parent-state clause fallback
  are fully implemented but never invoked anywhere in this module today
  (section 5) — not a bug, just currently-unused machinery.
- `UnifyResult::UnifyNotLast` is defined and handled identically to
  `UnifyLast` wherever it's read, but no `unify*Term` implementation ever
  returns it (section 6) — reserved for functionality that doesn't exist
  yet.
