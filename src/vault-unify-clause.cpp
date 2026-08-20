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
 * a clause's head term can alias into ANOTHER clause's tree (e.g. the
 * VarTerm shared between an `if( cond ) { ... }` auxiliary clause's head
 * and the call-site term left in the clause/query that triggered it -- see
 * AnyTermFactory::operator()(IfStatementInput) in vault-unify-parser.cpp);
 * freeing it again here, per-Clause-instance, would double-free it.
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


