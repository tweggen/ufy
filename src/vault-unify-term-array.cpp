/**
 * @file vault-unify-term-array.cpp
 *
 * @author Timo Weggen
 *
 * ROADMAP Phase 2 ("consistent list/array semantics", SPEC.md section 11):
 * ArrayTerm, a first-class ordered term kind for array literals (`[a, b]`),
 * replacing the previous MapTerm-with-stringified-numeric-keys desugaring.
 * Mirrors ConsTerm's/MapTerm's double-dispatch and toString()/toJSON()/
 * toContextString() style throughout -- see include/vault-unify.hpp for the
 * class declaration.
 */

#include <string>

#include <boost/shared_ptr.hpp>

#include <list>

#include <vault-unification.hpp>

namespace vault {
namespace unify {


UnifyResult ArrayTerm::unifyTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const AbstractTerm* pOther ) const
{
    // One object always unifies with itself.
    if( this == pOther ) return UnifyLast;

    // Dispatch second step.
    return pOther->unifyArrayTerm(
        pEngine,
        pUCStackTop,
        pUCMine,
        pUCOther,
        this );
}


UnifyResult ArrayTerm::unifyConsTerm(
        Engine* /*pEngine*/,
        UnifyContext* /*pUCStackTop*/,
        UnifyContext* /*pUCOther*/,
        UnifyContext* /*pUCMine*/,
        const ConsTerm* /*pOther*/ ) const
{
    // An array term does not unify with a cons term.
    return UnifyNot;
}


UnifyResult ArrayTerm::unifyMapTerm(
        Engine* /*pEngine*/,
        UnifyContext* /*pUCStackTop*/,
        UnifyContext* /*pUCOther*/,
        UnifyContext* /*pUCMine*/,
        const MapTerm* /*pOther*/ ) const
{
    // An array term does not unify with a map term (ArrayTerm/MapTerm are
    // deliberately unrelated kinds now -- SPEC.md section 11).
    return UnifyNot;
}


UnifyResult ArrayTerm::unifyVarTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const VarTerm* pOther ) const
{
    // Sort parameters and forward to generic version.
    return pUCStackTop->genericUnifyVarWithKnown(
        pEngine,
        pUCStackTop,
        pUCOther,       // Unify to scope the variable
        pOther,         // Variable in scope
        pUCMine,        // Unify context of myself
        this            // non-var term
        );
}


/**
 * I am asked to unify with another array. Equal length required, then
 * positional (index-by-index) element unification -- mirrors
 * ConsTerm::unifyConsTerm's subterm loop (including the UnifyError-
 * propagation check), but there is no name to compare first (an array has
 * none).
 */
UnifyResult ArrayTerm::unifyArrayTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const ArrayTerm* pOther ) const
{
    VAULT_UNIFY_DI( UNIFY, "ArrayTerm: Asked to unify %s with %s.\n",
        this->toString().c_str(), pOther->toString().c_str() );

    // 1. If terms are identical, unifies trivially.
    if( this == pOther ) {
        return UnifyLast;
    }

    size_t l = m_vecElements.size();
    // 2. If terms have different length, they do not unify.
    if( pOther->m_vecElements.size() != l ) {
        VAULT_UNIFY_DI( UNIFY, "ArrayTerm: Length differs.\n" );
        return UnifyNot;
    }

    for( size_t i=0; i<l; ++i ) {
        const AbstractTerm* pTermMe = m_vecElements[i];
        const AbstractTerm* pTermOther = pOther->m_vecElements[i];

        // Try to unify the terms. Possible variable binding will be emitted
        // to the unification context.
        int res = pUCStackTop->unifyTerms( pEngine, pTermMe, pUCOther, pUCMine, pTermOther );
        // Note: res==UnifyError(-1) is truthy, so it must be checked
        // explicitly here first -- otherwise a sub-term unification error
        // would be silently treated as if the sub-term had unified.
        if( UnifyError==(UnifyResult)res ) {
            VAULT_UNIFY_DI( UNIFY, "ArrayTerm: Subterm errored during unification.\n" );
            return UnifyError;
        }
        if( !res ) {
            VAULT_UNIFY_DI( UNIFY, "ArrayTerm: Subterm doesnt unify.\n" );
            return UnifyNot;
        }
    }

    // Unifies.
    return UnifyLast;
}


const std::string ArrayTerm::toString() const
{
    std::string str = "[";
    size_t l = m_vecElements.size();
    for( size_t i=0; i<l; ++i ) {
        if( i ) str += ", ";
        str += m_vecElements[i]->toString();
    }
    str += "]";
    return str;
}


const std::string ArrayTerm::toJSON( bool useContent, const UnifyContext* pUCStackTop, const UnifyContext* pUCTerm ) const
{
    std::string str = "[";
    size_t l = m_vecElements.size();
    for( size_t i=0; i<l; ++i ) {
        if( i ) str += ", ";
        str += m_vecElements[i]->toJSON( useContent, pUCStackTop, pUCTerm );
    }
    str += "]";
    return str;
}


const std::string ArrayTerm::toContextString(
        const UnifyContext* pUCStackTop,
        const UnifyContext* pUCTerm ) const
{
    std::string str = "[";
    size_t l = m_vecElements.size();
    for( size_t i=0; i<l; ++i ) {
        if( i ) str += ", ";
        str += m_vecElements[i]->toContextString( pUCStackTop, pUCTerm );
    }
    str += "]";
    return str;
}


};
};

