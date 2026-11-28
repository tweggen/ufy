/**
 * @file source-retention-test.cpp
 *
 * Engine item E5 -- source retention -- tested at the engine.
 *
 * Every parsed clause keeps the text it was parsed from, on its
 * ClauseOrigin. Without it the only way to show a definition is
 * Clause::toString(), which prints the engine's debug form
 * (`warm( $c (VT29) ) :- colour( $c (VT29) ), unify( ... ).`): not the
 * language, not what the user wrote, and not something the parser will read
 * back -- so a browser could show a definition but never let you edit and
 * redefine it (lens gate G3.3).
 *
 * The program is chosen so the easy wrong answers fail: two clauses on one
 * line (a line-based slice would give both to each), a rule using
 * `foreach` (whose desugared clauses must NOT claim the user's text), and a
 * comment between clauses.
 */

#include <vault-unify.hpp>
#include <vault-unify-debug.hpp>

#include <string>
#include <vector>

#include "../session/test-harness.hpp"
#include "../../tools/unify-tool-support.hpp"

namespace {

using namespace unify_test;

void noop( boost::shared_ptr<vault::unify::Job> )
{
}

struct Fixture {
    vault::unify::RuntimeContext rt;
    vault::unify::FileDebugInfo* pInfo;
    int errors;

    Fixture() : pInfo( NULL ), errors( 0 )
    {
        rt.setupDone();
        pInfo = new vault::unify::FileDebugInfo( "<retention-test>" );
        rt.getWorld()->adoptFileDebugInfo( pInfo );
    }

    void load( const std::string& text )
    {
        errors += rt.parseExecuteSegment( text.begin(), text.end(), &noop, pInfo );
        unifytool::waitForEngineIdle( rt );
    }

    /** The live clauses named `name`, with their kinds and texts. */
    std::vector<vault::unify::ClauseOrigin> origins( const std::string& name )
    {
        std::vector<vault::unify::ClauseOrigin> out;
        vault::unify::ExecutionState::ClauseIterator it =
            rt.getWorld()->getRootState()->clauseIterator();
        for( ; it.isValid(); it.next() ) {
            const vault::unify::Clause* p = it.getClause();
            if( p && p->leftHandTerm()
                && p->leftHandTerm()->getName().value() == name ) {
                out.push_back( p->getOrigin() );
            }
        }
        return out;
    }

    std::vector<vault::unify::ClauseOrigin> synthesized()
    {
        std::vector<vault::unify::ClauseOrigin> out;
        vault::unify::ExecutionState::ClauseIterator it =
            rt.getWorld()->getRootState()->clauseIterator();
        for( ; it.isValid(); it.next() ) {
            const vault::unify::Clause* p = it.getClause();
            if( p && p->getOrigin().kind == vault::unify::ClauseOrigin::SYNTHESIZED ) {
                out.push_back( p->getOrigin() );
            }
        }
        return out;
    }
};

const char* const kProgram =
    "colour( red ); colour( green );\n"
    "\n"
    "// warm things\n"
    "warm( $c ) {\n"
    "    colour( $c );\n"
    "}\n"
    "walk( $l ) {\n"
    "    foreach ( $e : $l ) {\n"
    "        colour( $e );\n"
    "    }\n"
    "}\n";

} // namespace

int main()
{
    Registry registry;

    registry.add( "E5 each clause keeps exactly its own text", []() {
        Fixture f;
        f.load( kProgram );
        UT_CHECK_EQ( f.errors, 0 );

        const std::vector<vault::unify::ClauseOrigin> colour = f.origins( "colour" );
        UT_CHECK_EQ( colour.size(), std::size_t( 2 ) );
        if( colour.size() == 2 ) {
            UT_CHECK_EQ( colour[0].sourceText, std::string( "colour( red );" ) );
            UT_CHECK_EQ( colour[1].sourceText, std::string( "colour( green );" ) );
        }
    } );

    registry.add( "E5 a rule keeps its text across lines, comment included", []() {
        Fixture f;
        f.load( kProgram );
        const std::vector<vault::unify::ClauseOrigin> warm = f.origins( "warm" );
        UT_CHECK_EQ( warm.size(), std::size_t( 1 ) );
        if( !warm.empty() ) {
            UT_CHECK_EQ( warm[0].sourceText,
                         std::string( "// warm things\n"
                                      "warm( $c ) {\n"
                                      "    colour( $c );\n"
                                      "}" ) );
        }
    } );

    registry.add( "E5 a desugared rule keeps the loop as written", []() {
        Fixture f;
        f.load( kProgram );
        const std::vector<vault::unify::ClauseOrigin> walk = f.origins( "walk" );
        UT_CHECK_EQ( walk.size(), std::size_t( 1 ) );
        if( !walk.empty() ) {
            UT_CHECK_MSG( walk[0].sourceText.find( "foreach ( $e : $l )" )
                              != std::string::npos,
                          "walk/1's text: " << walk[0].sourceText );
        }
        /* The clauses the loop became are the engine's, and claim no text. */
        const std::vector<vault::unify::ClauseOrigin> synth = f.synthesized();
        UT_CHECK( !synth.empty() );
        for( const vault::unify::ClauseOrigin& o : synth ) {
            UT_CHECK_MSG( o.sourceText.empty(),
                          "a synthesized clause claims text: " << o.sourceText );
        }
    } );

    registry.add( "E5 the kept text parses back to the same predicate", []() {
        Fixture f;
        f.load( kProgram );
        std::string all;
        for( const char* name : { "colour", "warm", "walk" } ) {
            for( const vault::unify::ClauseOrigin& o : f.origins( name ) ) {
                all += o.sourceText + "\n";
            }
        }
        Fixture g;
        g.load( all );
        UT_CHECK_EQ( g.errors, 0 );
        UT_CHECK_EQ( g.origins( "colour" ).size(), std::size_t( 2 ) );
        UT_CHECK_EQ( g.origins( "walk" ).size(), std::size_t( 1 ) );
    } );

    registry.add( "E5 a same-line comment stays; a // inside a string is text", []() {
        Fixture f;
        f.load( "note( \"a // b\" ); // primary\n"
                "next( x );\n" );
        UT_CHECK_EQ( f.errors, 0 );
        const std::vector<vault::unify::ClauseOrigin> note = f.origins( "note" );
        const std::vector<vault::unify::ClauseOrigin> next = f.origins( "next" );
        UT_CHECK_EQ( note.size(), std::size_t( 1 ) );
        UT_CHECK_EQ( next.size(), std::size_t( 1 ) );
        if( !note.empty() && !next.empty() ) {
            UT_CHECK_EQ( note[0].sourceText,
                         std::string( "note( \"a // b\" ); // primary" ) );
            UT_CHECK_EQ( next[0].sourceText, std::string( "next( x );" ) );
        }
    } );

    registry.add( "E5 a block-comment opener inside a string opens nothing", []() {
        // The case that needs the scanner to step over strings: taken for a
        // comment opener, the slash-star would run to the end of the span
        // and cut note/1 off at its opening quote.
        Fixture f;
        f.load( "note( \"/*\" );\n"
                "// about next\n"
                "next( x );\n" );
        UT_CHECK_EQ( f.errors, 0 );
        const std::vector<vault::unify::ClauseOrigin> note = f.origins( "note" );
        const std::vector<vault::unify::ClauseOrigin> next = f.origins( "next" );
        if( note.size() == 1 && next.size() == 1 ) {
            UT_CHECK_EQ( note[0].sourceText, std::string( "note( \"/*\" );" ) );
            UT_CHECK_EQ( next[0].sourceText, std::string( "// about next\nnext( x );" ) );
        } else {
            UT_FAIL( "expected one note/1 and one next/1" );
        }
    } );

    registry.add( "E5 builtins have no text", []() {
        Fixture f;
        for( const vault::unify::ClauseOrigin& o : f.origins( "print" ) ) {
            UT_CHECK( o.sourceText.empty() );
        }
    } );

    return registry.run( "engine item E5: source retention" ) == 0 ? 0 : 1;
}
