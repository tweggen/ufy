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

