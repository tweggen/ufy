/**
 * @file vault-unify-world.cpp
 *
 * @author Timo Weggen
 *
 * Implementation of unification engine.
 */

#include <string>
 
#include <boost/shared_ptr.hpp>
#include <boost/thread.hpp>

#include <list>

#include <vault-unification.hpp>

#include <vault-unify-clause-builtin.hpp>
#include <vault-unify-debug.hpp>

namespace vault {
namespace unify {


void World::setTermDebugInfo( const AbstractTerm* pTerm, TermDebugInfo* pTermDebugInfo )
{
    // Remove former info.
    // TXWTODO: Lock begin
    TermDebugMap::iterator it = m_mapDebugInfos.find( pTerm );
    if( it != m_mapDebugInfos.end() ) {
        TermDebugInfo* pOldTermDebugInfo = it->second;
        delete pOldTermDebugInfo;
    }
    m_mapDebugInfos[pTerm] = pTermDebugInfo;
    // TXWTODO: Lock end.
}


TermDebugInfo* World::getTermDebugInfo( const AbstractTerm* pTerm )
{
    // TermDebugInfo* pTermDebugInfo = NULL;
    // TXWTODO: Guard from here.
    TermDebugMap::iterator it = m_mapDebugInfos.find( pTerm );
    if( it != m_mapDebugInfos.end() ) {
        return it->second;
    } else {
        return NULL;
    }
}


#if 0
int World::appendRootClause( const Clause* pClause, ClauseId& out_uid )
{
    static ClauseId nextClauseId;
    m_rootClauses.push_back( pClause );
    out_uid = ++nextClauseId;
    return 0;
}
#endif


void World::init()
{
    WorldPtr spWorld = shared_from_this();
    m_rootState.appendClause( spWorld, new UnifyBuiltinClause() );
    m_rootState.appendClause( spWorld, new PrintBuiltinClause() );
    m_rootState.appendClause( spWorld, new EmitBuiltinClause() );
    m_rootState.appendClause( spWorld, new MemberBuiltinClause() );
}


World::World()
        : m_rootState( NULL )
{
}


/** 
 * Trigger shutdown of this world. 
 *
 * @param spWorld
 *    A shared poin the to this world.
 *    If the shared pointer leaves scope, the world can be considered to
 *    be deleted.
 */
int World::asyncShutdown( WorldPtr spWorld )
{
    if( this != spWorld.get() ) {
        return -EINVAL;
    }
    /// TODO: Trigger shutdown of world object, interrupting all async
    /// Execution states recursively.
    return 0;
}


};

};


