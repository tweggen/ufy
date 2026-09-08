/**
 * @file nested-binding-test.cpp
 *
 * Engine item E7.0 (plans/todo/lens/E7-STRUCTURED-VALUES.md): a variable
 * that sits INSIDE a bound term must resolve when the solution is handed
 * out, not print as `VT17`.
 *
 * The shape that makes this fail is not exotic -- it is the ordinary way a
 * rule head returns a structure. `wrap( wrapper( $c ) ) { colour( $c ); }`
 * binds the caller's `$x` to `wrapper( $c )` in the CLAUSE's scope, and
 * only afterwards does the body bind `$c`. So the term the solution points
 * at is compound and still contains a variable; rendering it without a
 * UnifyContext cannot follow that variable to `red`, and the caller is
 * handed a term with an engine-internal id where its answer should be.
 *
 * These cases assert at the engine rather than through Session on purpose:
 * `SolveJob::getSolutionList()` is where the rendering happens, and a
 * regression here should name the engine, not the adapter. The session
 * contract suite covers the same property from the other side.
 *
 * Note the lifetime rule this file has to obey and the reason it is worth
 * obeying loudly: `getSolutionList()` may only be called from inside the
 * job's onFinished callback. Engine::executionLoop() drops the last
 * reference the moment that callback returns, and ~SolveJob then frees the
 * arena the solution terms live in (see the comment at
 * vault-unify-local-session.cpp:577-586). Everything the cases read is
 * therefore copied out to strings while the job is still alive.
 */

#include <vault-unify.hpp>
#include <vault-unify-debug.hpp>
#include <vault-unify-solvejob.hpp>

#include <boost/shared_ptr.hpp>

#include <map>
#include <string>
#include <vector>

#include "../session/test-harness.hpp"
#include "../../tools/unify-tool-support.hpp"

namespace {

using namespace unify_test;

const char* const kProgramUri = "<nested-binding-test>";

/*
 * The database every case queries.
 *
 * Two colours rather than one, so a case that accidentally read the same
 * solution twice would not look like a pass. `wrap/1` returns a structure
 * built in the clause's own scope; `deep/1` does it two levels down, which
 * is what proves the resolution recurses rather than handling only the
 * outermost argument.
 */
const char* const kProgram =
    "colour( red );\n"
    "colour( green );\n"
    "\n"
    "wrap( wrapper( $c ) ) {\n"
    "    colour( $c );\n"
    "}\n"
    "\n"
    "deep( outer( middle( $c ) ) ) {\n"
    "    colour( $c );\n"
    "}\n";

/// One solution: variable display name -> the text the engine shipped.
typedef std::map<std::string, std::string> SolutionRow;

/** Collects the solutions of every SolveJob that finishes while it lives. */
struct Collector {
    std::vector<SolutionRow> rows;

    void operator() ( boost::shared_ptr<vault::unify::Job> spJob )
    {
        vault::unify::SolveJob* pSolveJob =
            dynamic_cast<vault::unify::SolveJob*>( spJob.get() );
        if( !pSolveJob ) {
            return;
        }

        vault::unify::SolveJob::SolutionListPtr spList =
            pSolveJob->getSolutionList();
        if( !spList ) {
            return;
        }

        vault::unify::SolveJob::SolutionList::const_iterator it;
        for( it = spList->begin(); it != spList->end(); ++it ) {
            const vault::unify::SolveJob::SolutionMapPtr& spMap = *it;
            if( !spMap ) {
                continue;
            }
            SolutionRow row;
            vault::unify::SolveJob::SolutionMap::const_iterator itVar;
            for( itVar = spMap->begin(); itVar != spMap->end(); ++itVar ) {
                row[ itVar->first ] = itVar->second;
            }
            rows.push_back( row );
        }
    }
};

/** Owns a RuntimeContext with kProgram loaded, and runs queries against it. */
struct Fixture {
    vault::unify::RuntimeContext rt;
    vault::unify::FileDebugInfo* pFileDebugInfo;
    Collector collector;
    int parseErrors;

    Fixture()
        : pFileDebugInfo( NULL )
        , parseErrors( 0 )
    {
        rt.setupDone();
        pFileDebugInfo = new vault::unify::FileDebugInfo( kProgramUri );
        rt.getWorld()->adoptFileDebugInfo( pFileDebugInfo );
        load( kProgram );
    }

    void load( const std::string& strText )
    {
        /*
         * The callback fires on the engine's worker thread. Reading
         * `collector` afterwards is safe only because waitForEngineIdle()
         * synchronizes with that thread through its own barrier job.
         */
        Collector* pCollector = &collector;
        parseErrors += rt.parseExecuteSegment(
            strText.begin(), strText.end(),
            [pCollector]( boost::shared_ptr<vault::unify::Job> spJob ) {
                ( *pCollector )( spJob );
            },
            pFileDebugInfo );
        unifytool::waitForEngineIdle( rt );
    }

    /** Runs one `query { ... }` segment and returns just its solutions. */
    std::vector<SolutionRow> runQuery( const std::string& strQuery )
    {
        collector.rows.clear();
        load( strQuery );
        return collector.rows;
    }
};

/** The one binding named `strVar`, or a failure naming what was there. */
std::string bindingOf( const SolutionRow& row, const std::string& strVar )
{
    SolutionRow::const_iterator it = row.find( strVar );
    if( it == row.end() ) {
        std::string strHave;
        for( it = row.begin(); it != row.end(); ++it ) {
            if( !strHave.empty() ) { strHave += ", "; }
            strHave += it->first;
        }
        UT_FAIL( "no binding for " << strVar << "; the solution has: "
                 << ( strHave.empty() ? std::string( "nothing" ) : strHave ) );
    }
    return it->second;
}

bool contains( const std::string& strHaystack, const std::string& strNeedle )
{
    return strHaystack.find( strNeedle ) != std::string::npos;
}

/** All values bound to `strVar` across the solutions, joined for a message. */
std::string joinBindings( const std::vector<SolutionRow>& rows,
                          const std::string& strVar )
{
    std::string strRes;
    for( size_t i = 0; i < rows.size(); ++i ) {
        if( i ) { strRes += " | "; }
        SolutionRow::const_iterator it = rows[i].find( strVar );
        strRes += ( it == rows[i].end() ) ? std::string( "(unbound)" )
                                          : it->second;
    }
    return strRes;
}

} // namespace

int main()
{
    Registry registry;

    registry.add( "E7.0 a variable nested inside a bound term resolves", []() {
        Fixture f;
        UT_CHECK_EQ( f.parseErrors, 0 );

        std::vector<SolutionRow> rows = f.runQuery( "query { wrap( $x ); }\n" );
        UT_CHECK_MSG( rows.size() == 2,
                      "expected one solution per colour, got " << rows.size() );

        bool sawRed = false;
        bool sawGreen = false;
        for( size_t i = 0; i < rows.size(); ++i ) {
            const std::string strValue = bindingOf( rows[i], "$x" );

            /*
             * The regression this file exists for. Before E7.0 the engine
             * shipped "wrapper( $c (VT17) )" here: the structure was right
             * and the answer inside it was an internal id.
             */
            UT_CHECK_MSG( !contains( strValue, "VT" ),
                          "an unresolved variable reached the caller: "
                              << describe( strValue ) );
            UT_CHECK_MSG( contains( strValue, "wrapper" ),
                          "the structure was lost: " << describe( strValue ) );

            if( contains( strValue, "red" ) )   { sawRed = true; }
            if( contains( strValue, "green" ) ) { sawGreen = true; }
        }

        UT_CHECK_MSG( sawRed && sawGreen,
                      "both colours should appear once; got "
                          << joinBindings( rows, "$x" ) );
    } );

    registry.add( "E7.0 resolution recurses to any depth", []() {
        Fixture f;

        std::vector<SolutionRow> rows = f.runQuery( "query { deep( $x ); }\n" );
        UT_CHECK_EQ( rows.size(), (size_t) 2 );

        for( size_t i = 0; i < rows.size(); ++i ) {
            const std::string strValue = bindingOf( rows[i], "$x" );
            /*
             * Two levels down. A fix that only dereferenced the bound
             * term's immediate arguments would pass the case above and
             * fail here, which is why both are written.
             */
            UT_CHECK_MSG( !contains( strValue, "VT" ),
                          "an unresolved variable survived two levels of "
                          "nesting: " << describe( strValue ) );
            UT_CHECK_MSG( contains( strValue, "outer" )
                              && contains( strValue, "middle" ),
                          "the structure was lost: " << describe( strValue ) );
        }
    } );

    registry.add( "E7.0 a flat binding still renders as its bare atom", []() {
        Fixture f;

        /*
         * The other half of the contract, and the reason this case exists
         * as well as the ones above: E7.0 changed HOW every binding is
         * rendered, not just the broken ones. An ordinary atom binding is
         * what a front end shows most of the time -- lens's transcript
         * golden is literally `$x = red` -- so it is pinned here, exactly,
         * rather than left to a golden in another project to notice.
         */
        std::vector<SolutionRow> rows =
            f.runQuery( "query { colour( $y ); }\n" );
        UT_CHECK_EQ( rows.size(), (size_t) 2 );

        UT_CHECK_EQ( bindingOf( rows[0], "$y" ), std::string( "red" ) );
        UT_CHECK_EQ( bindingOf( rows[1], "$y" ), std::string( "green" ) );
    } );

    return registry.run( "engine item E7.0 (nested bindings resolve)" ) == 0
               ? 0
               : 1;
}
