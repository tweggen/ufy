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
vault::unify::Clause::UnificationState OutputBuiltinClause::startUnification( 
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const
{
    // We don't unify to a particular goal.
    out_pGoal = NULL;

    VAULT_UNIFY_DI( UNIFY, "print called with uc original = %s, uc cand = %s.\n", 
        pUCOriginal?pUCOriginal->toString().c_str():"(nil)",
        pUCCand?pUCCand->toString().c_str():"(nil)" );
    
    /*
     * look, wether we have a consterm with exactly two arguments
     * to unify with our lhs.
     */
    const ConsTerm* pGoalTerm =
        dynamic_cast<const ConsTerm*>(
            pUCStackTop->m_csTermToUnify.getAbstractTerm() );
    if( !pGoalTerm ) {
        // No match in name?
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }
    // const ConsTerm* pClauseTerm = leftHandTerm();

    // The goal resulting from the unification.
    const Goal* pGoal = NULL;
    
    int arity = pGoalTerm->getArity();
    if( pGoalTerm->getName() != leftHandTerm()->getName() ) {
        // No match in name?
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }


    /* 
     * Ask the consterm to unify itself, passing the context to the original
     * context of the goal.
     */
#if 0
    int unifyResult = pGoalTerm->unifyTerm(
        pEngine,
        pUCStackTop,
        pUCOriginal, 
        pUCOriginal, // changed 20150127 pUCCand,
        pClauseTerm );
#else
    UnifyResult unifyResult = UnifyLast;
    pGoal = NULL;
#endif

    std::string strResult;

    if( UnifyLast==unifyResult ) {
        // No right hand side for print.
        pGoal = NULL;

        // If we could unify, print it.
        for( int i=0; i<arity; ++i ) {
            if( i ) strResult += ",";
            //strResult += pGoalTerm->getTermAt( i )->toContextString( 
            //    pUCStackTop, pUCOriginal );
            strResult += convertTerm(
                pGoalTerm->getTermAt( i ), pUCStackTop, pUCOriginal );
             // pGoalTerm->getTermAt( i )->toJSON( true, pUCStackTop, pUCOriginal );
        }
    
    } else {
        pGoal = NULL;
    }

    /*
     * Trigger actual output.
     */
    output( pEngine, strResult );

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

    return UnificationOK;
}


OutputBuiltinClause::~OutputBuiltinClause()
{
}

/**
 * Create the description of this built clause.
 */
OutputBuiltinClause::OutputBuiltinClause( const char* name )
        : SimpleBuiltinClause( 
             new vault::unify::ConsTerm( name,
                 new vault::unify::VarTerm() ) )
{
}


};
};


