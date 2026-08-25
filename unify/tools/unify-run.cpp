/**
 * @file unify-run.cpp
 *
 * @author Timo Weggen
 *
 * CLI front end for the Unify engine.
 *
 * Usage: unify-run [-i|--interactive] [program.ufy]
 *
 *   unify-run program.ufy      run the program and exit (batch mode)
 *   unify-run                  start the interactive REPL
 *   unify-run -i program.ufy   run the program, then stay in the REPL with
 *                              everything it defined still loaded
 *
 * Batch mode exists for ROADMAP.md Phase 0 ("Reproducible baseline"): the
 * golden-output test harness needs something that runs a .ufy file and
 * lets query output be diffed against an expected file. Its behaviour is
 * unchanged and must stay that way -- every golden test invokes exactly
 * `unify-run <program.ufy>`.
 *
 * The REPL is ROADMAP.md Phase 4; it lives in unify-repl.cpp.
 *
 * Either way this drives the engine's public API the same way an embedding
 * application would: vault::unify::RuntimeContext::setupDone() to stand up
 * a World + Engine + one worker thread, then parseExecuteSegment() to parse
 * and run source. `print`/`emit` builtins already write their output
 * straight to stdout as a side effect of unification (see
 * vault-unify-clause-builtin-{print,emit}.cpp), so simply running a program
 * to completion is enough to produce output a golden test can compare.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <vault-unify.hpp>

/*
 * Private headers (live in src/, not include/); reachable here the same way
 * the module's own .cpp files reach them, because vault-unify-core exposes
 * src/ as a (documented, legacy-Jamfile-matching) public include path --
 * see CMakeLists.txt.
 *   - vault-unify-solvejob.hpp: SolveJob::getErrorCount(), below.
 *   - vault-unify-debug.hpp: vault-unify.hpp only forward-declares
 *     `class FileDebugInfo;` (it is otherwise a src/-private type) -- the
 *     full definition is needed here to construct one for the program file
 *     (ROADMAP Phase 2 "File imports / include").
 */
#include <vault-unify-solvejob.hpp>
#include <vault-unify-debug.hpp>

#include "unify-repl.hpp"
#include "unify-tool-support.hpp"


namespace {


/*
 * Written only on the engine's single worker thread (from onQueryFinished);
 * read by main() after the barrier wait, which synchronizes with that
 * thread via the barrier callback's mutex -- no extra locking needed.
 */
int s_runtimeErrorCount = 0;


/**
 * Called once per top-level query ("query { ... }") job in batch mode when
 * it finishes. print/emit builtins already wrote their output to stdout
 * while the job ran; here we only collect internal unification errors the
 * job recorded (SolveJob::getErrorCount()) so they can drive the exit code.
 *
 * Note the REPL deliberately does NOT reuse this: at the prompt a finished
 * query also reports its variable bindings (see unify-repl.cpp), which is
 * exactly the extra output a golden test must never see.
 */
void onQueryFinished( boost::shared_ptr<vault::unify::Job> spJob )
{
    vault::unify::SolveJob* pSolveJob =
        dynamic_cast<vault::unify::SolveJob*>( spJob.get() );
    if( pSolveJob ) {
        s_runtimeErrorCount += pSolveJob->getErrorCount();
    }
}


void printUsage( const char* pProgram, FILE* pOut )
{
    fprintf( pOut, "usage: %s [-i|--interactive] [--trace] [program.ufy]\n", pProgram );
    fprintf( pOut, "  program.ufy            run the program and exit\n" );
    fprintf( pOut, "  (no arguments)         start the interactive REPL\n" );
    fprintf( pOut, "  -i, --interactive      after running the program, stay in the REPL\n" );
    fprintf( pOut, "      --trace            keep the engine's stderr trace on in the REPL\n" );
    fprintf( pOut, "  -h, --help             show this text\n" );
}


/**
 * Parse and run one .ufy file into `rt`, and wait until every query it
 * started has finished. Returns the number of parse errors; internal
 * unification errors land in s_runtimeErrorCount via onQueryFinished().
 */
int runProgramFile( vault::unify::RuntimeContext& rt, const char* pPath )
{
    std::string content;
    if( !unifytool::readWholeFile( pPath, content ) ) {
        fprintf( stderr, "unify-run: cannot open '%s'.\n", pPath );
        return -1;
    }

    /*
     * ROADMAP Phase 2 ("File imports / include"): the path as-given (not
     * resolved to an absolute path) seeds relative import resolution --
     * RuntimeContext::parseExecuteSegment()'s import handling resolves a
     * `import "...";` path against this FileDebugInfo's directory
     * (boost::filesystem::path(...).parent_path()), and an as-given relative
     * path already has the right directory component for that (or none, if
     * it is a bare filename, which correctly falls back to resolving
     * against the process CWD). It also makes parse-error diagnostics say
     * the real filename instead of "<input>".
     *
     * Ownership: registered with the World immediately below, which owns it
     * exclusively from that point on -- see World::adoptFileDebugInfo()'s
     * comment (include/vault-unify.hpp) for why this must NOT be deleted
     * here (or anywhere in this function): ~World() (run via rt's
     * destructor, back in main()) frees it exactly once, regardless of
     * whether the program went on to build any term referencing it via
     * TermDebugInfo.
     */
    vault::unify::FileDebugInfo* pMainFileDebugInfo =
        new vault::unify::FileDebugInfo( pPath );
    rt.getWorld()->adoptFileDebugInfo( pMainFileDebugInfo );

    /*
     * Parses the whole file; for every clause definition it finds, it
     * appends the clause to the root execution state, and for every
     * top-level query it creates and enqueues a SolveJob (see
     * vault-unify-runtime-context.cpp). Job *creation* happens synchronously
     * on this thread; the actual solving happens asynchronously on the
     * single worker thread setupDone() started.
     *
     * The return value is the number of parse errors encountered (each one
     * already reported to stderr as a gcc-style "<file>:<line>:<column>:
     * parse error" diagnostic by parseExecuteSegment() itself); it drives
     * this tool's exit code below.
     */
    const int parseErrorCount = rt.parseExecuteSegment(
        content.begin(), content.end(),
        onQueryFinished, pMainFileDebugInfo );

    // The engine has no "wait until idle" call; see waitForEngineIdle()
    // (unify-tool-support.hpp) for the barrier-job trick that stands in
    // for one.
    unifytool::waitForEngineIdle( rt );

    return parseErrorCount;
}


} // anonymous namespace


int main( int argc, char** argv )
{
    const char* pProgram = argc ? argv[0] : "unify-run";

    bool interactive = false;
    bool trace = false;
    const char* pPath = NULL;

    for( int i = 1; i < argc; ++i ) {
        const char* pArg = argv[i];
        if( 0 == strcmp( pArg, "-i" ) || 0 == strcmp( pArg, "--interactive" ) ) {
            interactive = true;
        } else if( 0 == strcmp( pArg, "--trace" ) ) {
            trace = true;
        } else if( 0 == strcmp( pArg, "-h" ) || 0 == strcmp( pArg, "--help" ) ) {
            printUsage( pProgram, stdout );
            return 0;
        } else if( '-' == pArg[0] && pArg[1] ) {
            fprintf( stderr, "%s: unknown option '%s'.\n", pProgram, pArg );
            printUsage( pProgram, stderr );
            return 2;
        } else if( pPath ) {
            fprintf( stderr, "%s: more than one program given ('%s' and '%s').\n",
                pProgram, pPath, pArg );
            printUsage( pProgram, stderr );
            return 2;
        } else {
            pPath = pArg;
        }
    }

    // No program to run means there is nothing to do but talk to the user.
    if( !pPath ) {
        interactive = true;
    }

    /*
     * Batch mode keeps the engine's stderr trace exactly as it has always
     * been (README.md's `2>/dev/null` still applies, and the golden tests
     * depend on nothing here). An interactive session turns it off,
     * because a prompt buried under a page of trace per keystroke is not a
     * prompt -- and because silencing it wholesale with a shell redirect
     * would take parse-error diagnostics with it. `--trace` puts it back,
     * which is the only way to watch the engine work from the REPL.
     *
     * This is set before the program file runs, not just before the REPL
     * starts, so `-i program.ufy` gives one consistently quiet session
     * rather than a loud load followed by a quiet prompt.
     */
    if( interactive && !trace ) {
        vault::unify::setDebugTraceEnabled( false );
    }

    vault::unify::RuntimeContext rt;
    rt.setupDone();

    int parseErrorCount = 0;
    if( pPath ) {
        parseErrorCount = runProgramFile( rt, pPath );
        if( parseErrorCount < 0 ) {
            // Could not be opened at all -- a usage error, checked before
            // any parsing began, so nothing has run and the REPL (if it
            // was asked for) would start from a state the user did not
            // ask for. Bail out instead.
            return 2;
        }
    }

    int replErrorCount = 0;
    if( interactive ) {
        replErrorCount = unifytool::runRepl( rt );
    }

    int result = 0;
    if( parseErrorCount > 0 ) {
        fprintf( stderr, "unify-run: %d parse error(s) while reading '%s'.\n",
            parseErrorCount, pPath );
        result = 1;
    }
    if( s_runtimeErrorCount > 0 ) {
        fprintf( stderr, "unify-run: %d internal unification error(s) while running '%s'.\n",
            s_runtimeErrorCount, pPath );
        result = 1;
    }
    // Errors typed at the prompt were each reported as they happened; they
    // only need to reach the exit code, which matters for a piped session
    // (`unify-run < script.ufy`) far more than for an interactive one.
    if( replErrorCount > 0 ) {
        result = 1;
    }

    return result;
}
