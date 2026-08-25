/**
 * @file vault-unification.cpp
 *
 * @author Timo Weggen
 *
 * Implementation of unification engine.
 */

#include <string>
 
#include <boost/shared_ptr.hpp>

#include <list>

#include <vault-unification.hpp>

#include <vault-unify-clause-builtin.hpp>

namespace vault {
namespace unify {


/**
 * This clause unifies, if the first argument unifies with the second. 
 */
vault::unify::Clause::UnificationState UnifyBuiltinClause::startUnification( 
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& /*inout_pCCC*/ ) const
{
    // We don't unify to a particular goal.
    out_pGoal = NULL;

    /*
     * look, wether we have a consterm with exactly two arguments
     * to unify with our lhs.
     */
    const ConsTerm* pGoalTerm =
        dynamic_cast<const ConsTerm*>(
            pUCStackTop->m_csTermToUnify.getAbstractTerm() );
    if( !pGoalTerm ) {
        // No goalTerm?
        pUCStackTop->unificationDone( UnifyNot, NULL );        
        return UnificationOK;
    }
    int arity = pGoalTerm->getArity();

    if( 2 != arity || pGoalTerm->getName() != leftHandTerm()->getName() ) {
        // No match in name?
        // No match in name?
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }

    /*
     * Remember the arguments and trigger unification.
     */
    const AbstractTerm* pTermLeft = pGoalTerm->getTermAt( 0 );
    const AbstractTerm* pTermRight = pGoalTerm->getTermAt( 1 );
    
    VAULT_UNIFY_DI( UNIFY, "Calling unify term with ucStackTop==%lld, ucOriginal==%lld, ucCand==%lld\n", 
        pUCStackTop?pUCStackTop->getUnifyContextId():0ll,
        pUCOriginal?pUCOriginal->getUnifyContextId():0ll,
        pUCCand?pUCCand->getUnifyContextId():0ll );
    
    UnifyResult unifyResult = pTermLeft->unifyTerm(
        pEngine,
        pUCStackTop,
        pUCOriginal,
        pUCOriginal, // changed 20150127 pUCCand,
        pTermRight );
    
    // Store the result as-is (Phase 1: propagate UnifyError instead of
    // silently mapping it to "did not unify").
    pUCStackTop->unificationDone( unifyResult, NULL );

    if( (int)unifyResult < 0 ) {
        // Error unifying.
        return UnificationError;
    } else {
        return UnificationOK;
    }
}


// See OutputBuiltinClause::~OutputBuiltinClause() (vault-unify-clause-builtin-output.cpp)
// for why the head term is not freed here.
UnifyBuiltinClause::~UnifyBuiltinClause()
{
}

/**
 * Create the description of this built clause.
 */
UnifyBuiltinClause::UnifyBuiltinClause()
        : SimpleBuiltinClause( 
             new vault::unify::ConsTerm( "unify",
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm() ) )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}


};
};


