#if !defined( _VAULT_CLAUSE_BUILTIN_HPP )
#define _VAULT_CLAUSE_BUILTIN_HPP

namespace vault {
namespace unify {

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


};
};

#endif

