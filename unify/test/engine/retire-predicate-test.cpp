/**
 * @file retire-predicate-test.cpp
 *
 * Engine item E3 -- predicate-scoped retract -- tested at the engine.
 *
 * `World::retirePredicate()` is what `define( ..., ReplacePredicates )` and
 * `undefine` are built on (lens gate G3.3). It extends the tombstone
 * mechanism runtime `retract` already uses rather than adding a second way
 * to take a clause out: a clause is never removed from the store, only
 * retired at a generation, so a query already walking the predicate keeps
 * the logical update view it started with.
 *
 * The cases are chosen so that the obvious wrong implementations fail:
 * retiring by name alone (another arity survives), retiring everything up to
 * the current end (the replacement is lost), retiring builtins (`print`
 * vanishes), and stamping one generation per clause (a reader could see
 * half a predicate).
 */

#include <vault-unify.hpp>
#include <vault-unify-debug.hpp>
#include <vault-unify-solvejob.hpp>

#include <string>
#include <vector>

#include "../session/test-harness.hpp"
#include "../../tools/unify-tool-support.hpp"

namespace {

using namespace unify_test;

const char* const kUri = "<retire-test>";

/** Counts the solutions of every query that finishes while it lives. */
struct Counter {
    std::vector<std::string> rows;

    void operator() ( boost::shared_ptr<vault::unify::Job> spJob )
    {
        vault::unify::SolveJob* pJob =
            dynamic_cast<vault::unify::SolveJob*>( spJob.get() );
        if( !pJob ) { return; }
        vault::unify::SolveJob::SolutionListPtr spList = pJob->getSolutionList();
        if( !spList ) { return; }
        for( vault::unify::SolveJob::SolutionList::const_iterator it = spList->begin();
             it != spList->end(); ++it ) {
            std::string row;
            if( *it ) {
                for( vault::unify::SolveJob::SolutionMap::const_iterator v = ( *it )->begin();
                     v != ( *it )->end(); ++v ) {
                    row += v->first + "=" + v->second + " ";
                }
            }
            rows.push_back( row );
        }
    }
};

struct Fixture {
    vault::unify::RuntimeContext rt;
    vault::unify::FileDebugInfo* pInfo;
    Counter counter;
    int errors;

    Fixture() : pInfo( NULL ), errors( 0 )
    {
        rt.setupDone();
        pInfo = new vault::unify::FileDebugInfo( kUri );
        rt.getWorld()->adoptFileDebugInfo( pInfo );
    }

    void load( const std::string& text )
    {
        Counter* pCounter = &counter;
        errors += rt.parseExecuteSegment(
            text.begin(), text.end(),
            [pCounter]( boost::shared_ptr<vault::unify::Job> spJob ) {
                ( *pCounter )( spJob );
            },
            pInfo );
        unifytool::waitForEngineIdle( rt );
    }

    std::vector<std::string> solve( const std::string& goal )
    {
        counter.rows.clear();
        load( "query { " + goal + "; }" );
        return counter.rows;
    }

    size_t storeSize()
    {
        return rt.getWorld()->clauseStoreSize();
    }

    vault::unify::CatalogueEntry entry( const std::string& name, int arity )
    {
        std::vector<vault::unify::CatalogueEntry> all;
        rt.getWorld()->copyCatalogue( all );
        for( size_t i = 0; i < all.size(); ++i ) {
            if( all[i].key.name == name && all[i].key.arity == arity ) {
                return all[i];
            }
        }
        return vault::unify::CatalogueEntry();
    }
};

const char* const kProgram =
    "colour( red );\n"
    "colour( green );\n"
    "colour( red, warm );\n"
    "warm( $c ) { colour( $c, warm ); }\n";

} // namespace

int main()
{
    Registry registry;

    registry.add( "E3 retiring a predicate tombstones exactly its clauses", []() {
        Fixture f;
        f.load( kProgram );
        UT_CHECK_EQ( f.errors, 0 );
        UT_CHECK_EQ( f.solve( "colour( $x )" ).size(), std::size_t( 2 ) );

        const size_t retired = f.rt.getWorld()->retirePredicate( "colour", 1, f.storeSize() );
        UT_CHECK_EQ( retired, std::size_t( 2 ) );

        UT_CHECK_EQ( f.solve( "colour( $x )" ).size(), std::size_t( 0 ) );
        /* colour/2 is another predicate, and warm/1 calls it: both intact. */
        UT_CHECK_EQ( f.solve( "colour( $x, $y )" ).size(), std::size_t( 1 ) );
        UT_CHECK_EQ( f.solve( "warm( $c )" ).size(), std::size_t( 1 ) );

        const vault::unify::CatalogueEntry e = f.entry( "colour", 1 );
        UT_CHECK_EQ( e.clauseCount, std::uint32_t( 0 ) );
        UT_CHECK_EQ( e.retiredCount, std::uint32_t( 2 ) );
        UT_CHECK_EQ( f.entry( "colour", 2 ).clauseCount, std::uint32_t( 1 ) );
    } );

    registry.add( "E3 clauses appended after the boundary survive", []() {
        /* The shape of ReplacePredicates: append the new, retire the old. */
        Fixture f;
        f.load( kProgram );
        const size_t boundary = f.storeSize();
        f.load( "colour( blue );\n" );

        UT_CHECK_EQ( f.rt.getWorld()->retirePredicate( "colour", 1, boundary ),
                     std::size_t( 2 ) );
        const std::vector<std::string> rows = f.solve( "colour( $x )" );
        UT_CHECK_EQ( rows.size(), std::size_t( 1 ) );
        UT_CHECK_MSG( !rows.empty() && rows[0].find( "blue" ) != std::string::npos,
                      "the replacement is not what is left: "
                          << ( rows.empty() ? "" : rows[0] ) );
        UT_CHECK_EQ( f.entry( "colour", 1 ).clauseCount, std::uint32_t( 1 ) );
    } );

    registry.add( "E3 builtins are never retired", []() {
        Fixture f;
        f.load( kProgram );
        UT_CHECK_EQ( f.rt.getWorld()->retirePredicate( "print", 1, f.storeSize() ),
                     std::size_t( 0 ) );
        f.solve( "print( still_here )" );
        UT_CHECK_EQ( f.errors, 0 );
        UT_CHECK_MSG( f.entry( "print", 1 ).clauseCount > 0,
                      "print/1 lost its clause" );
    } );

    registry.add( "E3 one retirement is one generation", []() {
        /*
         * All of a predicate's clauses go at ONE generation, so a query
         * started at any moment sees either the whole old definition or
         * none of it -- never a predicate with its first clause gone and
         * its second still there.
         */
        Fixture f;
        f.load( kProgram );
        const uint64_t before = f.rt.getWorld()->currentGeneration();
        f.rt.getWorld()->retirePredicate( "colour", 1, f.storeSize() );
        UT_CHECK_EQ( f.rt.getWorld()->currentGeneration(), before + 1 );

        /* And retiring nothing changes nothing. */
        const uint64_t after = f.rt.getWorld()->currentGeneration();
        UT_CHECK_EQ( f.rt.getWorld()->retirePredicate( "nosuch", 3, f.storeSize() ),
                     std::size_t( 0 ) );
        UT_CHECK_EQ( f.rt.getWorld()->currentGeneration(), after );
    } );

    registry.add( "E3 what a failed replacement appended can be taken back", []() {
        /*
         * ReplacePredicates with a parse error: the engine appends as it
         * parses, so the clauses before the error are already in. Taking
         * them back leaves the world as it was, rather than holding half a
         * new definition beside the whole old one.
         */
        Fixture f;
        f.load( kProgram );
        const size_t boundary = f.storeSize();
        f.load( "colour( blue );\n" );
        UT_CHECK_EQ( f.rt.getWorld()->retireAppendedSince( boundary ), std::size_t( 1 ) );
        UT_CHECK_EQ( f.solve( "colour( $x )" ).size(), std::size_t( 2 ) );
        UT_CHECK_EQ( f.entry( "colour", 1 ).clauseCount, std::uint32_t( 2 ) );
    } );

    return registry.run( "engine item E3: predicate-scoped retract" ) == 0 ? 0 : 1;
}
