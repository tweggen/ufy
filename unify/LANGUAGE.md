# The Unify Language — a tutorial

Unify (`.ufy`) is the rule-engine language embedded in vault's home-automation
system. This document teaches you to *write* Unify programs. It assumes you
can program, but assumes **no** prior exposure to Prolog or logic
programming — every concept from that world is introduced from scratch.

For the precise, implementation-cited semantics behind everything here, see
`SPEC.md` in this directory; this tutorial never contradicts it.

Every example below is either lifted (verbatim or as a small excerpt) from
`test/conformance/*.ufy` or `mediaplayer.ufy` — with output taken from the
matching `test/golden/*.expected` file — or composed trivially from
constructs those files already prove. Lifted examples say which file they
come from.

---

## 1. What is Unify?

Most languages you know are *imperative*: you write steps, and variables are
boxes you assign new values into. Unify is a **logic programming** language:
you write **facts** (things that are true) and **rules** (things that are
true *if* something else is), then ask **questions** — queries — and the
engine searches for every way to answer them.

A fact like `color( red );` states that `color(red)` holds. A rule like
`warm( $x ) { color( $x ); temp( $x, hot ); }` states that `warm($x)` holds
for any `$x` for which *both* `color($x)` and `temp($x, hot)` hold. Instead
of assignment, Unify uses **unification**: `=`, and every argument match in
a call, tries to make two structures *equal* by binding whichever variables
are still unbound — a two-way pattern match, not a one-way store. And
instead of stopping at one answer, the engine does a **depth-first
backtracking search**: given `color( $x )` against three facts, it finds all
three, running whatever follows once per match.

Unify is dressed in C/Java clothing — `{ }` blocks, `;` terminators,
`if`/`for`/`foreach` — so it reads comfortably to programmers who have never
seen Prolog. But under that syntax it *thinks* in Prolog's terms: facts,
rules, unification, exhaustive backtracking search. That combination suits
home-automation rules well: "for every zone that matches this pattern, do
this"; "toggle this sensor and remember what it was" read naturally as
facts/rules/queries over a small live database that `assert`/`retract` can
update as events happen.

A complete, runnable program (from `test/conformance/clause-order.ufy`):

```unify
color( red );
color( green );
color( blue );

query {
    color( $x );
    print( $x );
}
```

```
print: red
print: green
print: blue
```

Three facts are defined; the query asks "for every `$x` such that
`color($x)` holds, print `$x`." The engine finds all three matches, in the
order the facts were written, and runs `print($x)` once per match — there is
no loop here; the *search itself* visits every color.

---

## 2. Getting started

Programs run via the `unify-run` command-line tool, built as part of this
module's CMake build (target `unify-run`, see `tools/unify-run.cpp`):

```
unify-run program.ufy
```

`unify-run` parses the whole file, runs every `query { ... }` block it
finds, and lets `print`/`emit` write output to stdout as a side effect of
the search — one invocation parses and runs one file top to bottom.

Run with **no file**, it starts an interactive REPL instead, which is the
quickest way to try anything in this tutorial:

```console
$ unify-run
Unify REPL. :help for help, :quit to leave.
ufy> color( red );
ufy> ? color( $x );
$x = red
-- 1 solution
```

Everything typed at the prompt is ordinary `.ufy` source and accumulates for
the session, exactly as if it were being appended to one file. The REPL adds
just two things of its own: `? <goals>` as shorthand for
`query { <goals> }`, and a report of each query's variable bindings (see
`README.md` for the full prompt reference). Neither is part of the language,
and neither works in a `.ufy` file. `unify-run -i program.ufy` runs a file
first and then hands you the prompt with everything it defined loaded.

**Output format**: every reached `print(...)` writes exactly one line:

```
print: <value>
```

with variables resolved to their current binding (or shown by name if still
unbound). This exact `print: ` prefix is what every golden test diffs
against, and what every example in this document shows.

**Exit codes**:

- **0** — the file parsed cleanly and no internal runtime error occurred.
  An *ordinary* failed goal (a comparison that doesn't hold, a fact that
  doesn't match) is not an error and does not affect the exit code.
- **1** — a parse error, or at least one internal runtime error
  (`UnifyError` — division by zero, an unbound variable used in
  arithmetic, asserting a non-ground fact, ...). Diagnostics go to stderr.
- **2** — a usage error (wrong arguments, more than one program, file can't
  be opened) — checked before any parsing begins. Note that *no* arguments
  is not a usage error: it starts the REPL.

A parse error is reported gcc-style to stderr:

```
<file>:<line>:<column>: parse error
<the offending source line>
<caret pointing at the column>
```

`test/parse-error.ufy` is a real example: two valid clauses, then a third
whose body is missing its closing `}`. Its golden stdout is empty (nothing
in the file is a query), and the test harness separately requires
`unify-run` to exit `1` for it — confirming the parse failure drives the
exit code independent of stdout.

---

## 3. Facts, rules, and queries

**A fact** is a bare term (name plus zero or more arguments) plus `;`:

```unify
color( red );
fruit( apple );
```

**A rule** adds a `{ ... }` body: a fact-shaped head that holds whenever
every goal in its body holds:

```unify
is_shape( $x ) { shape( $x ); }
```

**A query** is a `query { ... }` block: a brace-delimited sequence of goals,
run immediately, at the point it appears:

```unify
query {
    color( $x );
    print( $x );
}
```

A rule body and a query body are the same kind of thing — a flat list of
`;`-terminated statements — so anything valid in one is valid in the other.

**Variables** start with `$`: `$x`, `$state`. Every occurrence of `$x`
*within one clause or query* is the same variable; its scope ends at that
clause/query's closing `}`. There is no dedicated "don't-care" wildcard like
Prolog's `_` — if you need a placeholder, the only option is an ordinary
named variable, conventionally spelled `$_something` to signal "I don't need
this." Because it's an ordinary variable, using `$_x` twice in one clause
still means "the same value both times" — a naming convention, not two
independent unused slots.

**Comments**: `// line comment` and `/* block comment */` (no nesting).

**Semicolon discipline**: a fact ends with `;`. A rule's `{ ... }` body and a
`query { ... }` block need no trailing `;` after their closing brace.
*Inside* a body, every plain goal ends with `;`, except the block-shaped
statements (`if`, `for`, `foreach`), which end with their own `}` and no
extra `;`.

**Zero-argument heads/calls are barewords, not empty parentheses.** An
argument list, if written at all, requires at least one element —
`foo()` does not parse. A zero-arity predicate has no parentheses at all:
`after { ... }` as a definition, `after;` as a call (from
`test/conformance/cut.ufy`). `after()` would be a parse error.

**Definition order matters, including relative to queries.** Clauses of one
predicate are tried in exactly the order written (`clause-order.ufy` above
never prints any order but `red, green, blue`). The whole file is processed
event-by-event, top to bottom: a clause is visible the instant it's parsed,
and a query runs *immediately* where it appears — so a query only ever sees
facts/rules written *above* it (plus anything an earlier `import`, chapter
11, pulled in). A query written before the facts it needs simply finds
nothing and prints nothing.

---

## 4. Unification

Forget "assignment." In Unify, `=` and every argument match in a call are
the *same* operation: **unification** — making two terms structurally
identical by binding whichever variables are still unbound. Matching the
query's `$x` against `color(red)` binds `$x` to `red` (a new binding, since
`$x` started empty). Matching `1 = 2` binds nothing, because both sides are
already fixed and disagree — the goal just fails, quietly, and the search
moves on.

This is the point of the three-fact example from chapter 1:

```unify
color( red );
color( green );
color( blue );

query {
    color( $x );
    print( $x );
}
```

```
print: red
print: green
print: blue
```

One `$x`, but **three separate bindings**: the engine tries the first fact,
binds `$x` to `red`, runs `print($x)`, then backtracks and tries the next
candidate, binding the same `$x` fresh to `green`, and so on. One query,
three separate runs of `print`.

A failing goal doesn't raise an error — it prunes that one search path
silently. `test/conformance/unification-basics.ufy`:

```unify
pair( a, b );

query {
    pair( a, b, c );
    print( "should-not-print-arity-mismatch" );
}
```

`pair(a, b, c)` (3 arguments) can never unify with the fact `pair(a, b)` (2
arguments) — different arity never unifies, regardless of values — so this
query produces **no output at all**, not an error. This silent-failure
discipline is pervasive: a missing map key (chapter 5), a false comparison
(chapter 6), an unmatched `retract` (chapter 9) — none of these raise
anything; they simply don't succeed.

---

## 5. Data

Unify has four kinds of term: plain atoms/calls, maps, arrays, and
variables. There is no separate number or string kind.

**Atoms, numbers, and strings are the same thing, by spelling.** A bareword
`red`, a digit run `205`, and a quoted string `"205"` all become the
identical kind of term — differing only in spelling, not in what they are.
From `test/conformance/strings-numbers.ufy`:

```unify
code( "205" );

query {
    code( $c );
    print( $c );
}

query {
    205 = "205";
    print( "number-equals-quoted-string" );
}
```

```
print: 205
print: number-equals-quoted-string
```

The fact was stored with a quoted `"205"`; the query matched it with a bare
`205`, and `205 = "205"` unifies even though one side looks like a number
and the other a quoted string — same spelling, same atom. Numbers are
unsigned digit runs only in source (no literal negatives, no floats); an
evaluated arithmetic result can still come out negative (chapter 6).

**Maps** are `{ key: value, ... }` literals; `lhs -> key` reads a value back
out. From `test/conformance/deref.ufy`:

```unify
place( { name: "kitchen", level: 1 } );

query {
    place( $p );
    print( $p->name );
}

query {
    place( $p );
    print( $p->missing );
}
```

```
print: kitchen
```

The second query asks for a key, `missing`, that isn't in the map — `->` on
a missing key **fails silently**, so that query prints nothing. Two maps
unify only if they share exactly the same set of keys and every value
unifies too — any extra or missing key fails the whole comparison, no
partial match (`test/conformance/map-unify.ufy`):

```unify
query {
    { x: 1, y: 2 } = { x: 1, y: 2 };
    print( "maps-equal" );
}

query {
    { x: 1, y: 2 } = { x: 1 };
    print( "should-not-print-size-mismatch" );
}
```

```
print: maps-equal
```

**Arrays** are `[a, b, c]` literals with element-by-element unification —
lengths must match exactly, or the whole comparison fails
(`test/conformance/findall-arrays.ufy`):

```unify
data( [ a, b, c ] );

query {
    [ 1, 2, 3 ] = [ 1, 2, 3 ];
    print( "arrays-equal" );
}

query {
    data( $arr );
    print( $arr );
}
```

```
print: arrays-equal
print: [a, b, c]
```

There's no working `arr[i]` indexing syntax (see chapter 12's limitations) —
arrays are consumed with `foreach`/`for` (chapter 7) or built with `findall`
(chapter 8).

**Ranges** (`a..b`, inclusive) write a whole array of consecutive integers.
With both bounds literal numbers, a range expands at parse time into
exactly that array — `1..4` *is* `[1, 2, 3, 4]`. From
`test/conformance/loops.ufy`:

```unify
query {
    foreach ( $x : 1..4 ) { print( $x ); }
    print( "after-foreach-range" );
}
```

```
print: 1
print: 2
print: 3
print: 4
print: after-foreach-range
```

---

## 6. Expressions

**Arithmetic** only happens on a side of `=` that contains a `+ - * /`
operator — otherwise Unify has no arithmetic-evaluation syntax at all.
`*`/`/` bind tighter than `+`/`-`, both left-associative; there is no
parenthesized grouping, so the precedence table (chapter 12) is the only
way to control order. Numbers are 64-bit integers; `/` truncates toward
zero; dividing by zero is a runtime error (silent on stdout, but it makes
`unify-run` exit 1). From `test/conformance/arithmetic.ufy`:

```unify
query {
    $y = 2 + 3 * 4;
    print( $y );
}

query {
    $y = 3 - 10;
    print( $y );
}
```

```
print: 14
print: -7
```

**Comparisons** (`< <= > >= == !=`) are plain **goals**: you write them
bare, as a statement, and they succeed or fail like any other goal. If both
sides resolve to integers, the comparison is numeric; otherwise both sides'
text is compared lexicographically. From `test/conformance/comparison.ufy`:

```unify
query {
    5 < 10;
    print( "five-less-than-ten" );
}

query {
    "abc" < "abd";
    print( "string-comparison" );
}

query {
    2 + 3 == 5;
    print( "arithmetic-in-comparison" );
}
```

```
print: five-less-than-ten
print: string-comparison
print: arithmetic-in-comparison
```

**`=` has two jobs, and which one runs depends on whether arithmetic is
present.** Ordinarily `=` is structural unification (chapter 4). The moment
either side contains `+ - * /`, `=` instead *evaluates* that side to a
number and unifies the *result* against the other side. Contrast, side by
side:

```unify
// Plain unification (test/conformance/infix-unify.ufy):
to_1( $a ) { $a = 1; }

query {
    to_1( $v );
    print( $v );
}
```

```
print: 1
```

```unify
// Arithmetic evaluation (test/conformance/arithmetic.ufy):
query {
    $y = 20 / 4 + 1;
    print( $y );
}
```

```
print: 6
```

The first `=` never evaluates anything — it binds `$a` (hence `$v`) to the
atom `1`. The second sees a `+`/`/`, evaluates `20 / 4 + 1` to `6`, and
unifies `$y` against *that*. Two ordinary atoms on both sides (no
arithmetic operator anywhere) are always plain unification, no matter what
they look like.

---

## 7. Control flow

**`if (cond) { body }`** runs `body` once if `cond`'s *first* solution
succeeds, and otherwise runs nothing — a committed if-then-else that never
retries `cond`. Either way execution continues after the `if`. From
`test/conformance/if-statement.ufy`:

```unify
flag( on );

query {
    print( "before-then-query" );
    if( flag( on ) ) { print( "then-branch" ); }
    print( "after-then-query" );
}
```

```
print: before-then-query
print: then-branch
print: after-then-query
```

(A query with `if( flag( off ) )` instead simply skips the body and prints
only the before/after lines — `test/conformance/if-statement.ufy`'s second
query, golden-verified the same way.)

**`cut;`** is a goal statement (not an operator) that commits the *current*
clause/query's choice: once reached, no further alternative clause is tried
for whichever call selected this clause, and nothing to its LEFT in the
same body backtracks for another solution. Goals AFTER the cut are
unaffected. From `test/conformance/cut.ufy`:

```unify
c( 1 );
c( 2 );
c( 3 );

first( $x ) { c( $x ); cut; }

query {
    first( $v );
    print( $v );
}
```

```
print: 1
```

Without `cut;` this would print `1`, `2`, and `3` — exactly what happens
when nothing follows the cut, from the same file:

```unify
after {
    cut;
    c( $z );
    print( $z );
}

query { after; }
```

```
print: 1
print: 2
print: 3
```

The cut commits `after`'s own clause choice, but `c($z)` is written *after*
it, so all three of its solutions still run untouched.

**Prefix `!` is negation-as-failure**, unrelated to `cut` — it applies to
one goal. `!goal` succeeds exactly when `goal` does not hold. From
`test/conformance/negation.ufy`:

```unify
fruit( apple );

query {
    !fruit( apple );
    print( "should-not-print-fact-holds" );
}

query {
    !fruit( banana );
    print( "negation-succeeds-arg-mismatch" );
}

query {
    !nosuchpredicate( x );
    print( "should-not-print-undefined-predicate" );
}
```

```
print: negation-succeeds-arg-mismatch
```

The first fails (`fruit(apple)` genuinely holds); the second succeeds
(a `fruit/1` fact exists, but none matches `banana`). **The third is the
gotcha**: negating a call to a predicate defined *nowhere* also **fails** —
the opposite of textbook negation-as-failure ("nothing proves it, so its
negation holds"). In Unify, `!` only succeeds against a name it can find at
least one same-arity clause for, whose arguments then fail to match; an
entirely undefined name gives it nothing to work with. If you plan to
negate a call, make sure a clause of that name/arity exists.

**`for ( init; cond; step ) { body }`** is a classic counting loop where **a
failing `cond`, `body`, or `step` stops the loop outright** — the honest
C-flavored reading of "the loop body failed." From
`test/conformance/loops.ufy`:

```unify
query {
    for ( $i = 0; $i < 4; $i = $i + 1 ) {
        $i != 2;
        print( $i );
    }
    print( "after-for-body-failure" );
}
```

```
print: 0
print: 1
print: after-for-body-failure
```

(Without the `$i != 2` guard, the same loop prints `0 1 2 3` — see the
"count" query in `loops.ufy`.) The guard fails once `$i` reaches `2`, ending
the loop right there — `2` and `3` never print, and the loop still succeeds
once overall.

**`foreach ( $x : arrExpr ) { body }`** iterates an array (or range) element
by element, binding `$x` fresh each time. Unlike `for`, **a failing `body`
for one element does NOT stop the loop** — that iteration is silently
skipped and the loop continues:

```unify
query {
    foreach ( $x : [a, b, c] ) {
        $x != b;
        print( $x );
    }
    print( "after-foreach-body-failure" );
}
```

```
print: a
print: c
print: after-foreach-body-failure
```

`b` fails the guard and is silently skipped — `a` and `c` still print. That
is the one real behavioral difference from `for`'s stop-on-failure example
above.

---

## 8. Working with solutions

**`findall($tmpl, $goal)`**, always written as the right side of `=`, runs
`$goal` to *every* solution and collects one copy of `$tmpl` per solution
into an array, in solution order. It never fails: zero solutions produce an
empty array. From `test/conformance/findall-arrays.ufy`:

```unify
fruit( apple );
fruit( banana );
fruit( cherry );

query {
    $all = findall( $x, fruit( $x ) );
    print( $all );
}

query {
    $none = findall( $x, nosuchpredicate( $x ) );
    print( $none );
}
```

```
print: [apple, banana, cherry]
print: []
```

**Limitation**: `$goal` solves as a brand-new, independent search — bindings
made *before* the `findall(...)` call are not carried into it. A
hypothetical `$y = 1; $all = findall($x, related($x, $y));` would solve
`related($x, $y)` with `$y` **unbound** inside the nested search. Keep what
you `findall` over self-contained.

**Recursion** works, including threading an *invariant* parameter unchanged
through every recursive call. `cut` is what makes a recursive predicate's
output deterministic: without it, once the recursion bottoms out and
unwinds, the engine's exhaustive backtracking would retry every level's
always-succeeding base-case clause again, printing things twice. From
`test/conformance/recursion.ufy`:

```unify
count_down( $limit, $i ) {
    $i < $limit;
    print( $i );
    cut;
    $j = $i + 1;
    count_down( $limit, $j );
}
count_down( $limit, $i );

query {
    count_down( 4, 1 );
    print( "after-count-down" );
}
```

```
print: 1
print: 2
print: 3
print: after-count-down
```

`$limit` (`4`) threads unchanged through every call while `$i` counts up;
the second, bare-fact clause is the base case, reached once
`$i < $limit` finally fails. Each step commits via its own `cut`, so
`"after-count-down"` prints exactly once, not once per recursion level.

---

## 9. State

`assert(Fact);` and `retract(Fact);` mutate the running fact database.
`assert` adds a brand-new fact at the *end* of the database — the fact must
be fully ground (every variable bound); asserting a rule, or a fact with an
unbound variable, is a runtime error. `retract` removes the *first* fact (in
definition order) whose head unifies with its argument; no match is a
silent, ordinary failure.

The idiom this exists for: read the current value, retract it, assert the
new one. From `test/conformance/assert-retract.ufy`:

```unify
query {
    assert( sensor( kitchen, off ) );
}

query {
    sensor( kitchen, $state );
    print( $state );
}

toggle_sensor( $dev, $old, $new ) {
    sensor( $dev, $old );
    retract( sensor( $dev, $old ) );
    assert( sensor( $dev, $new ) );
}

query {
    toggle_sensor( kitchen, $old, on );
    print( $old );
}

query {
    sensor( kitchen, $state );
    print( $state );
}
```

```
print: off
print: off
print: on
```

The first `off` is the freshly-asserted state, read by a *separate*, later
query — proving `assert`'s effect is visible afterward. The second `off` is
`toggle_sensor` reporting the value it just replaced. The final `on` is the
new state, confirming the toggle took effect.

**The logical update view**, in plain terms: a single query — or a single
pass through a rule body — sees the database exactly as it stood the moment
*that* search started, no matter how many `assert`/`retract` calls happen
while it's still running, even ones it triggers itself. In `toggle_sensor`
above, the outer scan for `sensor($dev, $old)` isn't confused by its own
`retract`+`assert` of that fact: it already fixed its view of the database
when it started, so it correctly stops after the one value it began with,
rather than looping forever rediscovering its own freshly asserted
replacement.

---

## 10. Strings

`concat` and `strlen`, like `findall`, are recognized **only** as the right
side of `=` — written anywhere else they are *not* rewritten, and behave
like an ordinary (never-matching) predicate call. From
`test/conformance/strings.ufy`:

```unify
query {
    $s = concat( "foo", "bar", "baz" );
    print( $s );
}

query {
    $n = strlen( "hello" );
    print( $n );
}
```

```
print: foobarbaz
print: 5
```

`strlen` counts **bytes**, not Unicode codepoints — fine for ASCII text
like `"hello"`, but a multi-byte character counts as more than one.

**Gotcha**: because the rewrite only fires on a side of `=`, writing
`concat(...)` directly as a `print` argument does *not* concatenate
anything — `print` always succeeds and simply prints whatever term it's
handed, and an un-rewritten `concat(...)` is just an ordinary, unevaluated
2-argument term. `print( concat( "foo", "bar" ) );` prints the raw
structure `concat(foo,bar)`, not `foobar`. Bind the result with `=` first,
then print the variable — exactly as every example above does.

`contains`, `startswith`, and `endswith` need no `=` — they're ordinary
goals you call directly:

```unify
query {
    if( contains( "hello world", "wor" ) ) { print( "contains-yes" ); }
}

query {
    if( startswith( "hello world", "hello" ) ) { print( "startswith-yes" ); }
}

query {
    if( endswith( "hello world", "world" ) ) { print( "endswith-yes" ); }
}
```

```
print: contains-yes
print: startswith-yes
print: endswith-yes
```

---

## 11. Programs at scale

**`import "relative/path.ufy";`** splits a program across files. The path
resolves relative to the *importing file's own directory* (not the
process's working directory). Each file imports **at most once** per run —
a repeated `import` (even via a different-looking path resolving to the
same file) is a silent no-op. An imported file's clauses and queries run
**inline**, at the point of the `import` statement, not deferred to the end.

From `test/conformance/imports.ufy` and its library
`test/conformance/imports-lib.ufy`:

```unify
// imports-lib.ufy
shape( circle );
shape( square );
is_shape( $x ) { shape( $x ); }

query {
    is_shape( $x );
    print( $x );
}
```

```unify
// imports.ufy
import "imports-lib.ufy";

query {
    is_shape( $x );
    print( $x );
}

import "imports-lib.ufy";   // silent no-op, already imported
color( red );

query {
    color( $x );
    print( $x );
}
```

```
print: circle
print: square
print: circle
print: square
print: red
```

The first `circle`/`square` pair is the library's own query, running inline
the moment the first `import` is processed. The second pair is
`imports.ufy`'s own query, reusing the imported `is_shape/1`. There is no
third pair — the second `import` is a no-op. The final `red` proves the
importer's own definitions, written after the import, still work.

**Structuring a real ruleset**: `mediaplayer.ufy` in this directory is the
flagship worked example, using nearly everything above together: facts
carrying map metadata, `findall`+`foreach` for an inventory listing, `if`
for a decision, `for` for a bounded volume ramp, and `assert`/`retract` for
a playback state machine. One excerpt, the inventory listing:

```unify
zone( kitchen, { name: "Kitchen", volume: 30 } );
zone( living, { name: "Living Room", volume: 45 } );
zone( bedroom, { name: "Bedroom", volume: 20 } );

query {
    print( "zones:" );
    $zoneIds = findall( $zref, zone( $zref, $zm ) );
    foreach ( $zid : $zoneIds ) {
        zone( $zid, $meta );
        $name = $meta->name;
        $zoneLabel = concat( "  - ", $name, " (", $zid, ")" );
        print( $zoneLabel );
    }
}
```

Its slice of `test/golden/mediaplayer.expected`:

```
print: zones:
print:   - Kitchen (kitchen)
print:   - Living Room (living)
print:   - Bedroom (bedroom)
```

`findall` collects every zone id first (a fixed list to walk), `foreach`
visits each, `->` reads the display name back out of the zone's metadata
map, and `concat` assembles the printed line. Run `unify-run mediaplayer.ufy`
and compare against the full `test/golden/mediaplayer.expected` (24 lines,
covering the boot banner, loud-zone warning, volume ramp, event processing,
and final state report) to see a complete, larger program end to end.

---

## 12. Reference appendix

### Reserved words and shapes

| Form | Reservation kind | What it means |
|------|-------------------|----------------|
| `query { ... }` | positional (top-level only) | a top-level query block; a zero-arg clause named `query` can't be defined, but `query(x) { ... }` still can |
| `cut;` | exact name + arity 0 | commits: no more clause alternatives, no backtracking left of it |
| `for ( ; ; ) { }` | positional keyword | counting loop; body/cond/step failure stops it |
| `foreach ( $x : arr ) { }` | positional keyword | element/range iteration; a body failure skips just that element |
| `import "path";` | exact shape | textual inclusion, relative to the importing file, once per file |
| `assert( Fact );` | exact name + arity 1 | add a ground fact at the end of the database |
| `retract( Fact );` | exact name + arity 1 | remove the first matching fact |
| `$x = findall( $t, $g )` | exact shape (on a side of `=`) | collect every solution of `$g` into an array |
| `$x = concat( a, b, ... )` | exact shape (on a side of `=`) | string concatenation |
| `$x = strlen( s )` | exact shape (on a side of `=`) | byte length of a string |
| `contains`/`startswith`/`endswith` | soft (clause order only) | plain builtin goals; a same-named user clause is tried after the builtin, not blocked |

`if` is reserved positionally, like `for`/`foreach`: a clause **head** named
`if(...)` is unaffected, since a head is never parsed through this
statement grammar.

### Operator precedence (tightest binding first)

| Level | Operators | Associativity |
|-------|-----------|----------------|
| 1 | `*` `/` | left |
| 2 | binary `+` `-` | left |
| 3 | `..` (range) | — |
| 4 | `==` `!=` `<=` `>=` `<` `>` | none (single, non-chaining) |
| 5 | `=` (unify, or evaluate-then-unify if arithmetic) | — |

No parenthesized grouping exists anywhere — this table is the only way
precedence is controlled. Prefix `!` (negation) applies to a whole goal
statement, parsed separately from this expression grammar. Prefix `-` does
**not** negate a number (see limitations below) — avoid it.

### Builtins

Registered before any user clause: `unify` (what `=` desugars to for plain
unification), `print`, `emit` (like `print`, but JSON-formatted and also
forwarded to any registered event listener — used by vault's REST
frontend), member-deref (`->`), arithmetic-eval and comparison (what
arithmetic `=` and `< <= > >= == !=` desugar to), array-at and range (what
`foreach`/ranges desugar to), `concat`, `strlen`, `contains`, `startswith`,
`endswith`. `print`/`emit` are variadic in practice — they comma-join
however many arguments you give, despite a declared arity of one.

### Error and diagnostic behavior

- A parse error prints `<file>:<line>:<column>: parse error` plus the
  source line and a caret, to stderr; `unify-run` exits 1 once parsing
  finishes.
- An internal runtime error (division by zero; an unbound variable in
  arithmetic/comparison; asserting a non-ground fact or a rule; a dynamic
  range over 100000 elements) fails that one goal silently on stdout, but
  is tallied and also makes `unify-run` exit 1.
- An *ordinary* failed goal (a false comparison, a non-matching fact, a
  `retract` with nothing to remove) is not an error — no diagnostic, no
  effect on the exit code.
- Exit 2 means a usage problem, checked before parsing begins.

### Current limitations (and workarounds)

- **`assert` is facts-only and ground-only.** Asserting a rule, or a fact
  with any unbound variable, is a runtime error. Bind every value first.
- **`retract` is check-and-remove, not a binding goal**, and only removes
  the *first* matching fact. Capture the value you need (as `$old` in
  `toggle_sensor`) before retracting.
- **`findall`'s sub-goal runs in a fresh scope**; outer bindings made
  before the call are invisible inside it. Keep the sub-goal self-contained.
- **`concat`/`strlen` inside an arithmetic expression are not supported** —
  `$x = concat(a, b) + 1;` fails at runtime. Compute the concat/strlen into
  its own variable with a separate `=` first, then use it arithmetically.
- **No floating-point numbers** — only 64-bit integers. Use scaled integers
  if you need fractions.
- **`strlen` counts UTF-8 bytes, not codepoints** — reliable for ASCII only.
- **No array indexing syntax** — `arr[i]` parses but never succeeds. Use
  `foreach`/`for` (chapter 7) or `findall` (chapter 8) instead.
- **Prefix `-` does not negate a number** — an unfinished, unused operator.
  Use `0 - x` instead of `-x`.
- **Negating an undefined predicate fails, not succeeds** (chapter 7) — the
  opposite of textbook negation-as-failure. Make sure a clause of the
  negated name/arity exists before relying on `!` over it.
