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


};
};

#endif

