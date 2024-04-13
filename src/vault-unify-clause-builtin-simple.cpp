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


const vault::unify::ConsTerm* SimpleBuiltinClause::getConsTermArg( 
    UnifyContext* pUCStackTop, UnifyContext* pUCOriginal,
    const ConsTerm* pGoalTerm,
    int idx
    )
{
    if( idx >= pGoalTerm->getArity() ) {
        return NULL; // behind end of array
    }
    const vault::unify::AbstractTerm* pTermLeft = pGoalTerm->getTermAt( idx );
    const vault::unify::VarTerm* pVarLeft = dynamic_cast<const vault::unify::VarTerm*>( pTermLeft );
    const vault::unify::AbstractTerm* pTermLeftBound = NULL;
    const vault::unify::ConsTerm* pConsTermLeft = NULL;
    // Note, that we do not need the map's unify context.
    vault::unify::UnifyContext* pUCLeft = NULL;

    // Is it a variable? Then read it's bound value.
    // If it is unbound, we do not unify.
    if( pVarLeft ) {
        // Yes, this is a variable, and it is bound.
        (void) pVarLeft->getBoundTerm(
            pUCStackTop,
            pUCOriginal,
            pTermLeftBound,
            pUCLeft );

        if( !pTermLeftBound ) {
            // Not supported.
            return NULL;
        }
    } else {
        pTermLeftBound = pTermLeft;
    }

    // Get a constant.
    pConsTermLeft = dynamic_cast<const vault::unify::ConsTerm*>( pTermLeftBound );
    if( !pConsTermLeft ) {
        return NULL;
    }   
    return pConsTermLeft;
}

bool SimpleBuiltinClause::isTerminal() const
{
    return false;
}


std::string SimpleBuiltinClause::toString() const
{
    return leftHandTerm()->toString() + ".";
}

SimpleBuiltinClause::SimpleBuiltinClause( ConsTerm* pLeftHandTerm )
        : Clause( pLeftHandTerm )
{
}


};
};


