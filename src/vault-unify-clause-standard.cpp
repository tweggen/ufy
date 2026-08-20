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

#include <vault-unify-clause-standard.hpp>


namespace vault {
namespace unify {


bool StandardClause::isTerminal() const
{
     return NULL==m_pRightHandGoal || m_pRightHandGoal->isEmpty(); 
}

std::string StandardClause::toString() const
{
    std::string strClause;
    strClause = leftHandTerm()->toString();
    const Goal* pGoal = rightHandGoal();
    if( !pGoal ) {
    } else {
        strClause += " :- ";
        strClause += pGoal->toString();
    }
    strClause += ".";
    return strClause;
}


/**
 * @param pUCStackTop
 *     This contains an empty uc frame we store bindings into while unifying.
 * @param pUCOriginal 
 *     Contains the unify context of the next goal term we want
 *     to unify.
 * @param pUCCand
 *     Contains the unify context of the clause we want to unify with.
 *     This is NULL (in the end, we are the clause).
 */
vault::unify::Clause::UnificationState StandardClause::startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& /*inout_pCCC*/ ) const
{
    UnifyResult unifyResult;

    // Obtain the next goal term.
    const AbstractTerm* pGoalTerm =
        pUCStackTop->m_csTermToUnify.getAbstractTerm();
    // Obtain the left hand side term of the clause.
    const AbstractTerm* pClauseTerm = leftHandTerm();

    // The goal resulting from the unification.
    const Goal* pGoal = NULL;
    
    /*
     * If the are denoting the same object, they obviously unify.
     */
    if( pClauseTerm==pGoalTerm ) {
        // Duplicate clause right hand term.
        pUCStackTop->unificationDone( UnifyLast, pGoal );
        return UnificationOK;
    }

    /* 
     * Ask the consterm to unify itself, passing the context to the original
     * context of the goal.
     */
    unifyResult = pUCStackTop->unifyTerms(
        pEngine,
        pGoalTerm,
        pUCCand,
        pUCOriginal,
        pClauseTerm );

    if( UnifyLast==unifyResult || UnifyNotLast==unifyResult ) {
        pGoal = rightHandGoal();
    } else {
        pGoal = NULL;
    }
    out_pGoal = pGoal;

    // And store resuilt.
    // As all these goals are synchronous, we can store them immediately.
    pUCStackTop->unificationDone( unifyResult, pGoal );

    if( (int)unifyResult < 0 ) {
        // Error unifying.
        return UnificationError;
    } else {
        return UnificationOK;
    }
}


};
};


