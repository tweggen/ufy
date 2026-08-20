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

TermTraversable::~TermTraversable() {}


#if VAULT_UNIFY_USE_VIRTUAL_DEREF
const AbstractTerm* AbstractTerm::deref() const
{
    return this;
}
#endif

/**
 * Given an arbitrary term, return a bound term or NULL.
 * If the term is non-var, return it. If it is a varterm, find the
 * instance and return. If no instance is bound, return NULL.
 *
 * TXWTODO: Implement a polymorphic helper function in XxxtTerm?
 */
int AbstractTerm::getBoundTerm(
    const UnifyContext* pUCStackTop,
    const UnifyContext* pUCInput,
    // const AbstractTerm* pInputTerm,
    const AbstractTerm*& out_pTerm,
    UnifyContext*& out_pUCOutput ) const
{
    const AbstractTerm* pInputTerm = this;
    const VarTerm* pVarTerm = dynamic_cast<const VarTerm*>( pInputTerm );
    if( !pVarTerm ) {
        out_pUCOutput = const_cast<UnifyContext*>( pUCInput );
        out_pTerm = pInputTerm;
        return 0;
    }
    // Dereference VarTerm.
    UnifyContextId uidOrg;
    if( pUCInput ) {
        uidOrg = pUCInput->getUnifyContextId();
    } else {
        uidOrg = 0;
    }
    AssignmentId aid( uidOrg, pVarTerm->getBinding() );

    boost::shared_ptr<SingleVarInstance> spInstance;
    InstanceId iid = 0;
    (void) pUCStackTop->findVarBinding( aid, iid );
    if( iid ) {
        (void) pUCStackTop->findVarInstance( iid, spInstance );
    }
    if( spInstance ) {
        VAULT_UNIFY_DI( UNIFY, "VT%lld: Found binding to instance.\n", 
            (long long) pVarTerm->getBinding() );
        const AbstractTerm* pInstanceTerm = spInstance->getTerm();
        UnifyContext* pUCInstanceTerm = spInstance->getUnifyContext();
        out_pTerm = pInstanceTerm;
        out_pUCOutput = pUCInstanceTerm;
   } else {
        out_pTerm = NULL;
        out_pUCOutput = NULL;
   }
   return 0;
}



AbstractTermIterator::~AbstractTermIterator() {}

const TermTraversable* Goal::GoalIterator::getTermTraversable() const {
    return dynamic_cast<const TermTraversable*>( *m_it );
}


/**
 * ROADMAP Phase 1 (Ownership model), pass 2. See the ownership note above
 * the declarations of collectTermTree()/deleteTermTree() in vault-unify.hpp.
 *
 * Walks pTerm's children generically via the existing TermTraversable /
 * AbstractTermIterator machinery (ConsTerm's sub-terms, MapTerm's values;
 * VarTerm is a leaf and returns a NULL iterator), rather than reaching into
 * ConsTerm/MapTerm internals directly.
 */
void collectTermTree( const AbstractTerm* pTerm, std::set<const AbstractTerm*>& out_visited )
{
    if( !pTerm ) {
        return;
    }
    if( !out_visited.insert( pTerm ).second ) {
        // Already visited (an aliased/shared sub-term) -- do not recurse
        // into it again.
        return;
    }

    AbstractTermIterator* pIt = pTerm->abstractTermIterator();
    if( pIt ) {
        while( pIt->isValid() ) {
            const TermTraversable* pChildTraversable = pIt->getTermTraversable();
            if( pChildTraversable ) {
                const AbstractTerm* pChildTerm = dynamic_cast<const AbstractTerm*>( pChildTraversable );
                if( pChildTerm ) {
                    collectTermTree( pChildTerm, out_visited );
                }
            }
            pIt->next();
        }
        delete pIt;
    }
}


void deleteTermTree( const AbstractTerm* pTerm )
{
    if( !pTerm ) {
        return;
    }
    std::set<const AbstractTerm*> visited;
    collectTermTree( pTerm, visited );
    std::set<const AbstractTerm*>::const_iterator it, itEnd = visited.end();
    for( it = visited.begin(); it != itEnd; ++it ) {
        delete *it;
    }
}


/**
 * See the ownership note above this declaration in include/vault-unify.hpp.
 *
 * Dispatches on pTerm's dynamic type (VarTerm/ConsTerm/MapTerm are the only
 * concrete AbstractTerm kinds this module ever builds via the parser) and
 * recurses into children. The transient `AbstractTerm**`/`const Atom**`
 * arrays handed to the ConsTerm/MapTerm constructors are not retained by
 * either constructor (they copy the pointer VALUES out into their own
 * std::vector/std::map), so -- exactly like every other call site in this
 * module that builds a ConsTerm/MapTerm this way (AnyTermFactory in
 * vault-unify-parser.cpp) -- the arrays themselves are not freed here
 * either; only what they point at is (via the returned clone's own
 * ownership).
 */
AbstractTerm* cloneTermTree(
        const AbstractTerm* pTerm,
        const std::map<const VarTerm*, VarTerm*>& varSubstitution )
{
    if( !pTerm ) {
        return NULL;
    }

    if( const VarTerm* pSrcVar = dynamic_cast<const VarTerm*>( pTerm ) ) {
        std::map<const VarTerm*, VarTerm*>::const_iterator itSub =
            varSubstitution.find( pSrcVar );
        if( itSub != varSubstitution.end() ) {
            return itSub->second;
        }
        // Internal error: every VarTerm in the subtree is expected to have
        // been collected into varSubstitution before cloning. Do not
        // crash -- clone a fresh, unmapped VarTerm instead.
        VAULT_UNIFY_DI( ALWAYS,
            "cloneTermTree: VarTerm '%s' missing from substitution map; "
            "cloning as a fresh, unmapped variable.\n",
            pSrcVar->toString().c_str() );
        VarTerm* pFreshVar = new VarTerm();
        pFreshVar->setOriginalVarName( pSrcVar->getOriginalVarName() );
        return pFreshVar;
    }

    if( const ConsTerm* pSrcCons = dynamic_cast<const ConsTerm*>( pTerm ) ) {
        int nTerms = pSrcCons->getArity();
        AbstractTerm** ppTerms = nTerms ? new AbstractTerm*[nTerms] : NULL;
        for( int i = 0; i < nTerms; ++i ) {
            ppTerms[i] = cloneTermTree( pSrcCons->getTermAt( i ), varSubstitution );
        }
        ConsTerm* pCloneCons = new ConsTerm(
            Atom( pSrcCons->getName().value() ), nTerms, ppTerms );
        // The ConsTerm constructor copies the pointer values; the transient
        // array itself stays ours to free (delete[] NULL is a no-op).
        delete[] ppTerms;
        pCloneCons->setNegated( pSrcCons->isNegated() );
        return pCloneCons;
    }

    if( const MapTerm* pSrcMap = dynamic_cast<const MapTerm*>( pTerm ) ) {
        std::vector<std::pair<const Atom*, AbstractTerm*> > entries;
        pSrcMap->getEntries( entries );
        int nTuples = (int) entries.size();
        const Atom** ppAtoms = nTuples ? new const Atom*[nTuples] : NULL;
        AbstractTerm** ppTerms = nTuples ? new AbstractTerm*[nTuples] : NULL;
        for( int i = 0; i < nTuples; ++i ) {
            // MapTerm owns (and ~MapTerm() deletes) whatever Atom* keys it
            // is constructed with, so the clone needs its own copies --
            // it must not share Atom* pointers with pSrcMap.
            ppAtoms[i] = new Atom( entries[i].first->value() );
            ppTerms[i] = cloneTermTree( entries[i].second, varSubstitution );
        }
        MapTerm* pCloneMap = new MapTerm( ppAtoms, ppTerms, nTuples );
        // Same transient-array pattern as the ConsTerm branch above: the
        // constructor copies the pointer values, the arrays stay ours.
        delete[] ppAtoms;
        delete[] ppTerms;
        return pCloneMap;
    }

    // Internal error: no other concrete AbstractTerm kind exists in this
    // module. Log and give up rather than silently dropping the term.
    VAULT_UNIFY_DI( ALWAYS,
        "cloneTermTree: term '%s' is neither VarTerm, ConsTerm nor MapTerm; "
        "cannot clone.\n", pTerm->toString().c_str() );
    return NULL;
}


};
};


