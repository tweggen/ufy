#if !defined( _VAULT_CLAUSE_STANDARD_HPP )
#define _VAULT_CLAUSE_STANDARD_HPP

namespace vault {
namespace unify {

class StandardClause
        : public Clause
{
public:
    StandardClause( ConsTerm* pLeftHandTerm, Goal* pRightHandGoal )
                : Clause( pLeftHandTerm )
                , m_pRightHandGoal( pRightHandGoal ) {}

    /**
     * ROADMAP Phase 1 (Ownership model), pass 2: only the Goal *object*
     * (the m_listAbstractTerms wrapper) is deleted here; it is exclusively
     * owned by this StandardClause (a fresh `new Goal` per clause, never
     * shared -- see PrologParser::Context::createClause()). The body's
     * TERM trees (what the Goal's list points at) are NOT deleted here:
     * they can alias the clause's own head (a repeated variable) and/or
     * another clause's tree (`if` desugaring, see the note on
     * Clause::~Clause()), so they are freed once, de-duplicated, by
     * World::~World() before any Clause is destroyed.
     */
    virtual ~StandardClause() {
        delete m_pRightHandGoal;
        m_pRightHandGoal = NULL;
    }
    
    virtual vault::unify::Clause::UnificationState startUnification( 
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const;

    const Goal* rightHandGoal() const { return m_pRightHandGoal; }
    virtual bool isTerminal() const;

    virtual std::string toString() const;

private:
    Goal* m_pRightHandGoal;
};

};
};

#endif

