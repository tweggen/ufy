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
 * This clause unifies if the first argument is a map and contains
 * a field as named by the second argument. The term referenced is unified
 * with the third argument.
 */
vault::unify::Clause::UnificationState MemberBuiltinClause::startUnification( 
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const
{
    VAULT_UNIFY_DI( UNIFY, "Called.\n" );

    // Preset.
    out_pGoal = NULL;

    /*
     * look, wether we have a consterm with exactly three arguments
     * to unify with our lhs.
     */
    const ConsTerm* pGoalTerm =
        dynamic_cast<const ConsTerm*>(
            pUCStackTop->m_csTermToUnify.getAbstractTerm() );
    if( !pGoalTerm ) {
        // Wrong syntax.
        VAULT_UNIFY_DI( ALWAYS, "pGoalTerm==NULL.\n" );
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }
    int arity = pGoalTerm->getArity();

    if( pGoalTerm->getName() != leftHandTerm()->getName() ) {
        // Not me?
        // VAULT_UNIFY_DI( ALWAYS, "%s != %s.\n",
        //     pGoalTerm->getName().value().c_str(), leftHandTerm()->getName().value().c_str() );
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    } else {
        // me?
        // VAULT_UNIFY_DI( ALWAYS, "%s matches %s.\n",
        //    pGoalTerm->getName().value().c_str(), leftHandTerm()->getName().value().c_str() );
    }

    if( 3 != arity ) {
        VAULT_UNIFY_DI( ALWAYS, "3 != arity.\n" );
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }

    // VAULT_UNIFY_DI( ALWAYS, "Applying member.\n" );

    /*
     * Remember the arguments and trigger unification.
     */

    /*
     * The left term must be a MapTerm or a VarTerm bound to a MapTerm.
     */
    const AbstractTerm* pTermLeft = pGoalTerm->getTermAt( 0 );
    const AbstractTerm* pTermLeftBound = NULL;
    UnifyContext* pUCLeft = NULL;

    (void) pTermLeft->getBoundTerm(
        pUCStackTop,
        pUCOriginal,
        // pTermLeft,
        pTermLeftBound,
        pUCLeft );
    if( !pTermLeft ) {
        VAULT_UNIFY_DI( ALWAYS, "lhs not bound.\n" );
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;        
    }

    const MapTerm* pMapLeft = dynamic_cast<const MapTerm*>( pTermLeftBound );
    if( NULL==pMapLeft ) {
        VAULT_UNIFY_DI( ALWAYS, "Left hand side is not a map.\n" );
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }

    /*
     * The right term shall be a cons term. We do currently not allow the 
     * right term to be a unifiable variable.
     */
    const AbstractTerm* pTermRight = pGoalTerm->getTermAt( 1 );
    const ConsTerm* pConsRight = dynamic_cast<const ConsTerm*>( pTermRight );
    if( NULL==pConsRight ) {
        VAULT_UNIFY_DI( ALWAYS, "Right hand side is not an ConsTerm.\n" );
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;        
    }
    if( 0 != pConsRight->getArity() ) {
        VAULT_UNIFY_DI( ALWAYS, "Right hand side ConsTerm is not an atom..\n" );
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;                
    }

    /*
     * The output term can be anything, we will try to unify the dereferenced
     * term with the output term.
     */
    const AbstractTerm* pTermResult = pGoalTerm->getTermAt( 2 );
    // Therefore, no cast is required.

    /*
     * Look, wether the map contains the value.
     */
    std::string key = pConsRight->getName().value();

    const AbstractTerm* pTermValue = pMapLeft->getValue( key );
    if( !pTermValue ) {
        // VAULT_UNIFY_DI( ALWAYS, "Key \"%s\" not found in map.\n", key.c_str() );
        // Key not found.
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;                
    } else {
        VAULT_UNIFY_DI( ALWAYS, "Key \"%s\" has value \"%s\".\n", key.c_str(), pTermValue->toString().c_str() );
    }

    VAULT_UNIFY_DI( UNIFY, "Calling unify term with ucStackTop==%lld, ucOriginal==%lld, ucLeft==%lld\n", 
        pUCStackTop?pUCStackTop->getUnifyContextId():0ll,
        pUCOriginal?pUCOriginal->getUnifyContextId():0ll,
        pUCLeft?pUCLeft->getUnifyContextId():0ll );
    VAULT_UNIFY_DI( UNIFY, "member called with uc original = %s, uc cand = %s.\n", 
        pUCOriginal?pUCOriginal->toString().c_str():"(nil)",
        pUCLeft?pUCLeft->toString().c_str():"(nil)" );


    UnifyResult unifyResult = pTermValue->unifyTerm(
        pEngine,
        pUCStackTop,
        pUCOriginal,
        pUCLeft,
        pTermResult );

    if( unifyResult>=0 ) {
        pUCStackTop->unificationDone( unifyResult, NULL );
    } else {
        // Terminated with error.
        pUCStackTop->unificationDone( UnifyNot, NULL );
    }

    return UnificationOK;
}


MemberBuiltinClause::~MemberBuiltinClause()
{
}

/**
 * Create the description of this built clause.
 */
MemberBuiltinClause::MemberBuiltinClause()
        : SimpleBuiltinClause( 
             new vault::unify::ConsTerm( "__builtin_member_deref",
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm() ) )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}

};
};

