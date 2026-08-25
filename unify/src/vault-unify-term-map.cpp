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


namespace vault {
namespace unify {


UnifyResult MapTerm::unifyTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const AbstractTerm* pOther ) const
{
    // One object always unifies with itself. See the scope-blindness
    // analysis above ConsTerm::unifyConsTerm's identical check
    // (vault-unify-term-cons.cpp) -- it applies here unchanged: a MapTerm
    // is always freshly allocated per source occurrence (AnyTermFactory,
    // vault-unify-parser.cpp), only a repeated VARIABLE name is ever shared
    // across scopes (SPEC.md section 10), so this pointer-identity shortcut
    // never hides a cross-scope aliasing bug the way VarTerm's own analogous
    // check once did.
    if( this == pOther ) return UnifyLast;

    // Dispatch second step.
    return pOther->unifyMapTerm(
        pEngine,
        pUCStackTop,
        pUCMine,
        pUCOther,
        this );
}


UnifyResult MapTerm::unifyConsTerm(
        Engine* /*pEngine*/,
        UnifyContext* /*pUCStackTop*/,
        UnifyContext* /*pUCOther*/,
        UnifyContext* /*pUCMine*/,
        const ConsTerm* /*pOther*/ ) const
{
    // A map term does not unify with a cons term.
    return UnifyNot;
}


UnifyResult MapTerm::unifyVarTerm(
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
 * I am asked to unify with another map. So give it a try.
 */
UnifyResult MapTerm::unifyMapTerm(
        Engine* pEngine,    
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const MapTerm* pOther ) const
{
    VAULT_UNIFY_DI( UNIFY, "MapTerm: Asked to unify %s with %s.\n", 
        this->toString().c_str(), pOther->toString().c_str() );

    // MapTerm
    // 1. If terms are identical, unifies to rhs of clause. See the
    // scope-blindness analysis above MapTerm::unifyTerm's own identical
    // check just above -- applies here unchanged.
    if( this == pOther ) {
        // Generate copy in goal.
        // Unify.
        return UnifyLast;
    }

    size_t l = m_mapContents.size();
    // 2. If terms have different aryty, they do not unify.
    if( pOther->m_mapContents.size() != l ) {
        VAULT_UNIFY_DI( UNIFY, "MapTerm: Size differs.\n" );
        // Does not unify.
        return UnifyNot;
    }

    // Empty maps are identical by definition.
    if( 0==l ) {
        return UnifyLast;
    }
    
    MapTermMap::const_iterator 
        itMe = m_mapContents.begin(),
        itMeEnd = m_mapContents.end();
    MapTermMap::const_iterator
        itOther = pOther->m_mapContents.begin();

    for( ; itMe != itMeEnd; ++itMe, ++itOther ) {
        int res;

        const MapTermValue& mePair = itMe->second;
        const MapTermValue& otherPair = itOther->second;

        const Atom* const& pAtomMe = mePair.first;
        const Atom* const& pAtomOther = otherPair.first;

        if( pAtomMe->value() != pAtomOther->value() ) {
            VAULT_UNIFY_DI( UNIFY, "MapTerm: Keys differ.\n" );
            return UnifyNot;
        }

        const AbstractTerm* const& pTermMe = mePair.second;
        const AbstractTerm* const& pTermOther = otherPair.second;

        // Try to unify the terms. Possible variable binding will be emitted
        // to the unification context.
        // Old API: res = pTermMe->unifyTerm( pEngine, pUCStackTop, pUCOther, pUCMine, pTermOther );
        res = pUCStackTop->unifyTerms( pEngine, pTermMe, pUCOther, pUCMine, pTermOther );
        // If unifies and dynamic, continue unification.
        // If does not unify and dynamic, continue unification as far as possible.
        // If unifies and static, continue unification.
        // If does not unify and static, abort unification.
        // Note: res==UnifyError(-1) is truthy, so it must be checked
        // explicitly here first -- otherwise a sub-term unification error
        // would be silently treated as if the sub-term had unified.
        if( UnifyError==(UnifyResult)res ) {
            VAULT_UNIFY_DI( UNIFY, "MapTerm: Subterm errored during unification.\n" );
            return UnifyError;
        }
        if( !res ) {
            VAULT_UNIFY_DI( UNIFY, "MapTerm: Subterm doesnt unify.\n" );
            return UnifyNot;
        }
    }

    // Unifies.
    return UnifyLast;
}


UnifyResult MapTerm::unifyArrayTerm(
        Engine* /*pEngine*/,
        UnifyContext* /*pUCStackTop*/,
        UnifyContext* /*pUCOther*/,
        UnifyContext* /*pUCMine*/,
        const ArrayTerm* /*pOther*/ ) const
{
    /*
     * These are terms of different types (ROADMAP Phase 2: ArrayTerm is a
     * distinct kind now, unrelated to MapTerm -- SPEC.md section 11).
     * Cannot unify at all.
     */
    return UnifyNot;
}


const AbstractTerm* MapTerm::getValue( const std::string& key ) const
{
    MapTermMap::const_iterator it = m_mapContents.find( key );
    if( m_mapContents.end() == it ) return NULL;
    MapTermValue val = it->second; 
    const AbstractTerm* pTerm = val.second;
    return pTerm;
}
    
const std::string MapTerm::toString() const
{
    std::string str;

    str += "{";
    MapTermMap::const_iterator 
        itMe = m_mapContents.begin(),
        itMeEnd = m_mapContents.end();

    bool isFirst = true;

    for( ; itMe != itMeEnd; ++itMe ) {
        if( !isFirst ) {
            str += ", ";
        } else {
            isFirst = false;
        }

        const MapTermValue& mePair = itMe->second;
        const Atom* const& pAtomMe = mePair.first;
        const AbstractTerm* const& pTermMe = mePair.second;

        str += pAtomMe->value();
        str += ": ";
        str += pTermMe->toString();
    }
    str += "}";

    return str;
}


const std::string MapTerm::toJSON( bool useContent, const UnifyContext* pUCStackTop, const UnifyContext* pUCTerm ) const
{
    std::string str;

    str += "{";
    MapTermMap::const_iterator 
        itMe = m_mapContents.begin(),
        itMeEnd = m_mapContents.end();

    bool isFirst = true;

    for( ; itMe != itMeEnd; ++itMe ) {
        if( !isFirst ) {
            str += ", ";
        } else {
            isFirst = false;
        }

        const MapTermValue& mePair = itMe->second;
        const Atom* const& pAtomMe = mePair.first;
        const AbstractTerm* const& pTermMe = mePair.second;

        str += "\"";
        str += pAtomMe->value();
        str += "\"";
        str += ": ";
        str += pTermMe->toJSON( useContent, pUCStackTop, pUCTerm );
    }
    str += "}";

    return str;
}


const std::string MapTerm::toContextString(
        const UnifyContext* pUCStackTop,
        const UnifyContext* pUCTerm ) const
{
    std::string str;

    str += "{";
    MapTermMap::const_iterator 
        itMe = m_mapContents.begin(),
        itMeEnd = m_mapContents.end();

    bool isFirst = true;

    for( ; itMe != itMeEnd; ++itMe ) {
        if( !isFirst ) {
            str += ", ";
        } else {
            isFirst = false;
        }

        const MapTermValue& mePair = itMe->second;
        const Atom* const& pAtomMe = mePair.first;
        const AbstractTerm* const& pTermMe = mePair.second;

        str += pAtomMe->value();
        str += ": ";
        str += pTermMe->toContextString( pUCStackTop, pUCTerm );
    }
    str += "}";

    return str;
}



/**
 * ROADMAP Phase 1 (Ownership model), pass 2: the Atom* keys are allocated
 * fresh for this MapTerm alone (AnyTermFactory::operator()(ArrayTermInput)/
 * operator()(MapTermInput), vault-unify-parser.cpp -- each entry gets its
 * own `new Atom`, never shared with any other MapTerm), so freeing them
 * here is safe regardless of when/how this destructor runs.
 *
 * The AbstractTerm* VALUES are intentionally NOT freed here: they are
 * ordinary child terms reachable via abstractTermIterator() (see the
 * TermIterator above), so they are already covered by whatever
 * collectTermTree()/deleteTermTree() pass is deleting this MapTerm itself
 * (see the ownership note in vault-unify.hpp). Deleting them here too would
 * double-free them.
 */
MapTerm::~MapTerm()
{
    MapTermMap::const_iterator it, itEnd = m_mapContents.end();
    for( it = m_mapContents.begin(); it != itEnd; ++it ) {
        delete it->second.first;
    }
}


/** 
 * Construct a map term with the given keys and the given values.
 * The atoms and the terms passed to this constructor will be released
 * by the MapTerm destructor. The caller must not release them.
 */
MapTerm::MapTerm(
    const Atom** keys,
    AbstractTerm** values,
    int nTuples )
{
    {
        const Atom** k = keys;
        AbstractTerm** v = values;
        for( int i=0; i<nTuples; ++i ) {
            m_mapContents[(*k)->value()] = MapTermValue( *k, *v );
            ++k; ++v;
        }
    }
}


};
};


