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

ClauseId Clause::m_nextUid;


/**
 * ROADMAP Phase 1 (Ownership model), pass 2: this used to `delete
 * m_pLeftHandTerm` directly. It no longer does: the head term (and its
 * children) is freed as part of the whole-clause-database, de-duplicated
 * pass performed by World::~World()/ExecutionState::collectAllTermTrees()
 * BEFORE any Clause in the database is destroyed. That pass exists because
 * a clause's head term can alias into the SAME clause's own body (a
 * repeated variable used in both) -- freeing it again here, per-Clause-
 * instance, would double-free it. (`if( cond ) { ... }` auxiliary clauses
 * used to alias a VarTerm with a SIBLING clause's head/the call-site term
 * left in the clause/query that triggered them too, which is why this
 * comment used to describe a cross-clause case as well; AnyTermFactory::
 * operator()(IfStatementInput) in vault-unify-parser.cpp now clones
 * cond/body into fresh variables private to each synthesized clause, so
 * that no longer happens -- the whole-database pass remains, since the
 * intra-clause case above still needs it.)
 */
Clause::~Clause()
{
    m_pLeftHandTerm = NULL;
}

Clause::Clause( ConsTerm* pLeftHandTerm )
        : m_pLeftHandTerm( pLeftHandTerm )
        , m_uid( m_nextUid++ )
{
}


};
};


