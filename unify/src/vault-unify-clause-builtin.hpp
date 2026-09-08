#if !defined( _VAULT_CLAUSE_BUILTIN_HPP )
#define _VAULT_CLAUSE_BUILTIN_HPP

namespace vault {
namespace unify {

/**
 * Parse s as a signed int64 -- the engine's ONE definition of "is this text
 * a number", and the reason it is declared here rather than kept file-local
 * in an anonymous namespace, this module's usual per-file style: it is now
 * read from two directions. The builtins ask it whether an atom may be
 * arithmetic (evaluateArith() below, and the comparison builtin), and
 * engine item E7's term-to-Value walker
 * (src/vault-unify-term-value.hpp) asks it whether a 0-arity atom should
 * leave the engine as `Kind::Int` -- so a second copy would let a front end
 * show as a number something the engine refuses to add, or the reverse.
 * Implemented in vault-unify-clause-builtin-arith.cpp.
 *
 * v1 semantics: no floats; a leading '-' is accepted here even though
 * m_ruleNumber -- src/vault-unify-parser.hpp -- never produces one itself,
 * since an evaluated result CAN be negative, e.g. `3 - 10;`, and its atom
 * text ("-7") must itself be readable back as a number by a later
 * expression. Rejects empty strings, anything with trailing garbage after
 * the digits (strtoll only requires a PREFIX to be numeric; a manual
 * full-string check is required to actually reject something like
 * "12abc"), and out-of-range values. Returns false (and leaves out_value
 * untouched) on any failure.
 *
 * What it does NOT reject, because strtoll does not: leading whitespace, a
 * leading '+', and leading zeros -- so " 7", "+7" and "007" are all the
 * number 7 here. Harmless for arithmetic, where such an atom can only have
 * been written by hand; noted because E7's walker inherits it.
 */
bool parseInt64( const std::string& s, int64_t& out_value );


/**
 * Recursively evaluate an arithmetic expression term (a `__builtin_arith`
 * tree, a VarTerm bound to one/a number, or a plain 0-arity numeric atom)
 * to an int64. Implemented in vault-unify-clause-builtin-arith.cpp (see
 * that file's own comment on evaluateArith() for the full contract);
 * declared here -- rather than kept file-local in an anonymous namespace --
 * specifically so vault-unify-clause-builtin-string.cpp's concat/strlen
 * argument resolution can reuse this SAME evaluator for a `__builtin_arith`
 * argument (e.g. `$s = concat($a, 1 + 2);`) instead of duplicating the
 * recursive tree walk.
 */
bool evaluateArith(
    UnifyContext* pUCStackTop,
    UnifyContext* pUCOriginal,
    const AbstractTerm* pTerm,
    int64_t& out_value,
    std::string& out_error );


/**
 * The equals/assign builtin clause.
 */
class UnifyBuiltinClause
        : public SimpleBuiltinClause
{
public:
    UnifyBuiltinClause();
    virtual ~UnifyBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification( 
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;
    
private:
};


/**
 * The base class for print and emit.
 */
class OutputBuiltinClause
        : public SimpleBuiltinClause
{
public:
    OutputBuiltinClause( const char* );
    virtual ~OutputBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;

protected:
    virtual void output( Engine* pEngine, const std::string& ) const = 0;
    virtual std::string convertTerm( 
        const AbstractTerm* term,
        UnifyContext*,
        UnifyContext* ) const = 0;

private:
};


class EmitBuiltinClause
        : public OutputBuiltinClause
{
public:
    EmitBuiltinClause();
    virtual ~EmitBuiltinClause();

protected:
    virtual void output( Engine* pEngine, const std::string& ) const;
    virtual std::string convertTerm(
        const AbstractTerm* term,
        UnifyContext*,
        UnifyContext* ) const;

};


class PrintBuiltinClause
        : public OutputBuiltinClause
{
public:
    PrintBuiltinClause();
    virtual ~PrintBuiltinClause();

protected:
    virtual void output( Engine* pEngine, const std::string& ) const;
    virtual std::string convertTerm(
        const AbstractTerm* term,
        UnifyContext*,
        UnifyContext* ) const;

};


/**
 * The member builtin clause.
 */
class MemberBuiltinClause
        : public SimpleBuiltinClause
{
public:
    MemberBuiltinClause();
    virtual ~MemberBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;
};


/**
 * ROADMAP Phase 2: "Arithmetic and comparison builtins with a defined
 * evaluation construct" (SPEC.md). What `$y = $x + 1;` desugars to when
 * either side of `=` contains arithmetic operators
 * (AnyTermFactory::operator()(const InfixTermsInput&), case '=',
 * vault-unify-parser.cpp): evaluates the `__builtin_arith(...)` tree given
 * as its first argument (int64 semantics; leading '-' accepted when
 * reading a number; division by zero is a `UnifyError`) and unifies the
 * (possibly negative) result -- a fresh atom, e.g. "-5" -- with its second
 * argument.
 */
class ArithEvalBuiltinClause
        : public SimpleBuiltinClause
{
public:
    ArithEvalBuiltinClause();
    virtual ~ArithEvalBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;
};


/**
 * ROADMAP Phase 2: what a comparison goal (`$x < 5;`, plus `<= > >= == !=`)
 * desugars to (AnyTermFactory::operator()(const CompareTermInput&),
 * vault-unify-parser.cpp). Numeric (int64) comparison when both resolved
 * sides are integer atoms (or evaluate to one via `__builtin_arith`);
 * lexicographic string comparison otherwise. Succeeds (`UnifyLast`) or
 * fails (`UnifyNot`); an unresolved/unbound side is a `UnifyError`.
 */
class CompareBuiltinClause
        : public SimpleBuiltinClause
{
public:
    CompareBuiltinClause();
    virtual ~CompareBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;
};


/**
 * ROADMAP ("for"/"foreach" loops, language owner request 2026-08-21): what
 * `foreach`'s synthesized `__fe__N` clause's own `__builtin_array_at($arr,
 * $idx, $out)` goal resolves (AnyTermFactory::operator()(const
 * ForeachStatementInput&), vault-unify-parser.cpp) -- never written
 * directly by a user program. Requires argument 0 to resolve (via
 * `AbstractTerm::getBoundTerm`) to an `ArrayTerm` and argument 1 to resolve
 * to a 0-arity `ConsTerm` atom whose text parses fully as an int64 (an
 * unbound/non-integer index is a `UnifyError`, mirroring
 * `ArithEvalBuiltinClause`); an out-of-range index (negative, or `>=` the
 * array's length) is an ordinary `UnifyNot` -- this is the loop's own
 * termination condition. On a hit, unifies the element against argument 2.
 * Implemented in vault-unify-clause-builtin-array.cpp.
 */
class ArrayAtBuiltinClause
        : public SimpleBuiltinClause
{
public:
    ArrayAtBuiltinClause();
    virtual ~ArrayAtBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;
};


/**
 * ROADMAP ("for"/"foreach" loops + ranges, language owner request
 * 2026-08-21): what a range with a NON-literal bound (a variable, or itself
 * an arithmetic expression) desugars to (AnyTermFactory::operator()(const
 * RangeTermInput&), vault-unify-parser.cpp) -- a range with BOTH bounds
 * literal is expanded eagerly at parse time instead and never reaches this
 * builtin (SPEC.md). Resolves both bounds via the SAME `evaluateArith()`
 * helper `__builtin_eval`/`__builtin_compare` use for their own operands
 * (vault-unify-clause-builtin-arith.cpp's own anonymous namespace, where
 * this class is also implemented) -- so a range bound may itself be a
 * `__builtin_arith` expression, not just a bare variable or literal. Builds
 * a fresh `ArrayTerm` of one 0-arity `ConsTerm` atom per integer in
 * `[a, b]` (inclusive; empty if `b < a`), capped at
 * `RANGE_BUILTIN_MAX_ELEMENTS` elements (a `UnifyError` beyond, unlike
 * the eager literal-bounds path's silent truncation, since a builtin CAN
 * genuinely fail the goal), adopts it via `UnifyContext::adoptTerm()`
 * (mirroring `ArithEvalBuiltinClause`'s fresh-result ownership), and unifies
 * it against argument 2.
 */
class RangeBuiltinClause
        : public SimpleBuiltinClause
{
public:
    RangeBuiltinClause();
    virtual ~RangeBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;
};


/**
 * ROADMAP Phase 2 ("String operations (concat, compare, match)", SPEC.md):
 * what `$s = concat( $a, $b, ... );` (2+ args on either side of `=`)
 * desugars to (`AnyTermFactory::operator()(const InfixTermsInput&)`, case
 * `'='`, vault-unify-parser.cpp) -- mirroring how `findall(...)` is
 * recognized by exact name there. Because a builtin clause head declares a
 * FIXED arity (see e.g. `ArithEvalBuiltinClause`/`RangeBuiltinClause`
 * above), the parser wraps the variadic source argument list into ONE
 * `ArrayTerm` first, so this builtin's own head is a plain, fixed arity 2:
 * `__builtin_concat([a, b, ...], out)`. Each array element is resolved like
 * an `__builtin_eval`/`__builtin_compare` operand -- following a `VarTerm`
 * binding, evaluating a `__builtin_arith` sub-tree via `evaluateArith()`
 * (declared above; the same evaluator `__builtin_eval` uses) if present,
 * otherwise requiring a 0-arity `ConsTerm` atom -- and the resolved
 * strings are concatenated in order. Implemented in
 * vault-unify-clause-builtin-string.cpp, alongside `StrlenBuiltinClause`
 * and the `contains`/`startswith`/`endswith` goal builtins below.
 */
class ConcatBuiltinClause
        : public SimpleBuiltinClause
{
public:
    ConcatBuiltinClause();
    virtual ~ConcatBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;
};


/**
 * ROADMAP Phase 2 (String operations, SPEC.md): what `$n = strlen( $s );`
 * (exactly 1 arg on a side of `=`) desugars to (case `'='`,
 * vault-unify-parser.cpp) -- same reservation idea as `ConcatBuiltinClause`
 * above, but with a single argument, so no `ArrayTerm` wrapping is needed:
 * `__builtin_strlen(arg, out)`, head declared arity 2. Resolves argument 0
 * exactly like one `ConcatBuiltinClause` array element (see above) and
 * unifies the resolved string's BYTE length (`std::string::size()` -- v1
 * counts UTF-8 bytes, not Unicode codepoints; SPEC.md documents this) as a
 * fresh decimal atom against argument 1.
 */
class StrlenBuiltinClause
        : public SimpleBuiltinClause
{
public:
    StrlenBuiltinClause();
    virtual ~StrlenBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;
};


/**
 * ROADMAP Phase 2 (String operations, SPEC.md): `contains( $s, $sub );` --
 * an ordinary PLAIN GOAL builtin (no `=`/parser rewrite involved at all,
 * unlike concat/strlen above -- the same shape as `unify`/`__builtin_member_deref`,
 * section 9), registered under its own literal, user-facing name `contains`
 * (`World::init`, vault-unify-world.cpp) rather than a `__builtin_`-prefixed
 * internal name. Because builtins are appended to the root `ExecutionState`
 * before any user clause (section 5), a user-defined `contains/2` clause is
 * only a SOFT reservation: `ExecutionState::ClauseIterator` still tries
 * candidate clauses of the same name+arity in definition order, so this
 * builtin's own result (success or failure) is always produced first, and
 * the user's own clause could still be reached on backtracking -- it is not
 * removed the way `cut`/`findall`/`assert`/`retract`'s exact-shape parser
 * reservations are. Resolves both arguments exactly like one
 * `ConcatBuiltinClause` array element (see above); an unbound argument is a
 * `UnifyError`. Succeeds (`UnifyLast`) iff argument 0's resolved string
 * contains argument 1's as a substring (`std::string::find`).
 */
class ContainsBuiltinClause
        : public SimpleBuiltinClause
{
public:
    ContainsBuiltinClause();
    virtual ~ContainsBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;
};


/**
 * ROADMAP Phase 2 (String operations, SPEC.md): `startswith( $s, $prefix );`
 * -- same shape, registration, soft-reservation, and argument resolution as
 * `ContainsBuiltinClause` above; succeeds iff argument 0's resolved string
 * starts with argument 1's.
 */
class StartswithBuiltinClause
        : public SimpleBuiltinClause
{
public:
    StartswithBuiltinClause();
    virtual ~StartswithBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;
};


/**
 * ROADMAP Phase 2 (String operations, SPEC.md): `endswith( $s, $suffix );`
 * -- same shape, registration, soft-reservation, and argument resolution as
 * `ContainsBuiltinClause` above; succeeds iff argument 0's resolved string
 * ends with argument 1's.
 */
class EndswithBuiltinClause
        : public SimpleBuiltinClause
{
public:
    EndswithBuiltinClause();
    virtual ~EndswithBuiltinClause();

    virtual vault::unify::Clause::UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;
};


};
};

#endif

