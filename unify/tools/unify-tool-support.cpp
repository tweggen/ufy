/**
 * @file unify-tool-support.cpp
 *
 * @author Timo Weggen
 *
 * See unify-tool-support.hpp for what lives here and why.
 */

#include <fstream>
#include <sstream>
#include <string>

#include <vault-unify.hpp>

/*
 * Private engine header (lives in src/, not include/); reachable here the
 * same way the module's own .cpp files reach it, because vault-unify-core
 * exposes src/ as a (documented, legacy-Jamfile-matching) public include
 * path -- see CMakeLists.txt. Needed only for the barrier-job trick
 * explained in waitForEngineIdle()'s header comment.
 */
#include <vault-unify-solvejob.hpp>

#include "unify-tool-support.hpp"


namespace unifytool {


void waitForEngineIdle( vault::unify::RuntimeContext& rt )
{
    boost::mutex waitMutex;
    boost::condition_variable waitCond;
    bool barrierDone = false;

    vault::unify::SolveJob* pBarrier = new vault::unify::SolveJob();
    pBarrier->setWorld( rt.getWorld() );
    {
        vault::unify::Goal* pBarrierGoal = new vault::unify::Goal();
        pBarrier->setGoal( pBarrierGoal );
        // Adopt the empty barrier Goal so it is freed along with pBarrier
        // itself (see ROADMAP Phase 1: SolveJob arena) instead of leaking.
        pBarrier->adoptGoal( pBarrierGoal );
    }
    pBarrier->onFinished(
        [&waitMutex, &waitCond, &barrierDone]( boost::shared_ptr<vault::unify::Job> ) {
            vault::unify::Guard g( waitMutex );
            barrierDone = true;
            waitCond.notify_one();
        } );
    pBarrier->startJob( rt.getEngine() );
    rt.getEngine()->addJob( boost::shared_ptr<vault::unify::Job>( pBarrier ) );

    {
        vault::unify::Guard g( waitMutex );
        while( !barrierDone ) {
            waitCond.wait( g );
        }
    }
}


bool readWholeFile( const char* pPath, std::string& out_content )
{
    std::ifstream in( pPath, std::ios::binary );
    if( !in ) {
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    out_content = buf.str();
    return true;
}


} // namespace unifytool
