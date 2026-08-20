/**
 * @file unify-run.cpp
 *
 * @author Timo Weggen
 *
 * Minimal CLI runner for .ufy programs.
 *
 * Usage: unify-run <program.ufy>
 *
 * This exists for ROADMAP.md Phase 0 ("Reproducible baseline"): the
 * upcoming golden-output test harness needs something that runs a .ufy
 * file and lets query output be diffed against an expected file. There is
 * no other main() anywhere in this module (the real entry point is
 * combine/applications/stuart-app); this is a standalone one built just for
 * that purpose.
 *
 * It drives the same public API the REST server frontend uses
 * (vault-unify-rest-server.cpp): vault::unify::RuntimeContext::setupDone()
 * to stand up a World + Engine + one worker thread, then
 * parseExecuteSegment() to parse and run the file. `print`/`emit` builtins
 * already write their output straight to stdout as a side effect of
 * unification (see vault-unify-clause-builtin-{print,emit}.cpp), so simply
 * running the program to completion is enough to produce output a golden
 * test can compare.
 */

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <vault-unify.hpp>

/*
 * Private header (lives in src/, not include/) for vault::unify::SolveJob.
 * It is reachable here the same way the module's own .cpp files reach it,
 * because vault-unify-core exposes src/ as a (documented, legacy-Jamfile-
 * matching) public include path -- see CMakeLists.txt. We need it only for
 * the "barrier job" trick explained below.
 */
#include <vault-unify-solvejob.hpp>


namespace {


/*
 * Written only on the engine's single worker thread (from onQueryFinished);
 * read by main() after the barrier wait, which synchronizes with that
 * thread via the barrier callback's mutex -- no extra locking needed.
 */
int s_runtimeErrorCount = 0;


/**
 * Called once per top-level query ("goal ?") job when it finishes.
 * print/emit builtins already wrote their output to stdout while the job
 * ran; here we only collect internal unification errors the job recorded
 * (SolveJob::getErrorCount()) so they can drive the exit code.
 */
void onQueryFinished( boost::shared_ptr<vault::unify::Job> spJob )
{
    vault::unify::SolveJob* pSolveJob =
        dynamic_cast<vault::unify::SolveJob*>( spJob.get() );
    if( pSolveJob ) {
        s_runtimeErrorCount += pSolveJob->getErrorCount();
    }
}


} // anonymous namespace


int main( int argc, char** argv )
{
    if( argc < 2 ) {
        fprintf( stderr, "usage: %s <program.ufy>\n", argc ? argv[0] : "unify-run" );
        return 2;
    }

    std::ifstream in( argv[1], std::ios::binary );
    if( !in ) {
        fprintf( stderr, "unify-run: cannot open '%s'.\n", argv[1] );
        return 2;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string content = buf.str();

    vault::unify::RuntimeContext rt;
    rt.setupDone();

    /*
     * Parses the whole file; for every clause definition it finds, it
     * appends the clause to the root execution state, and for every
     * top-level query it creates and enqueues a SolveJob (see
     * vault-unify-runtime-context.cpp). Job *creation* happens synchronously
     * on this thread; the actual solving happens asynchronously on the
     * single worker thread setupDone() started above.
     *
     * The return value is the number of parse errors encountered (each one
     * already reported to stderr as a gcc-style "<file>:<line>:<column>:
     * parse error" diagnostic by parseExecuteSegment() itself); it drives
     * this tool's exit code below.
     */
    const int parseErrorCount = rt.parseExecuteSegment(
        content.begin(), content.end(),
        onQueryFinished, NULL );

    /*
     * Wait until every query job created above has actually finished
     * running. There is no "wait until idle" call on Engine (ROADMAP.md
     * notes there is no engine shutdown path yet either), so instead we
     * exploit two properties of the current scheduler:
     *
     *  - Engine::executionLoop() drains m_lsReadyJobs strictly FIFO from a
     *    single worker thread (there is exactly one; setupDone() adds only
     *    one).
     *  - Under the default (non-debugging) REGULAR target state, a SolveJob
     *    runs its performSlice() to completion in a single call -- it never
     *    re-queues itself as READY (see SolveJob::performSlice() in
     *    vault-unify-solvejob.cpp; only debugger-halt states do that).
     *
     * So one more, trivial SolveJob -- an empty Goal, which "solves"
     * immediately -- queued *after* parseExecuteSegment() returns is
     * guaranteed to run only once every job queued above it has already run
     * to completion. We wait for that barrier job instead of the real ones.
     */
    boost::mutex waitMutex;
    boost::condition_variable waitCond;
    bool barrierDone = false;

    vault::unify::SolveJob* pBarrier = new vault::unify::SolveJob();
    pBarrier->setWorld( rt.getWorld() );
    pBarrier->setGoal( new vault::unify::Goal() );
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

    int result = 0;
    if( parseErrorCount > 0 ) {
        fprintf( stderr, "unify-run: %d parse error(s) while reading '%s'.\n",
            parseErrorCount, argv[1] );
        result = 1;
    }
    if( s_runtimeErrorCount > 0 ) {
        fprintf( stderr, "unify-run: %d internal unification error(s) while running '%s'.\n",
            s_runtimeErrorCount, argv[1] );
        result = 1;
    }

    return result;
}
