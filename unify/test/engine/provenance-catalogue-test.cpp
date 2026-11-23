/**
 * @file provenance-catalogue-test.cpp
 *
 * Engine items E1 (clause provenance) and E2 (definition catalogue), tested
 * against the real engine.
 *
 * These are the two items gate G0.7 of plans/todo/lens/ACCEPTANCE.md rests
 * on -- "listing returns correct kinds with no `__` name-prefix heuristic".
 * The session contract suite asserts that property through the boundary;
 * this asserts it at the engine, where it is actually implemented, so a
 * regression names the engine rather than the adapter.
 *
 * What makes the case worth writing rather than assuming: the program below
 * is chosen so that the OLD heuristics and the NEW facts disagree. It
 * defines a predicate called `__cache`, which the `__`-prefix test would
 * have called internal, and it uses `foreach`/`for`/`if`, which really do
 * desugar into `__fe__N`/`__for__N`/`__if__N` clauses that really are
 * internal. A test whose program contained neither would pass against a
 * name check and prove nothing.
 */

#include <vault-unify.hpp>
#include <vault-unify-debug.hpp>
#include <vault-unify-solvejob.hpp>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "../session/test-harness.hpp"
#include "../../tools/unify-tool-support.hpp"

namespace {

using namespace unify_test;

const char* const kProgramUri = "<provenance-test>";

/*
 * A program that makes every provenance kind observable at once:
 *   colour/1, warm/1  -- plain MODULE clauses
 *   __cache/1         -- MODULE, despite the name the old heuristic hid
 *   walk/1            -- MODULE, but desugars into a SYNTHESIZED __fe__N
 *   count/1           -- MODULE, desugars into a SYNTHESIZED __for__N
 *   guard/1           -- MODULE, desugars into a SYNTHESIZED __if__N
 * plus the engine's own builtins, which are BUILTIN.
 */
const char* const kProgram =
    "colour( red );\n"
    "colour( green );\n"
    "\n"
    "warm( $x ) {\n"
    "    colour( $x );\n"
    "}\n"
    "\n"
    "__cache( warmed );\n"
    "\n"
    "walk( $l ) {\n"
    "    foreach ( $e : $l ) {\n"
    "        colour( $e );\n"
    "    }\n"
    "}\n"
    "\n"
    "count( $n ) {\n"
    "    for ( $i = 0; $i < $n; $i = $i + 1 ) {\n"
    "        colour( red );\n"
    "    }\n"
    "}\n"
    "\n"
    "guard( $x ) {\n"
    "    if ( colour( $x ) ) {\n"
    "        colour( green );\n"
    "    }\n"
    "}\n";

void noopFinished( boost::shared_ptr<vault::unify::Job> )
{
}

/** Owns a RuntimeContext with the test program loaded. */
struct Fixture {
    vault::unify::RuntimeContext rt;
    vault::unify::FileDebugInfo* pFileDebugInfo;
    int parseErrors;

    Fixture()
        : pFileDebugInfo( NULL )
        , parseErrors( 0 )
    {
        rt.setupDone();
        pFileDebugInfo = new vault::unify::FileDebugInfo( kProgramUri );
        rt.getWorld()->adoptFileDebugInfo( pFileDebugInfo );
    }

    void load( const std::string& strText,
               vault::unify::ClauseOrigin::Kind kind
                   = vault::unify::ClauseOrigin::MODULE )
    {
        parseErrors += rt.parseExecuteSegment(
            strText.begin(), strText.end(),
            &noopFinished, pFileDebugInfo, kind );
        unifytool::waitForEngineIdle( rt );
    }

    /** Every clause in the root state, by provenance kind. */
    std::map<int, int> clauseKindCounts()
    {
        std::map<int, int> counts;
        vault::unify::ExecutionState* pRoot = rt.getWorld()->getRootState();
        vault::unify::ExecutionState::ClauseIterator it = pRoot->clauseIterator();
        for( ; it.isValid(); it.next() ) {
            const vault::unify::Clause* pClause = it.getClause();
            if( !pClause ) { continue; }
            ++counts[ (int) pClause->getOrigin().kind ];
        }
        return counts;
    }

    bool findEntry( const std::string& strName, int nArity,
                    vault::unify::CatalogueEntry& out_entry )
    {
        std::vector<vault::unify::CatalogueEntry> entries;
        rt.getWorld()->copyCatalogue( entries );
        for( size_t i = 0; i < entries.size(); ++i ) {
            if( entries[i].key.name == strName
                && entries[i].key.arity == nArity ) {
                out_entry = entries[i];
                return true;
            }
        }
        return false;
    }
};

std::string kindName( vault::unify::ClauseOrigin::Kind kind )
{
    switch( kind ) {
    case vault::unify::ClauseOrigin::MODULE:      return "MODULE";
    case vault::unify::ClauseOrigin::ASSERTED:    return "ASSERTED";
    case vault::unify::ClauseOrigin::BUILTIN:     return "BUILTIN";
    case vault::unify::ClauseOrigin::SYNTHESIZED: return "SYNTHESIZED";
    case vault::unify::ClauseOrigin::TRANSCRIPT:  return "TRANSCRIPT";
    }
    return "?";
}

} // namespace

int main()
{
    Registry registry;

    registry.add( "E1 builtins are BUILTIN, before anything is parsed", []() {
        Fixture f;
        std::map<int, int> counts = f.clauseKindCounts();

        UT_CHECK_MSG( counts[ (int) vault::unify::ClauseOrigin::BUILTIN ] > 0,
                      "a freshly initialised World has no BUILTIN clauses" );
        UT_CHECK_MSG( counts[ (int) vault::unify::ClauseOrigin::MODULE ] == 0,
                      "a freshly initialised World already has MODULE clauses" );
    } );

    registry.add( "E1 parsed clauses are MODULE and carry file and line", []() {
        Fixture f;
        f.load( kProgram );
        UT_CHECK_EQ( f.parseErrors, 0 );

        vault::unify::ExecutionState* pRoot = f.rt.getWorld()->getRootState();
        vault::unify::ExecutionState::ClauseIterator it = pRoot->clauseIterator();

        bool sawColour = false;
        for( ; it.isValid(); it.next() ) {
            const vault::unify::Clause* pClause = it.getClause();
            if( !pClause || !pClause->leftHandTerm() ) { continue; }
            if( pClause->leftHandTerm()->getName().value() != "colour" ) {
                continue;
            }
            const vault::unify::ClauseOrigin& origin = pClause->getOrigin();
            UT_CHECK_MSG( origin.kind == vault::unify::ClauseOrigin::MODULE,
                          "colour/1 has kind " << kindName( origin.kind ) );
            UT_CHECK_EQ( origin.uriFile, std::string( kProgramUri ) );
            UT_CHECK_MSG( origin.line > 0,
                          "a parsed clause has no source line" );
            UT_CHECK_MSG( origin.module != vault::unify::NO_MODULE,
                          "a parsed clause was given no module id" );
            sawColour = true;
        }
        UT_CHECK_MSG( sawColour, "colour/1 is missing from the database" );
    } );

    registry.add( "E1 desugared for/foreach/if clauses are SYNTHESIZED", []() {
        Fixture f;
        f.load( kProgram );

        vault::unify::ExecutionState* pRoot = f.rt.getWorld()->getRootState();
        vault::unify::ExecutionState::ClauseIterator it = pRoot->clauseIterator();

        int synthesized = 0;
        int underscoredButNotSynthesized = 0;
        for( ; it.isValid(); it.next() ) {
            const vault::unify::Clause* pClause = it.getClause();
            if( !pClause || !pClause->leftHandTerm() ) { continue; }
            const std::string& strName =
                pClause->leftHandTerm()->getName().value();
            const bool isUnderscored =
                strName.size() >= 2 && strName.compare( 0, 2, "__" ) == 0;
            const bool isSynthesized =
                pClause->getOrigin().kind
                    == vault::unify::ClauseOrigin::SYNTHESIZED;

            if( isSynthesized ) {
                ++synthesized;
                UT_CHECK_MSG( isUnderscored,
                              "a SYNTHESIZED clause is named '" << strName
                                  << "' -- the parser's naming changed, and "
                                     "any code still keying on __ is now "
                                     "wrong in a new way" );
            }
            /*
             * Count only the USER's clauses here. Builtins are underscored
             * too -- __builtin_eval, __builtin_cut and friends -- which is
             * precisely why the old REPL heuristic needed two tests rather
             * than one, and why neither alone was ever sufficient. With
             * provenance the question is asked directly instead.
             */
            const bool isUserCode =
                pClause->getOrigin().kind == vault::unify::ClauseOrigin::MODULE
                || pClause->getOrigin().kind
                       == vault::unify::ClauseOrigin::TRANSCRIPT
                || pClause->getOrigin().kind
                       == vault::unify::ClauseOrigin::ASSERTED;
            if( isUnderscored && isUserCode ) {
                ++underscoredButNotSynthesized;
            }
        }

        UT_CHECK_MSG( synthesized >= 3,
                      "expected at least three desugared clauses (foreach, "
                      "for, if), found " << synthesized );

        /*
         * The point of the whole exercise: `__cache/1` is the user's, and
         * provenance says so even though its name looks internal. Under the
         * old `__`-prefix heuristic this count would have been 0 -- not
         * because the heuristic was right, but because it could not see the
         * difference.
         */
        UT_CHECK_MSG( underscoredButNotSynthesized == 1,
                      "expected exactly one USER predicate with a __ name "
                      "(__cache/1), found " << underscoredButNotSynthesized );
    } );

    registry.add( "E1 transcript clauses are TRANSCRIPT, not MODULE", []() {
        Fixture f;
        f.load( "typed( here );\n", vault::unify::ClauseOrigin::TRANSCRIPT );

        vault::unify::CatalogueEntry entry;
        UT_CHECK_MSG( f.findEntry( "typed", 1, entry ),
                      "typed/1 is not in the catalogue" );
        UT_CHECK_MSG( entry.kind == vault::unify::ClauseOrigin::TRANSCRIPT,
                      "typed/1 has kind " << kindName( entry.kind )
                          << ", not TRANSCRIPT" );
    } );

    registry.add( "E2 the catalogue counts clauses per name and arity", []() {
        Fixture f;
        f.load( kProgram );

        vault::unify::CatalogueEntry entry;
        UT_CHECK_MSG( f.findEntry( "colour", 1, entry ),
                      "colour/1 is missing from the catalogue" );
        UT_CHECK_EQ( entry.clauseCount, (uint32_t) 2 );
        UT_CHECK_EQ( entry.retiredCount, (uint32_t) 0 );
        UT_CHECK_MSG( entry.kind == vault::unify::ClauseOrigin::MODULE,
                      "colour/1 has kind " << kindName( entry.kind ) );
        UT_CHECK_EQ( entry.uriFile, std::string( kProgramUri ) );
        UT_CHECK_MSG( entry.firstLine > 0, "no first line recorded" );
        UT_CHECK_MSG( entry.generation > 0, "no generation recorded" );

        UT_CHECK_MSG( f.findEntry( "warm", 1, entry ), "warm/1 missing" );
        UT_CHECK_EQ( entry.clauseCount, (uint32_t) 1 );

        /* Arity is part of the key: colour/1 and a colour/2 are distinct. */
        vault::unify::CatalogueEntry unused;
        UT_CHECK_MSG( !f.findEntry( "colour", 2, unused ),
                      "colour/2 was never defined but is in the catalogue" );
    } );

    registry.add( "E2 the catalogue reports builtins with BUILTIN kind", []() {
        Fixture f;

        std::vector<vault::unify::CatalogueEntry> entries;
        f.rt.getWorld()->copyCatalogue( entries );

        int builtins = 0;
        for( size_t i = 0; i < entries.size(); ++i ) {
            if( entries[i].kind == vault::unify::ClauseOrigin::BUILTIN ) {
                ++builtins;
                UT_CHECK_MSG( entries[i].key.module == vault::unify::NO_MODULE,
                              "builtin " << entries[i].key.name
                                  << " claims to belong to a module" );
            }
        }
        UT_CHECK_MSG( builtins > 0, "no builtins in the catalogue" );
    } );

    registry.add( "E2 assert adds to the catalogue as ASSERTED", []() {
        Fixture f;
        f.load( kProgram );
        f.load( "query { assert( runtime( made ) ); }\n" );

        vault::unify::CatalogueEntry entry;
        UT_CHECK_MSG( f.findEntry( "runtime", 1, entry ),
                      "an asserted predicate is missing from the catalogue" );
        UT_CHECK_EQ( entry.clauseCount, (uint32_t) 1 );
        UT_CHECK_MSG( entry.kind == vault::unify::ClauseOrigin::ASSERTED,
                      "runtime/1 has kind " << kindName( entry.kind )
                          << ", not ASSERTED" );
    } );

    registry.add( "E2 retract decrements live count and counts the tombstone",
                  []() {
        Fixture f;
        f.load( kProgram );

        vault::unify::CatalogueEntry before;
        UT_CHECK_MSG( f.findEntry( "colour", 1, before ), "colour/1 missing" );
        UT_CHECK_EQ( before.clauseCount, (uint32_t) 2 );

        f.load( "query { retract( colour( red ) ); }\n" );

        vault::unify::CatalogueEntry after;
        UT_CHECK_MSG( f.findEntry( "colour", 1, after ),
                      "colour/1 vanished from the catalogue after a retract -- "
                      "a predicate with tombstones still exists" );
        UT_CHECK_EQ( after.clauseCount, (uint32_t) 1 );

        /*
         * The tombstone counter is the whole reason this field exists:
         * clauses are never removed, so this is the number that grows
         * without bound under repeated redefinition. Gate G3.7 is written
         * against it, and it cannot be written against a number nobody
         * records.
         */
        UT_CHECK_EQ( after.retiredCount, (uint32_t) 1 );
        UT_CHECK_MSG( after.generation > before.generation,
                      "a retract did not advance the predicate's generation" );
    } );

    registry.add( "E2 the module registry maps ids back to files", []() {
        Fixture f;
        f.load( kProgram );

        vault::unify::CatalogueEntry entry;
        UT_CHECK_MSG( f.findEntry( "colour", 1, entry ), "colour/1 missing" );
        UT_CHECK_MSG( entry.key.module != vault::unify::NO_MODULE,
                      "colour/1 has no module id" );
        UT_CHECK_EQ( f.rt.getWorld()->moduleFile( entry.key.module ),
                     std::string( kProgramUri ) );

        /* An id nobody issued resolves to nothing rather than to garbage. */
        UT_CHECK_EQ( f.rt.getWorld()->moduleFile( 9999 ), std::string() );
        UT_CHECK_EQ( f.rt.getWorld()->moduleFile( vault::unify::NO_MODULE ),
                     std::string() );
    } );

    return registry.run( "engine items E1/E2 (provenance and catalogue)" ) == 0
               ? 0
               : 1;
}
