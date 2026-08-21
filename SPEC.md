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
7. Negation-as-failure · 8. Cut · 9. Builtins · 10. Deviations and quirks ·
11. Arrays and findall.

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

**`for`/`foreach` statements** (language owner request, 2026-08-21; section
12): `m_ruleForeachStatement`/`m_ruleForStatement`, tried before
`m_ruleIfStatement`/`m_ruleSingleGoal` in `m_ruleAnyStatement` — the same
kind of structural/positional reservation `if` already has (section 10),
not by exact name+arity like `cut`/`query`.

---

## 3. Terms

Four term kinds under `AbstractTerm` (`include/vault-unify.hpp`):
`ConsTerm`, `MapTerm`, `ArrayTerm`, `VarTerm`. No separate number/string
kind.

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
']');`, e.g. `[a, b, c]`. `AnyTermFactory::operator()(const ArrayTermInput&)`
(`src/vault-unify-parser.cpp`) builds a distinct `ArrayTerm`
(`include/vault-unify.hpp`, `src/vault-unify-term-array.cpp`) — an ordered
`std::vector<AbstractTerm*>`, unrelated to `MapTerm`, with ordinary
positional (index-by-index) unification. See section 11 for the full
unification rule and for `findall`, which produces `ArrayTerm` results.
*(Superseded, ROADMAP Phase 2 "Arrays and findall", 2026-08-21)*: arrays
used to desugar straight into a `MapTerm` keyed by stringified 0-based
indices ("0","1","2",...) — **arrays were maps with numeric-looking string
keys, not a distinct term kind**, and those keys sorted lexicographically
once an array had 10+ elements ("10" < "2"), a trap for anything inspecting
the `MapTerm` directly (invisible to ordinary array/array unification,
since both sides built identical keys in identical order). Section 11 below
is the replacement; this quirk no longer applies.

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

**Reconciliation with cut (section 8, ROADMAP Phase 2)**: prefix `!` and
the goal statement `cut;` are unrelated constructs that happen to share a
symbol family in Prolog folklore but nothing else here. `!` stays exactly
as described above — a per-candidate result transform applied while
walking the *same* backtracking machinery as any other goal, explicitly
**not** committing (previous paragraph). `cut` is a distinct reserved goal
name (not an operator, not spelled `!`) that actually does commit — it
prunes choice points on the solver's own `SolveContext` stack, something
no per-candidate transform like negation could do. Introducing `cut`
required no change to `!`'s grammar or semantics at all.

---

## 8. Cut

ROADMAP Phase 2 ("Cut (`!` in Prolog's sense) or a committed-choice
construct; reconcile with the current prefix-`!` negation syntax"). The
cut is the keyword goal statement `cut;` — a C-feel goal statement, not a
punctuation operator — deliberately distinct from prefix `!`, which
remains negation-as-failure, completely unchanged (section 7). The two
share nothing: `!` is parsed as an operator prefixing a `SingleGoal`
(`AnyTermFactory::operator()(const SingleGoalInput&)`,
`src/vault-unify-parser.cpp`) and read per-candidate inside
`UnifyContext::getUnificationResult()`; `cut` is a distinct reserved goal
name, recognized structurally before clause iteration ever begins
(`SolveJob::performSlice()`, `src/vault-unify-solvejob.cpp`). Writing
`!cut;` parses (negation applies to whatever ConsTerm the rest of the
statement evaluates to) but is not a construct this module gives any
useful meaning to and is not used anywhere.

**Reservation**: `cut` is a reserved word for a zero-argument goal, the
same shape as the `query` reservation (section 2): a user clause literally
named bare `cut` with no arguments can no longer be defined as such (the
bareword is renamed to the reserved internal name before a `Clause` is
ever built), but `cut(a)`, `cut(a, b)`, … remain perfectly ordinary
clause heads/calls, unaffected — exactly as `query(a) { ... }` remains
expressible once it takes an argument. Unlike `query` (a purely
grammar-positional reservation — `m_ruleQuery` only ever fires at the
start of a top-level form), `cut`'s reservation is by exact name+arity,
applied wherever a bareword `ConsTerm` is built at all —
`AnyTermFactory::operator()(const ConsTermInput&)` renames `cut` (zero
values) to the internal atom `__builtin_cut` before constructing the
`ConsTerm`, covering a goal statement, a clause head, or a plain data
argument with the one change. `cut` is deliberately **not** a registered
`Clause`/builtin (contrast every predicate in section 9 below): a builtin
only ever sees the two `UnifyContext`s and the `Engine` (`Clause::
startUnification`'s signature, `include/vault-unify.hpp`), never the
`SolveJob`'s own `SolveContext` stack — and cutting requires reaching
into exactly that stack to prune choice points. `SolveJob::performSlice()`
checks the current goal term for this exact reserved name before it ever
consults a clause iterator; a `__builtin_cut` term is never looked up in
any `ExecutionState`'s clause list.

**Semantics** — standard Prolog cut, scoped to the clause (or query)
containing it. When `cut` executes in the body of clause C that was
selected for goal G, it succeeds exactly once and commits: (a) no further
clause alternatives are tried for G, and (b) goals in C's body to the LEFT
of the cut (and G's own bindings) do not backtrack into new solutions.
Goals AFTER the cut backtrack normally within their own subtree — a cut
never reaches forward. A cut in a query's own top-level goal chain commits
the query up to that point; the query itself is "the clause" in that case,
there being no enclosing call that selected it.

**Mechanism** (`SolveJob::performSlice()`): `SolveContext` forms a call
tree via `m_parentSolveContext`, mirrored 1:1 onto the flat, genuinely
LIFO `m_stackContext` (`SolveContext*` are only ever removed from its
back, in `discardTop()`) — so at any moment, walking `m_stackContext` from
back to front visits exactly the still-live contexts in push order, most
recent first. Every `GoalPart` (a clause/query body, or a fragment
continuing after one) is the `m_pMyGoalPart` of exactly one
`SolveContext` — the one pushed for that body's first term, in the
"non-terminal" branch of `performSlice()`'s unification-success handling
— every other `SolveContext` covering a later term of the same body is
instead pushed by the "terminal continuation" branch, which always passes
`pNewGoalPart=NULL`. So, given the `SolveContext` `sc` currently executing
`cut`, with `X = sc->m_csCurrent.getGoalPart()` (the `GoalPart` — clause
body or query root — that owns the cut): walking `sc`'s
`m_parentSolveContext` chain up to the unique ancestor `p` with
`p->m_pMyGoalPart==X` finds the choice point for X's own first body term;
`p->m_parentSolveContext` is then the context that matched G against
clause C and pushed `p` — the ENTRY context whose `m_itNextChildClause`
enumerates G's remaining alternatives. At query top level `p` has no
parent (it is the root `SolveContext` `SolveJob::startJob()` pushes
directly) — there being no enclosing call to also cut off, `p` itself is
the entry context in that case.

Pruning invalidates (`ExecutionState::ClauseIterator::invalidate()`,
`include/vault-unify.hpp` — a `bool` `isValid()` now checks first,
independent of and checked before the pre-existing parent-`ExecutionState`
fallback walk) every context's `m_itNextChildClause` from `m_stackContext`'s
back down to and including the entry context — never popping anything, so
continuations already found above the cut keep running; a pruned context
simply falls into the ordinary "no next child clause" `discardTop()` path
once the search naturally backtracks into it. This provably covers exactly
the right set: everything between the current top and the entry context
(inclusive) is, by the stack's LIFO discipline, exactly what was pushed
since the entry context ran — every choice point for a goal to the left of
the cut (including one left behind by a fully-resolved sibling subtree,
e.g. a preceding goal that itself called into a further user-defined
rule — correctly pruned, it is still "to the left"), the choice point for
X's first body term itself, and G's own remaining alternatives — while
nothing belonging to a goal after the cut is ever touched, because no
`SolveContext` for it exists yet. It also invalidates `sc`'s own iterator
(guarding a corner case: were a user clause literally named bare `cut` to
exist — parsed to this very same reserved name, see above — it would
otherwise wrongly unify with the cut term itself once backtracking
returns to `sc`).

Having pruned, `cut` then advances exactly like an ordinary terminal
clause match (fact/no-continuation): `GoalPartCursor csNext( sc-
>m_csCurrent ); csNext.next();`, pushing a child `SolveContext` carrying
forward `sc->m_pUnifyContext` unchanged (`cut` binds nothing new, so no
fresh `UnifyContext` is created) with `pNewGoalPart=NULL` — identical
shape to `performSlice()`'s existing `!haveNewGoal` push.

**Upgrading `if` to real if-then-else** (section 4): the synthesized
then-clause now reads `__if_N( V1..Vk ) { clonedCond; __builtin_cut;
clonedBody...; }` — cond's first success now commits against the fallback
clause, so `if` is a true if-then-else instead of the previous soft-if
(`( cond, body ; true )`, no commit — old section 9 QUIRK, since
superseded): if cond fails, the then-clause never reaches the cut and the
fallback (always succeeds, empty body) runs; if cond succeeds, cut prunes
the fallback and cond's own remaining alternatives. See
`test/conformance/if-statement.ufy` — its first query used to print the
line after the `if` twice (once via the then-branch, once via the
still-tried fallback); with cut it prints once.

**Conformance**: `test/conformance/cut.ufy` — (a) first-solution commit
across a rule's own body (`first($x) { c($x); cut; }` against three `c/1`
facts, prints the first only); (b) cut also stops backtracking into goals
to the LEFT of the cut, not merely the enclosing call (`pair($x,$y) {
a($x); b($y); cut; }` against two facts each of `a/1`/`b/1`, prints only
the very first pair); (c) goals AFTER the cut are unaffected and backtrack
normally (`after() { cut; c($z); print($z); }`, prints all three `c/1`
solutions); (d) cut at query top level (`query { c($x); cut; print($x);
}`, prints only the first).

---

## 9. Builtins

All eight appended to the root state, in this order, before any user clause
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

**`__builtin_array_at($arr, $idx, $out)`** — `ArrayAtBuiltinClause`
(`src/vault-unify-clause-builtin-array.cpp`, "for"/"foreach" loops, language
owner request 2026-08-21). What `foreach`'s synthesized loop clause's own
`__builtin_array_at($arr, $i, $x);` goal resolves (section 12) — never
written directly in a user program. Head declared arity 3, checked
manually. Argument 0 must resolve (`AbstractTerm::getBoundTerm`) to an
`ArrayTerm` (anything else, including unbound, is a silent `UnifyNot` —
mirroring `__builtin_member_deref`'s non-map left side); argument 1 must
resolve to a bound, 0-arity, integer-parseable `ConsTerm` atom (otherwise a
`UnifyError`); an out-of-range index (negative, or `>=` the array's length)
is an ordinary `UnifyNot` — this is `foreach`'s own loop-termination signal.
On a hit, unifies the element against argument 2.

**`__builtin_range($a, $b, $out)`** — `RangeBuiltinClause`
(`src/vault-unify-clause-builtin-arith.cpp`, "for"/"foreach" loops + ranges,
language owner request 2026-08-21). What a range with a non-literal bound
desugars to (section 12.3) — a range with both bounds literal is expanded
eagerly at parse time instead and never reaches this builtin. Resolves both
bounds via `evaluateArith()` (this file's own anonymous namespace, shared
with `__builtin_eval`/`__builtin_compare`) — so a bound may be a bare
integer atom, a variable bound to one, or itself a `__builtin_arith`
expression; an unbound/non-numeric bound is a `UnifyError`. Builds a fresh
`ArrayTerm` of one 0-arity `ConsTerm` atom per integer in `[a, b]`
(inclusive; empty if `b < a`), capped at 100000 elements (a `UnifyError`
beyond — unlike the parser's own eager literal-bounds path, which silently
truncates instead, since there is no clean way to fail a parse-time
desugaring the way a builtin can return `UnifyError`), adopted via
`UnifyContext::adoptTerm()` exactly like `__builtin_eval`'s/`findall`'s own
fresh solve-time results, and unified against argument 2.

---

## 10. Known deviations and quirks

All verified by direct code reading, collected here for quick reference.

- Bracket deref `lhs[rhs]` parses but always fails at run time — no
  `__builtin_array_deref` clause is ever registered (section 4).
- Prefix `!`'s handling inside `PrefixTermInput` (`__builtin_not`) is dead
  code; the grammar only ever feeds `'-'` there. The `!` that matters is
  parsed at the statement (`SingleGoalInput`) level (section 4).
- Prefix `-` does not implement negation: due to `case`/`default` label
  nesting, it always builds an anonymous, empty-named wrapper `ConsTerm`
  around its operand rather than negating a number (section 4).
- *(Superseded, ROADMAP Phase 2 "Cut", 2026-08-20)* `if (cond) { body }`
  used to be a soft-if (`( cond, body ; true )`, section 4) with no cut: the
  synthesized fallback clause was tried on backtracking even after `cond`
  already succeeded once, so whatever followed the `if` in the same goal
  chain could run again via the fallback path. Now that `cut` exists
  (section 8), the then-clause commits against the fallback once `cond`
  succeeds — see `test/conformance/if-statement.ufy` (its first query used
  to print the line after the `if` twice for exactly this reason; it prints
  once now).
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
  is tried (section 5). `__builtin_findall` (section 11) is not appended at
  all — like `__builtin_cut`, it is never a registered `Clause`, only a goal
  term `SolveJob::performSlice()` recognizes structurally — so a user rule
  named `__builtin_findall` (unlikely, but not grammatically prevented)
  would simply never be reached; `SolveJob::performSlice()` intercepts the
  term before any clause lookup happens at all.
- `->`/member-deref (`__builtin_member_deref`, `MemberBuiltinClause`,
  section 9) requires its left-hand side to resolve to a `MapTerm`
  specifically (`dynamic_cast<const MapTerm*>`) — this already excluded
  arrays in spirit even when they still desugared into a `MapTerm`
  (section 3's now-superseded quirk was the only reason `->`-on-an-array
  ever appeared to typecheck at all). Now that `ArrayTerm` is a distinct
  kind (section 11), `->` against an array simply fails outright, the same
  as `->` against any other non-map left-hand side; there is still no
  dedicated array-indexing syntax (`lhs[rhs]` remains the pre-existing
  unimplemented quirk immediately above).
- `ExecutionState::fork()` and the resulting parent-state clause fallback
  are fully implemented but never invoked anywhere in this module today
  (section 5) — not a bug, just currently-unused machinery.
- `UnifyResult::UnifyNotLast` is defined and handled identically to
  `UnifyLast` wherever it's read, but no `unify*Term` implementation ever
  returns it (section 6) — reserved for functionality that doesn't exist
  yet.
- **`for`/`foreach` are reserved statement keywords** (section 12), the same
  kind of reservation `if` already has (section 2): structural/positional,
  by grammar ordering (`m_ruleForeachStatement`/`m_ruleForStatement` are
  tried before `m_ruleSingleGoal` in `m_ruleAnyStatement`), **not** by exact
  name+arity like `cut`/`query`. A goal statement that merely happens to
  call a same-named predicate but does not match the full loop-header-plus-
  `{ }`-body shape simply fails to match here and backtracks to
  `m_ruleSingleGoal`, parsing as an ordinary call. A clause **head** named
  `for(...)`/`foreach(...)` is entirely unaffected either way —
  `m_ruleAnyStatement` is never reached from `m_ruleClause`, which parses a
  clause head via `m_ruleConsTerm` directly, exactly as already documented
  for `query`'s own reservation.
- `foreach`'s loop-variable position and `for`'s init/step assignment LHS
  are conventionally, but **not** grammatically enforced, a `$name`
  variable (section 12) — `m_ruleConsTerm`/`m_ruleSingleGoal` accept
  anything their own grammar does. If the actual parsed term is not a
  `VarTerm`, `AnyTermFactory` logs and substitutes a fresh, unbound variable
  instead of crashing; the original (now-orphaned) term is not further
  leak-hardened — an unsupported shape, not exercised by any test.
- A classic `for` loop's synthesized clause commits to `cond`+`body`'s
  first solution each iteration via its own `cut` (section 12.2) — the
  same commit-once-per-iteration discipline `if`/`foreach` use, and for the
  same reason: without it, this engine's exhaustive all-solutions
  backtracking would also retry the loop's fallback fact for every
  already-completed iteration once the loop ends, printing everything
  after the loop multiple times (the pre-`cut`-era "soft if" bug, directly
  above). `for` deliberately still has **no** `foreach`-style `feb`
  body-or-true wrapper, so a failing `cond`/`body` genuinely stops the loop
  (section 12.2) — the `cut` commits a *successful* iteration; it does not
  cushion a failing one.
- **Significant, engine-level finding (not specific to loops) uncovered
  while implementing section 12, verified independently by two separate
  investigations**: reusing the exact same `VarTerm*` C++ object as both a
  clause's head parameter and, *unchanged*, as an argument to that SAME
  clause's own recursive self-call — the ordinary way any hand-written
  recursive predicate threads an invariant argument through recursion,
  e.g. `p(Arr, I) :- ..., J is I+1, p(Arr, J).` with `Arr` reused — does
  **not** actually propagate that value past the *first* recursive call.
  A second (or deeper) invocation sees the variable as **unbound**. The
  mechanism: `VarTerm::unifyVarTerm`'s `if( this == pOther ) return
  UnifyLast;` fast path (`src/vault-unify-term-var.cpp`) fires on raw
  pointer identity alone (no scope comparison) and — unlike the ordinary
  variable-unification path just below it — never calls `bindVarBinding`/
  `bindVarBindingUsing`, so **no `AssignmentId` entry is ever written**.
  Since every clause-try gets its own fresh `UnifyContext` (`pUCCand`,
  `SolveJob::performSlice()`, `src/vault-unify-solvejob.cpp`) and a
  variable's binding key is `AssignmentId(uidScope, varTermId)` with
  `uidScope` tied to the ACTIVATION that introduced it (section 6) —
  `UnifyContext::findVarBinding` (`src/vault-unify-unifycontext.cpp`) walks
  the parent chain but searches for one fixed, exact `(uidScope, varId)`
  key at every level; an entry recorded under an ANCESTOR invocation's own
  `uidScope` can never satisfy a lookup keyed by a DESCENDANT invocation's
  different `uidScope`, no matter how far the walk goes. `foreach`/`for`
  (section 12) do **not** rely on this pattern precisely because of this —
  every value threaded through either construct's recursive call (`$arr`,
  every captured `V1..Vk`, even though they are genuinely invariant) is
  rebound via an explicit `unify(freshVar, current)` goal each iteration
  instead, forcing the ordinary (non-identity, scope-bridging) unification
  path. This finding is not otherwise acted on here (fixing it generally is
  out of scope for this task) but is recorded because it may affect
  ordinary hand-written recursive predicates elsewhere in this module —
  worth a dedicated look in a later ROADMAP phase.

---

## 11. Arrays and findall (ROADMAP Phase 2)

ROADMAP Phase 2 ("consistent list/array semantics + findall"). Two related
changes: array literals (`[a, b]`) become a genuine, distinct term kind
instead of desugaring into a `MapTerm`; and `findall` becomes a solver
special form that collects every solution of a sub-goal into an array.

### 11.1 Arrays

**Term kind**: `ArrayTerm` (`include/vault-unify.hpp`,
`src/vault-unify-term-array.cpp`) — an ordered `std::vector<AbstractTerm*>`,
built directly from the literal syntax (`m_ruleArrayTerm`, section 1/3;
`AnyTermFactory::operator()(const ArrayTermInput&)`,
`src/vault-unify-parser.cpp`, now builds an `ArrayTerm` instead of a
`MapTerm`). Two constructors: `ArrayTerm(AbstractTerm** ppTerms, int
nTerms)` (mirrors `ConsTerm`'s — the transient array's pointer *values* are
copied out, the array itself stays the caller's to free) for the literal
syntax, and an empty `ArrayTerm()` plus `append()` for building a result
incrementally (`findall`, section 11.2, below). `~ArrayTerm()` is trivial
(no keys of its own to free, unlike `~MapTerm()` — see section 3).

**Double dispatch**: `AbstractTerm` gained a fifth pure-virtual method,
`unifyArrayTerm(...)`, alongside `unifyTerm`/`unifyConsTerm`/
`unifyVarTerm`/`unifyMapTerm` (`include/vault-unify.hpp`) — every existing
concrete kind (`ConsTerm`, `VarTerm`, `MapTerm`) and the new `ArrayTerm`
implement it:
- `ConsTerm::unifyArrayTerm` / `MapTerm::unifyArrayTerm` → `UnifyNot`
  (`ArrayTerm` is unrelated to both — section 6's "different term kinds
  never unify" rule, unchanged).
- `VarTerm::unifyArrayTerm` → forwards to
  `UnifyContext::genericUnifyVarWithKnown` (same generic-bind path every
  other kind uses against a variable).
- `ArrayTerm::unifyConsTerm` / `ArrayTerm::unifyMapTerm` → `UnifyNot`
  (symmetric with the two bullets above).
- `ArrayTerm::unifyVarTerm` → forwards to `genericUnifyVarWithKnown`
  (symmetric with `VarTerm::unifyArrayTerm`).
- `ArrayTerm::unifyArrayTerm` (`src/vault-unify-term-array.cpp`) — the real
  rule: identical length required first (any mismatch is an immediate
  `UnifyNot`, no partial/prefix match), then element-by-element positional
  unification via `UnifyContext::unifyTerms` for each index, with the same
  `UnifyError`-propagates-immediately / `!res` → `UnifyNot` check
  `ConsTerm::unifyConsTerm` uses for its sub-terms (section 6) — there is
  simply no name to compare first (an array has none, unlike `ConsTerm`).
  `ArrayTerm::unifyTerm`/`ArrayTerm::unifyMapTerm`/etc. dispatch exactly
  like `ConsTerm`'s/`MapTerm`'s own `unifyTerm` (self-identity fast path,
  then the second-order call on `pOther`).

**Rendering**: `toString()`/`toContextString()`/`toJSON()` all render as
`[elem1, elem2, ...]` (`", "`-joined, no trailing separator, `"[]"` for an
empty array) — `ArrayTerm::toString()` etc., `src/vault-unify-term-array.cpp`,
mirroring `MapTerm`'s per-element delegation style. Because `print`/`emit`
(section 9) already resolve every argument generically via
`toContextString`/`toJSON` (virtual dispatch, no per-kind switch in
`PrintBuiltinClause`/`EmitBuiltinClause`), both render arrays correctly with
no changes needed there.

**Tree-walking utilities**: `collectTermTree`/`deleteTermTree` (generic,
via `abstractTermIterator()`) needed no changes at all. `cloneTermTree`
(`src/vault-unify-terms.cpp`, used by the `if`-statement desugaring,
section 4) gained an `ArrayTerm` branch (enumerate via `getElements()`,
clone each element, rebuild via the transient-array constructor — same
ownership pattern as its `ConsTerm`/`MapTerm` branches).

**Conformance**: `test/conformance/findall-arrays.ufy` (a) array/array
unification, equal length and elements, and (b) unequal length (fails
silently, same as any other structural mismatch); (c) an array stored in a
fact, retrieved through a variable and printed, pinning the
`"[a, b, c]"` rendering.

### 11.2 findall

**Syntax and reservation**: `$out = findall( $tmpl, $goal );` —
`AnyTermFactory::operator()(const InfixTermsInput&)`'s `case '=':`
(`src/vault-unify-parser.cpp`) recognizes a side of `=` that is a `ConsTerm`
literally named `"findall"` with **exactly 2** arguments (mirroring how
`"cut"` is reserved by exact name+arity, section 8) and desugars it to
`__builtin_findall(tmpl, goal, otherSide)`, discarding the now-unreachable
`findall(...)` wrapper `ConsTerm` node (its two children are reused
directly; freed the same way `deleteScratchTermTree()` frees other orphaned
structural scratch nodes elsewhere in this file). This is a **choice, not
an oversight**: `findall` used at any OTHER arity, or anywhere other than a
side of `=`, is left as a completely ordinary `ConsTerm`/predicate call —
exactly like `"query"`/`"cut"` staying ordinary once they take the "wrong"
shape for their respective reservations (section 2, section 8). If *both*
sides of `=` happen to be this exact `findall(...)` shape, the left side
wins (undefined-but-harmless — not exercised by any test, mirroring the
identical tie-break already documented for the arithmetic-in-`=` desugar,
section 4.1).

**Solver special form**: `__builtin_findall(tmpl, subgoal, out)` is
recognized directly in `SolveJob::performSlice()`
(`src/vault-unify-solvejob.cpp`), structurally, before clause iteration —
exactly like `__builtin_cut` (section 8) and for the same reason: it needs
machinery (running a whole nested search) no `Clause::startUnification()`
implementation ever gets access to (that signature only ever sees two
`UnifyContext`s and an `Engine`).

**Mechanism**:
1. A fresh `SolveJob` is constructed **on the stack, driven synchronously,
   never enqueued on the `Engine`** (`nestedJob.setWorld(...)`;
   `setGoal(&nestedGoal)` with `nestedGoal` wrapping `subgoal` alone;
   `startJob(...)`; one direct `performSlice()` call). This is safe and
   sufficient because `Job`'s default debug target state is `REGULAR`
   (`Job::Job()`, `src/vault-unify-job.cpp`), and `performSlice()` under
   `REGULAR` runs to full exhaustion in a single call — the same property
   `unify-run.cpp`'s "barrier job" trick already relies on (see its
   comment). The nested job solves `subgoal` **as if it were a brand-new
   top-level query**: its own root `UnifyContext` has no parent, exactly
   like `SolveJob::startJob()`'s own root context.
2. Every solution the nested job found (`m_listUnifySolutions`) contributes
   one **ground copy** of `tmpl`, via the new helper `resolveTermGrounded()`
   (`src/vault-unify-terms.cpp`, declared next to `cloneTermTree()` in
   `include/vault-unify.hpp`): it walks `tmpl` exactly like `cloneTermTree`
   structurally, but instead of substituting via a caller-supplied map, it
   resolves each `VarTerm` against the solution's `UnifyContext`
   (`findVarBinding`/`findVarInstance`, the same pair
   `SolveJob::getSolutionList()` and `VarTerm::toContextString()` use) and
   recursively resolves the bound instance term too (in *its own* binding's
   `UnifyContext` — a variable can be bound to a term that itself still
   contains unresolved variables scoped elsewhere); an unbound variable is
   cloned as a fresh, unbound `VarTerm`. Every node returned is a fresh
   allocation, because the nested job's entire arena is destroyed the
   moment this block finishes — nothing may keep pointing into it.
3. The ground copies are collected into a fresh `ArrayTerm` (`append()`,
   section 11.1), adopted into a fresh `UnifyContext` (parented on the
   current solve chain) via `UnifyContext::adoptTerm()`, and unified against
   `out` exactly the way `__builtin_eval`'s evaluated result is
   (`ArithEvalBuiltinClause`, `src/vault-unify-clause-builtin-arith.cpp`):
   same `UnifyContext` mechanics, same ownership idiom.
4. Findall then advances **exactly once**, like a terminal (fact/no-
   continuation) clause match, or like `cut` — never both: it invalidates
   its own `SolveContext`'s `m_itNextChildClause` first (so a later revisit
   falls into the ordinary "no next child clause" `discardTop()` path
   instead of re-recognizing and re-running this same term — the identical
   reasoning `cut` documents for its own iterator invalidation, section 8),
   then pushes a continuation `SolveContext` **only if** the array-vs-`out`
   unification actually succeeded.

**Determinism and failure modes**: `findall` itself never fails — zero
solutions of `subgoal` simply produce an empty `ArrayTerm` (`"[]"`,
section 11.1) — and it is deterministic: exactly one solution, no choice
point (nothing is ever tried a second way). The one way the overall goal
can still fail or error is the same as any ordinary unification: binding
the (possibly empty) result array against `out` can itself fail
(`out` already bound to an incompatible value) or error, handled exactly
like any other `UnifyError` elsewhere in `performSlice()` (recorded via
`recordError()`, then treated as `UnifyNot`).

**Ownership**: the nested job's own arena (`UnifyContext`s, `GoalPart`s,
`SolveContext`s) is freed by its own `~SolveJob()` when the nested
`SolveJob`/`Goal` go out of scope — safe to call directly on a stack
object that was never enqueued, since nothing in `~SolveJob()` depends on
having gone through the `Engine`'s scheduler (`triggerRelease()` is a
no-op; the arena vectors are freed unconditionally). The result `ArrayTerm`
and every grounded element in it are independent allocations built by
`resolveTermGrounded()` — they do not point into the nested job's arena at
all, so their lifetime is entirely decoupled from it; they are freed
instead (as one tree, via `collectTermTree()`) when the *outer* job's
adopting `UnifyContext` is destroyed. This is why `UnifyContext::~UnifyContext()`
(`src/vault-unify-unifycontext.cpp`) was generalized from a single, flat
`delete` (correct only for `__builtin_eval`'s original single-leaf-atom use
case) to a `collectTermTree()`-based, de-duplicated sweep over every
adopted term's *entire* tree — the same pattern `World::~World()`/
`~SolveJob()` already use for their own term trees.

**Variable-scope analysis (why this is correct)**: `SolveJob::getSolutionList()`
already resolves every top-level query variable via
`AssignmentId(0, varTerm->getBinding())` — unify-context id `0` meaning "no
parent scope", exactly what a variable occurring directly in a fresh
top-level query's own goal gets (`GoalPart`'s origin `UnifyContext*` is
`NULL` for such a root goal part — see `SolveJob::startJob()` — and
`genericUnifyVarWithKnown` maps a `NULL` scope pointer to unify-context id
`0`). Because the nested job solves `subgoal` the exact same way (its own
fresh root, unrelated to whatever scope enclosed the `findall(...)` call),
and because `tmpl`/`subgoal` share the identical `VarTerm*` objects for any
variable appearing in both (one `ClauseContext` symbol table per
clause/query, section 2/4), `resolveTermGrounded(tmpl, solutionUC, NULL)`
resolves those shared variables correctly: it looks them up as
`AssignmentId(0, ...)` against `solutionUC`, and the binding is reachable
by walking `solutionUC`'s `UnifyContext` parent chain (which mirrors the
nested job's own `SolveContext` call tree exactly) up to wherever it was
recorded — regardless of how deep inside `subgoal`'s own predicate calls
that binding actually happened, since only a real clause match creates a
new `UnifyContext`/scope level, never mere argument nesting within one
`ConsTerm`.

**v1 scope limitation (documented, not fixed here)**: the nested job solves
`subgoal` as a **fresh** top-level goal — outer bindings already in scope
at the `findall(...)` call site are **not** consulted. E.g.
`$y = 1; $all = findall($x, related($x, $y));` would solve `related($x,
$y)` with `$y` **unbound** in the nested search, not `1` — `subgoal` is
handed to the nested job by raw term reference; nothing propagates the
enclosing `UnifyContext`'s bindings into it. Supporting that would require
either re-resolving `subgoal` (like `resolveTermGrounded` does for `tmpl`)
before handing it to the nested job, or threading the outer `UnifyContext`
in as the nested root's parent — both nontrivial (the latter risks
`UnifyContext` id/scope collisions between the outer chain and the "fresh
top-level query" invariant `getSolutionList()`-style resolution above
depends on) and deliberately left for a later pass; this is the "v1 may
document a fresh-scope subgoal" option the design explicitly allowed for.

**Conformance**: `test/conformance/findall-arrays.ufy` (d) findall
collecting three fact solutions, template `$x` shared with the sub-goal,
printed as an array in solution (== clause definition) order; (e) findall
over an undefined predicate — zero solutions, an empty array, not a
failure; (f) a findall result unified against an equivalent array literal,
element by element.

---

## 12. Loops and ranges (language owner request, 2026-08-21)

The language owner requested a classic `for` loop, `foreach`, and — as
stage 1 of a wider constraint-domain wish — range values as data. All three
are desugars performed by `AnyTermFactory` (`src/vault-unify-parser.cpp`),
following the exact same ownership discipline the `if` desugar established
(section 4): any term tree spliced into a clause permanently appended to
`World`'s root `ExecutionState` must not alias a `VarTerm` with the
enclosing, per-query/-clause-lifetime term tree (`World::~World()`/
`~SolveJob()`'s comments) — so every synthesized clause below gets its own
fresh substitution via `cloneTermTree()`, exactly like `if`'s rule/fact
pair.

### 12.1 Ranges: `<a>..<b>`

**Grammar**: `m_ruleRange %= (m_ruleAdditive >> -(qi::lit("..") >>
m_ruleAdditive));`, inserted between `m_ruleAdditive` and `m_ruleComparison`
(so `m_ruleComparison` now operates on `m_ruleRange`'s result instead of
`m_ruleAdditive`'s directly) — a range's bounds are themselves
additive-level expressions (may be arithmetic, e.g. `1+1..5`), and since
this level sits below every other precedence level, a range is usable
anywhere any other `AnyTerm` is, not only inside `foreach`'s header. `".."`
is a plain two-character `qi::lit` token contributing no attribute of its
own; nothing else in this grammar ever uses a bare `.`, so there is no
longest-match ambiguity to resolve (unlike `==`/`!=`/`<=`/`>=` vs. `<`/`>`
just above it).

**Simplest honest rule (v1 design choice)**: a range is **not** a new
persistent term kind. `AnyTermFactory::operator()(const RangeTermInput&)`
builds both bounds first; if BOTH turn out to be literal, 0-arity `ConsTerm`
atoms whose text parses fully as an int64, the range is expanded **eagerly,
right here at parse time**, into a plain `ArrayTerm` literal —
indistinguishable from writing the array out by hand (`1..3` **is**
`[1, 2, 3]`, nothing more; the two now-redundant scratch bound terms are
deleted directly, mirroring the orphaned-wrapper-`ConsTerm` deletion the
`findall` desugar already does, section 11.2). This eager expansion is
capped at 100000 elements (`RANGE_MAX_ELEMENTS`,
`src/vault-unify-parser.cpp`), silently truncated (logged) beyond — there is
no clean way to fail a parse-time desugaring the way a builtin can return
`UnifyError`.

Otherwise (either bound is a variable, or itself an arithmetic expression)
the range is genuinely dynamic: a fresh anonymous var is substituted for it
(mirroring `->`'s own pre-goal pattern, section 4) and a
`__builtin_range(lhs, rhs, freshVar)` pre-goal is pushed onto the enclosing
statement's own term list, resolved at **solve time** by `RangeBuiltinClause`
(section 9) — which builds the same kind of `ArrayTerm`, capped at the same
100000 elements, but as a genuine `UnifyError` beyond the cap (a builtin
*can* fail the goal; the parser cannot). Consequence of "a range is usable
anywhere any other AnyTerm is": `1..$n` works as a plain data expression
too, not only inside `foreach ( $i : 1..$n )`.

### 12.2 `for ( $i = init; cond; $i = step ) { body }`

**Grammar**: all three header pieces reuse `m_ruleSingleGoal` (the very
same production a bare goal statement, or `if`'s condition, already uses) —
`m_ruleForStatement %= qi::lit("for") >> qi::lit("(") >> m_ruleSingleGoal >>
';' >> m_ruleSingleGoal >> ';' >> m_ruleSingleGoal >> qi::lit(")") >>
qi::lit("{") >> m_ruleGoal >> qi::lit("}");`. `initAssign`/`stepAssign` are
conventionally, but **not** grammatically enforced, a `$var = expr`
assignment (see section 10's quirk on this).

**Desugaring** (`AnyTermFactory::operator()(const ForStatementInput&)`):
only **one** synthesized auxiliary predicate is needed (unlike `foreach`,
section 12.3) — a rule clause and a fallback fact, exactly the same
two-clause shape `if` uses:

```
__for__3( $i, V1..Vk ) {
    clonedCond;
    clonedBody...;
    cut;                         // commit THIS iteration
    $i2 = clonedStep;           // fresh $i2, NOT one of V1..Vk
    $v1Next = V1; ...; $vkNext = Vk;   // rebind, see below
    __for__3( $i2, $v1Next..$vkNext );
}
__for__3( $i, V1..Vk );         // fallback: cond false -> loop ends
                                 // (also reached, via ordinary goal
                                 // failure+backtracking, if cond or
                                 // body ever fails)
```

`V1..Vk` (every captured variable OTHER than `$i`) are genuinely invariant
across the whole recursion, but are still **not** passed to the recursive
call as the literal same head-parameter object again — doing so would rely
on `VarTerm::unifyVarTerm`'s `this==pOther` identical-object fast path
(`src/vault-unify-term-var.cpp`) — which returns `UnifyLast` immediately
**without recording any `AssignmentId` binding at all**, since both sides
are literally the same pointer — to still make the value visible several
recursion levels down. `$v1Next = V1;` etc. (an ordinary `unify(...)` goal
per captured variable, run once per iteration — ordinary `=` between two
non-arithmetic terms, section 4) forces a genuine, ordinary variable-to-
variable binding through `UnifyBuiltinClause` instead, for every iteration
— see `foreach`'s identical treatment of `$arr`/its own `V1..Vm` below for
the full reasoning (this task's session report has the investigation this
design choice is based on: a fresh, never-before-referenced variable linked
to an existing one via ordinary unification always succeeds and is
resolvable from any descendant `UnifyContext`, sidestepping any question of
whether same-object reuse alone would also have worked here).

The `cut` right after `clonedCond`+`clonedBody` mirrors `foreach`'s own
`__fe__3` cut placement exactly (section 12.3) — right after the goals
that must succeed for this iteration to "count", before the step and
recursive call — for the SAME two reasons: (a) **without it**, once the
loop eventually ends and the rest of the enclosing query runs to
completion, this engine's exhaustive all-solutions backtracking would
**also** retry THIS call's own fallback fact as a sibling alternative —
once per iteration already passed through — printing everything after the
`for` loop once per iteration instead of once total (the exact "soft-if"
duplicate-output bug sections 8/10 document for the pre-`cut` `if`); (b) it
also commits `for` to cond/body's FIRST solution each iteration, matching
`if`'s own commit-to-first-solution semantics, rather than letting a
non-deterministic cond/body multiply the recursion. Crucially this does
**not** weaken "body failure stops the loop": `cut` is only ever reached
once cond+body have ALREADY succeeded for this call — a failing body never
reaches it, and the whole rule-clause candidate fails exactly as it would
without the `cut`, falling back to the fallback fact and ending the loop.

`$i` is the header's own control variable (`initAssign`'s LHS), force-
included as the FIRST var-collection root — `collectVarTermsOrdered`'s
dedup guarantees `freeVars[0] == $i` regardless of whether cond/body also
reference it — so it always has a head-parameter slot, and the recursive
call can unambiguously replace exactly that ONE slot with `$i2` while
carrying every other captured variable (`V1..Vk`, every other free `VarTerm`
of cond+body+step) through via its own rebind (above). The init assignment
(`$i = init`) is
built and run **once**, as an ordinary goal in the enclosing scope, right
before the call site (mirroring how `foreach`'s `arrExpr` is evaluated once
up front, section 12.3) — the call site is `__for__3( origV1..origVk )`,
spliced into the enclosing goal chain in place of the `for` statement.

**Step handling (the one subtlety)**: `stepAssign` as WRITTEN (`"$i = $i +
1"`) would, if cloned verbatim like cond/body, produce
`__builtin_eval(__builtin_arith("+", freshI, "1"), freshI)` — binding
`freshI` (already bound, from this call's own head-parameter unification,
to THIS iteration's value `V`) to `V+1` **again**, which never unifies
(`V+1 != V`). Instead, only the step's RHS **expression**
(`stepAssign.rhs.rhs.second`, e.g. just `"$i + 1"`) is built and cloned via
the SAME per-clause substitution cond/body use (so any `$i` within it
correctly reads the CURRENT iteration's fresh value), and its result is
assigned to a BRAND NEW fresh local `$i2` (never one of `V1..Vk`) — via
`__builtin_eval` if the cloned step expression is a `__builtin_arith` tree,
else a plain `unify`, exactly mirroring the `=` desugar's own arithmetic-
vs-plain decision (section 4.1). This sidesteps the double-occurrence
problem entirely, and is the same trick `foreach`'s own `$j = $i + 1` step
uses (section 12.3), spelled out by hand here since `for`'s step is
user-written source, not synthesized.

**Semantics decision — body/cond/step failure STOPS the loop**: no
`feb`-style wrapper (contrast `foreach`, section 12.3) — a failing cond,
body, or step goal fails the rule-clause candidate outright, backtracking
straight to the fallback fact, ending the loop. This is the plain,
uncushioned goal-failure behavior a classic `for` loop is given here
deliberately — C has no notion of a loop body "failing" and continuing to
the next iteration regardless, so failure-stops is the more honest
reading for this construct (contrast `foreach`'s per-element,
silent-failure-tolerant design, matched to the language's overall
"failure is silent and doesn't necessarily propagate" character for
per-element work). A `for` whose `cond` is false from the very first
check runs its body zero times and the loop still succeeds once, exactly
like `if`'s fallback when its own condition is false. The per-iteration
`cut` added right after cond+body (above) does not change any of this —
it only ever runs once cond+body have already succeeded.

**Conformance**: `test/conformance/loops.ufy`'s `for` queries pin this —
counting `0..3`, a `cond`-false-from-the-start loop (zero iterations), and
a body failure stopping the loop partway through.

### 12.3 `foreach ( $x : arrExpr ) { body }`

**Grammar**: `m_ruleForeachStatement %= qi::lit("foreach") >> qi::lit("(")
>> m_ruleConsTerm >> qi::lit(":") >> m_ruleAnyTerm >> qi::lit(")") >>
qi::lit("{") >> m_ruleGoal >> qi::lit("}");` — the loop variable is parsed
via `m_ruleConsTerm` (the same production every other bare `$name` variable
reference in this grammar goes through), conventionally but not
grammatically a `$`-prefixed variable (section 10); `arrExpr` is anything
`m_ruleAnyTerm` accepts, including a range (section 12.1) — so
`foreach ( $i : 1..5 )` and `foreach ( $x : someArrayVar )` are both just
`arrExpr` instantiations of the same rule.

**Desugaring** (`AnyTermFactory::operator()(const ForeachStatementInput&)`):
TWO mutually-recursive auxiliary predicates are synthesized (unique names
via `ClauseContext::nextAnonClauseName()`, e.g. `"__fe__3"`/`"__feb__3"`):

```
__fe__3( $arr, $i, $xSlot, V1..Vm ) {   // main loop
    __builtin_array_at( $arr, $i, $xSlot ); // fails -> index out of bounds
    __feb__3( $xSlot, V1..Vm );              // body-or-true, see below
    cut;                                     // commit THIS iteration
    $j = $i + 1;
    $arrNext = $arr; $v1Next = V1; ...; $vmNext = Vm;  // rebind, see below
    __fe__3( $arrNext, $j, $freshSlot, $v1Next..$vmNext ); // next iteration
}
__fe__3( $arr, $i, $xSlot, V1..Vm );    // fallback: index OOB -> loop ends

__feb__3( $x, V1..Vm ) { clonedBody...; cut; }  // body-or-true, rule
__feb__3( $x, V1..Vm );                          // body-or-true, fallback
```

`freeVars` = the free `VarTerm`s of (`$x`, `body`), with the loop variable
`$x` FORCE-INCLUDED as `freeVars[0]` even if `body` never mentions it (the
same "force-include as the first collection root" trick section 12.2's
`for` uses for its own control variable) — `V1..Vm` = `freeVars[1..]`,
every OTHER free variable. `$arr`/`$i` are BRAND NEW synthesized parameters
(never part of the enclosing scope); `$xSlot` occupies the loop variable's
own position (position 2) so the call site can legitimately pass the
ORIGINAL enclosing `$x` there, keeping it reachable/owned — an early draft
excluded `$x` from `__fe__3`'s parameters entirely, on the theory that it
never needs to be threaded through the recursion (true — see below), which
left it referenced from nowhere in the final term tree and leaked it; this
was caught alongside the recursion bug below (this task's session report
has both).

**Two things must never be threaded unchanged through the recursive
call** (both caught by hand-tracing against `VarTerm` unification, section
6, before this ever reached CI — see this task's session report):

1. **`$xSlot` itself** — it is rebound to a DIFFERENT array element every
   iteration by `__builtin_array_at`, so the recursive call passes a
   BRAND NEW, never-bound `$freshSlot` for that position instead of
   `$xSlot`'s own (by-then-bound) copy. An early draft reused `$xSlot`
   directly there, which broke the loop after its first element: the next
   invocation's own `$xSlot` would already be bound to THIS element, so
   its own `__builtin_array_at` would then try to bind an already-bound
   variable to the NEXT element and fail outright (exactly like `for`'s
   own `$i -> $i2` step, section 12.2, avoids the identical problem for
   its own per-iteration-changing value).
2. **`$arr` and `V1..Vm`** — even though these genuinely ARE invariant
   (the same value every iteration), they are still not passed as the
   literal same head-parameter object again. Doing so would rely on
   `VarTerm::unifyVarTerm`'s `this==pOther` identical-object fast path
   (`src/vault-unify-term-var.cpp`) — which returns `UnifyLast`
   immediately **without recording any `AssignmentId` binding at all**,
   since both sides are literally the same pointer — to still make the
   value visible several recursion levels down. Rather than rely on that,
   `$arrNext = $arr;` etc. (an ordinary `unify(...)` goal per captured
   variable, run once per iteration — plain `=` between two
   non-arithmetic terms, section 4) forces a genuine, ordinary
   variable-to-variable binding through `UnifyBuiltinClause` instead —
   a fresh, never-before-referenced variable linked to an existing one via
   ordinary unification always succeeds and is resolvable from any
   descendant `UnifyContext`, sidestepping any question of whether
   same-object reuse alone would also have worked here.

`arrExpr` is evaluated exactly
**once**, in the enclosing scope (any of its own pre-goals — e.g. a nested
`->`, or a variable-bounds range's `__builtin_range` pre-goal — land in the
enclosing goal chain exactly once, before the loop starts) — unlike `if`'s
`cond`, which is deliberately rebuilt/cloned fresh into the synthesized
clause since it must re-run every iteration; `arrExpr` is a single, fixed
value for the whole loop, evaluated once up front, like a classic
for-each's collection expression.

**Semantics decision — a body failure does NOT stop the loop**: it fails
that one iteration silently and the loop **continues** (the RECOMMENDED
default per this task's brief, matching the language's overall
silent-failure character — contrast `for`'s deliberately different choice,
section 12.2). This is exactly what `__feb__3` buys: WITHOUT it (body
inlined directly into `__fe__3`'s own rule-clause), a failing body would
fail `__fe__3`'s rule-clause candidate outright — no continuation would
ever reach the recursive call — backtracking straight past it to
`__fe__3`'s OWN fallback fact, i.e. STOPPING the loop (indistinguishable
from "index out of bounds"). `__feb__3`'s own fallback fact absorbs exactly
that failure (always succeeds trivially when its rule-clause — i.e. body —
has no solution at all), so `__fe__3`'s rule-clause always reaches `cut`
and the recursive step regardless of whether body succeeded.

**Cut-interaction analysis** (verified against `SolveJob::performSlice()`,
section 8's cut mechanism): the `cut` right after the `__feb__3` call
commits `__fe__3`'s OWN clause choice for THIS call (rule vs. fallback) and
every choice point to its LEFT within this one activation — which includes
pruning `body`'s own remaining alternatives (via `__feb__3`'s own choice,
still on the stack at that point) down to its first solution, standard cut
semantics. Crucially it does **not** reach the recursive `__fe__3(...)` call
written a few lines later: that call has not been pushed onto the
`SolveContext` stack yet when this cut runs (`performSlice()`'s cut handling
only ever invalidates `m_itNextChildClause` on contexts ALREADY on the
stack, walking from the top down to and including THIS activation's own
entry context — the context whose `m_itNextChildClause` enumerates
`__fe__3`'s own alternative clauses, i.e. the one that pushed the very first
body term of THIS invocation) — so the recursive call, made after the cut
returns control to the (unaffected) continuation, gets its own, entirely
unaffected, fresh entry context and fresh choice points once its turn
comes. This is why the recursion is not itself pruned/truncated by the
per-iteration cut — see this task's session report for the full
element-by-element hand-trace of `foreach ( $x : [a, b] ) { print($x); }`
through `performSlice()`.

**`__builtin_array_at($arr, $idx, $out)`**: see section 9. Out-of-bounds is
an ordinary `UnifyNot`, not an error — this is what makes the fallback fact
(loop end) and a body failure (section above) structurally indistinguishable
at the `__fe__3` level, which is exactly the intended "loop just ends
either way" behavior.

**Conformance**: `test/conformance/loops.ufy`'s `foreach` queries pin this
— iterating an array literal, iterating a literal range (`1..4`, expanded
eagerly to `[1, 2, 3, 4]` per section 12.1), and a body failure on one
element that does not stop the remaining iterations.

---

## 13. Runtime `assert`/`retract` (ROADMAP Phase 2)

ROADMAP Phase 2 ("runtime assert/retract — required for device/sensor state
in the home-automation workload"). `assert(Fact);` and `retract(Fact);` are
goal statements — like `cut`/`findall` (sections 8, 11.2), each is a
reserved, exact-name-**and**-arity (exactly one argument) goal name,
recognized structurally in `SolveJob::performSlice()`
(`src/vault-unify-solvejob.cpp`) rather than as a registered `Clause`/
builtin: `assert` needs the `World` itself (to append a new clause to the
root `ExecutionState`'s clause list) and `retract` needs to scan **and**
mutate that same list directly — neither capability a `Clause::
startUnification()` implementation ever gets (its signature only ever hands
it two `UnifyContext`s and an `Engine`, `include/vault-unify.hpp`).

**Reservation**: `AnyTermFactory::operator()(const ConsTermInput&)`
(`src/vault-unify-parser.cpp`) renames a bareword `assert`/`retract`
`ConsTerm` with EXACTLY one argument to the internal atom
`__builtin_assert`/`__builtin_retract` before the `ConsTerm` is built — the
same single choke point, and the same exact-name+arity discipline, `cut`
uses (section 8), so a goal statement, a clause head, or a plain data
argument are all covered by the one change. `assert(...)`/`retract(...)`
with any OTHER arity (zero, or two-plus) are unaffected and remain ordinary
clause heads/calls; a user clause literally named `assert(x)`/`retract(x)`
(exactly one argument) is shadowed — it can no longer be defined or called
as such, exactly like a bareword `cut` clause.

**`assert(Arg)`**: `Arg` is resolved against the CURRENT bindings via
`resolveTermGrounded()` (`src/vault-unify-terms.cpp` — the very helper
`findall`'s per-solution template clone uses, section 11.2) into a fresh,
fully independent clone: every bound `VarTerm` is resolved recursively (in
its own binding's scope), every still-unbound one is cloned as a fresh,
unbound `VarTerm` (`resolveTermGrounded()` itself never fails on this — that
check happens next, explicitly). **v1 requires the clone to be fully
ground**: if the clone's tree contains an unbound `VarTerm` anywhere (a
generic tree walk, `termTreeHasUnboundVar()`,
`src/vault-unify-solvejob.cpp`), the goal fails with a `UnifyError` (raises
the job's error count, `SolveJob::recordError()`) and the (entirely
orphaned — nothing else points at a fresh clone) clone is freed via
`deleteTermTree()`. A ground clone that is not a `ConsTerm` (e.g. an
`ArrayTerm`/`MapTerm` — `Clause`'s head is always a `ConsTerm`,
`include/vault-unify.hpp`) is likewise a `UnifyError`. **v1 is FACTS only**:
asserting a rule (a clause with a body) is out of scope — there is no syntax
to even write one as `assert`'s single argument, since `Arg` is an ordinary
term, never a `{ ... }` body.

On success, the ground `ConsTerm` becomes a brand-new `StandardClause`'s
head with **no** body (`new StandardClause(head, NULL)` — exactly how
`PrologParser::Context::createClause()` builds an ordinary parsed fact),
appended via `ExecutionState::appendClause()` to the **END** of the World's
root `ExecutionState`'s clause list — **assertz semantics**: this engine
already defines "clause order within one predicate name is exactly
[appended] order" (section 5), so appending is the predictable, consistent
choice, not a special case. The new clause is visible to every query from
that point on (including a later match of the very same predicate name),
per section 5's file-order/FIFO-single-worker-thread guarantee — the same
guarantee `print`/`emit`'s own cross-query stdout ordering already relies
on. Ownership: `appendClause()` does not distinguish a solve-time clause
from a parse-time one, so the new clause's term tree is freed exactly once,
with zero extra bookkeeping, by the existing whole-database sweep
(`ExecutionState::collectAllTermTrees()`/`~ExecutionState()`, driven by
`World::~World()`).

**`retract(Arg)`**: scans the root `ExecutionState`'s `m_listClauses` — the
exact list `ExecutionState::ClauseIterator` walks — in definition order,
skipping every builtin (`SimpleBuiltinClause`) and every rule with a
non-empty body (only a `StandardClause` whose `isTerminal()` is true, i.e. a
plain fact, is retractable in v1); for each remaining candidate, tries a
**throwaway trial unification** of `Arg` against the candidate's head,
directly, via `AbstractTerm::unifyTerm()` — mirroring
`UnifyBuiltinClause`'s own minimal direct-call pattern
(`src/vault-unify-clause-builtin-unify.cpp`) rather than going through the
normal candidate-unification machinery
(`SolveJob::startUnification()`/`SolveJob::adoptUnifyContext()`), which
would adopt the trial's `UnifyContext` into the job's own long-lived arena.
The FIRST candidate whose head unifies wins: that clause is tombstoned (see
below) and `retract` succeeds, exactly once (no choice point — the
remaining candidates are never tried). No candidate at all matching is an
ordinary, silent `UnifyNot` — precisely like a goal with no matching
clauses; no error, no output.

**The trial does not let `Arg`'s unification bind anything new outside
itself** ("check-and-remove, not a binding goal" — a deliberate v1 choice):
the trial's own `UnifyContext` (`ucScratch` in the code) is parented on the
CURRENT live chain (`sc->m_pUnifyContext`) purely so *reads*
(`findVarBinding()`'s parent walk) still see every binding already made so
far — an already-bound variable used inside `Arg` (e.g. `retract(fact(a,
$y))` after `$y` was bound by an earlier goal in the very same clause body)
resolves correctly — but every *write* the trial performs
(`bindVarBinding()`/`bindVarInstance()`, `src/vault-unify-unifycontext.cpp`,
always mutate the `UnifyContext` they are called ON, never a parent's own
maps) lands only in `ucScratch` itself, which is destroyed the moment the
trial ends. So a variable in `Arg` that is still unbound going in stays
unbound after `retract` runs, win or lose — nothing escapes.

**Tombstone, not erase**: a matched clause is never removed from
`m_listClauses` (`Clause::retire()`/`isRetired()`,
`include/vault-unify.hpp`) — `ExecutionState::ClauseIterator` (and every
copy of one — `UnifyContext::m_itClause`, `SolveContext::
m_itNextChildClause`) holds a plain `std::list<Clause*>::const_iterator`;
`std::list::erase()` only guarantees iterators OTHER than the one pointing
at the erased element stay valid, and this engine's own architecture
routinely has several such iterator copies live at once across a
multi-branch search (or a nested `findall`/`retract`-inside-a-rule
invocation) — any one of which could, in principle, be positioned on
exactly the node a concurrent `retract` wants to remove. Erasing that node
out from under a still-live copy would be a dangling-iterator bug; tombstoning
the `Clause` in place — the node stays a completely ordinary, valid
`m_listClauses` member — sidesteps the question entirely: nothing is ever
invalidated, because nothing is ever removed. `ClauseIterator::isValid()`
skips a retired clause when walking forward (checked freshly on every call,
not cached), so it is simply never offered as a candidate again from the
moment it is tombstoned. This also means a tombstoned clause needs **no**
separate "retired list" for its own memory the way
`World::m_lsRetiredDebugInfos` (`src/vault-unify-world.cpp`) needs one for a
*replaced* `TermDebugInfo*` (which is actually removed from the map that
would otherwise reach it again): staying an ordinary `m_listClauses` member
means the existing whole-database sweep
(`ExecutionState::collectAllTermTrees()`/`~ExecutionState()`) already frees
its term tree and the `Clause` object itself exactly once, tombstoned or
not.

**Logical update view**: an iteration already under way when a clause is
retracted stops offering that clause **from the moment it is tombstoned
onward**, even mid-scan — `ClauseIterator::isValid()`'s freshness (previous
paragraph) means it is checked live, not snapshotted at the iteration's
start. This is a documented, deliberate simplification rather than strict
ISO-Prolog logical-update-view semantics (a call is traditionally guaranteed
to see the exact clause set as it stood at call time, unaffected by any
assert/retract during its own execution) — real Prolog implementations vary
on this point too. A clause **appended** mid-scan (by contrast) IS seen by
an iterator that has not yet advanced past it: `std::list::push_back()`
never invalidates a previously captured `end()` iterator, and the new node
becomes part of the reachable range before that sentinel — ordinary
`std::list` behavior, not special-cased.

**Conformance**: `test/conformance/assert-retract.ufy` — (a) `assert()`
adds a fact absent beforehand; a separate, later query sees it; (b) the
state-machine pattern this feature exists for — a rule that reads the
current value, `retract()`s it, and `assert()`s the new one, with the old
value flowing back to the caller; (c) `retract()` removes only the FIRST of
several structurally-identical matching facts, confirmed via `findall`
still finding the remaining one; (d) `retract()` with no matching clause at
all fails silently, no output. Deliberately NOT exercised there (documented
above instead, to keep every query in that program at UnifyNot-or-success):
asserting a non-ground fact, and asserting/retracting a rule — both a
`UnifyError`.
