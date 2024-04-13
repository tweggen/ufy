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


};
};

#endif

