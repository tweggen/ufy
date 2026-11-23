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
 * vault-unify-local-session.cpp:577-586). Everything the E7.0 cases read is
 * therefore copied out to strings while the job is still alive.
 *
 * Engine item E7.4 adds the last group: a variable the solve left UNBOUND
 * is a row in the solution too, an unbound VarTerm carrying its display
 * name, instead of being dropped. Only the term form changed -- the string
 * form still omits it, deliberately, and one case below pins that.
 *
 * Engine item E7.2 adds the other half, and inverts that rule on purpose.
 * `SolveJob::getGroundedSolutions()` is still taken inside the callback,
 * but what it returns is read AFTER the job is gone -- that is the entire
 * claim of the phase, so the cases below hold the result across the job's
 * death (asserting through a boost::weak_ptr that the job really did die)
 * and only then walk the terms. A GroundedSolutions that still pointed into
 * the arena would be a use-after-free right there, which is why these cases
 * are worth running under ASan rather than merely compiling.
 */

#include <vault-unify.hpp>
#include <vault-unify-debug.hpp>
#include <vault-unify-solvejob.hpp>

/*
 * E7.1's walker, used here only by the E7.2 cases and only to ASSERT with:
 * a Value tree is far easier to read a failure out of than a hand-walked
 * term, and using it here also proves the two halves of E7 fit together.
 * GroundedSolutions itself must never depend on the session header -- it is
 * engine-side and deals in terms; the dependency belongs in the test and in
 * the adapter (E7.3), not in the job.
 */
#include "vault-unify-term-value.hpp"

#include <vault-unify-session-value.hpp>

#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
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
 *
 * `shape/1`, `items/1` and `coords/1` exist for E7.2: one of each term kind
 * that survives to the front end as structure, written as plain facts so
 * that what the case reads back is the literal the program spells out and
 * nothing the solver invented. Empty `[]` and `{}` are deliberately absent
 * -- they do not parse (SPEC.md, and unify/ROADMAP.md records it as a
 * defect), and E7 must not paper over that.
 *
 * `unconstrained/1` exists for E7.4 and is the smallest thing that leaves a
 * CALLER's variable unbound: a fact whose only argument is a variable of
 * its own. `unconstrained( $u )` succeeds, constrains nothing, and the
 * solution therefore has to say something about `$u` without having a
 * value for it. Getting a genuinely unbound variable out of a solution is
 * harder than it looks -- a variable simply absent from the goal is never
 * collected at all (SolveJob::collectGoalVarNames()), so it would prove
 * nothing -- and it must be the goal side that stays unbound, not the
 * clause's: `$u` is what the caller asked about, and if the engine bound
 * `$u` to the fact's `$ignored` instead of the other way round, the row
 * would come back under the wrong display name. The E7.4 cases assert the
 * NAME for exactly that reason.
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
    "}\n"
    "\n"
    "shape( point( 1, two, nested( 3 ) ) );\n"
    "items( [ a, b, c ] );\n"
    "coords( { x: 10, y: 20 } );\n"
    "\n"
    "unconstrained( $ignored );\n";

/// One solution: variable display name -> the text the engine shipped.
typedef std::map<std::string, std::string> SolutionRow;

/** Collects the solutions of every SolveJob that finishes while it lives. */
struct Collector {
    std::vector<SolutionRow> rows;

    /**
     * E7.2: the same solutions as terms, still owned after the job is gone.
     *
     * Move-only, so this vector is the one and only owner of those trees;
     * clearing it (runQuery() does, before every query) frees them.
     */
    std::vector<vault::unify::SolveJob::GroundedSolutions> grounded;

    /**
     * The last SolveJob seen, held WEAKLY so that holding it cannot be what
     * keeps the arena alive. A case that reads terms after this has expired
     * has genuinely read them after ~SolveJob() ran; without it, "the job
     * died" would be an assumption about Engine::executionLoop() rather
     * than something the test checks.
     */
    boost::weak_ptr<vault::unify::Job> wpLastJob;

    void operator() ( boost::shared_ptr<vault::unify::Job> spJob )
    {
        vault::unify::SolveJob* pSolveJob =
            dynamic_cast<vault::unify::SolveJob*>( spJob.get() );
        if( !pSolveJob ) {
            return;
        }

        wpLastJob = spJob;

        /*
         * Taken HERE, inside onFinished, exactly like getSolutionList()
         * below -- the job's arena is still standing at this point. What is
         * pushed is a set of resolveTermGrounded() clones that owns itself,
         * so the terms stay readable once this callback returns and the
         * engine drops its last reference.
         */
        grounded.push_back( pSolveJob->getGroundedSolutions() );

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
        collector.grounded.clear();
        load( strQuery );
        return collector.rows;
    }

    /**
     * Run one query and hand back its grounded solutions BY REFERENCE.
     *
     * By the time this returns, the job that produced them has finished,
     * been dropped by the engine and destroyed -- see waitForEngineIdle()
     * in load(), and the wpLastJob assertion every E7.2 case makes. So
     * everything a caller reads through this reference is read after the
     * arena is gone, which is the property under test.
     */
    const vault::unify::SolveJob::GroundedSolutions&
    runQueryGrounded( const std::string& strQuery )
    {
        runQuery( strQuery );
        if( collector.grounded.size() != 1 ) {
            UT_FAIL( "expected exactly one finished solve job, got "
                     << collector.grounded.size() );
        }
        return collector.grounded[0];
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

/*
 * ---------------------------------------------------------------------------
 * E7.2 helpers.
 * ---------------------------------------------------------------------------
 */

namespace us = vault::unify::session;

/** The grounded term bound to `strVar` in solution `idx`, or a failure. */
const vault::unify::AbstractTerm* groundedTerm(
    const vault::unify::SolveJob::GroundedSolutions& solutions,
    size_t idx,
    const std::string& strVar )
{
    const vault::unify::AbstractTerm* pTerm = solutions.find( idx, strVar );
    if( !pTerm ) {
        UT_FAIL( "solution " << idx << " has no grounded binding for "
                 << strVar );
    }
    return pTerm;
}

/** That same binding as a session Value -- what a front end would receive. */
us::Value groundedValue(
    const vault::unify::SolveJob::GroundedSolutions& solutions,
    size_t idx,
    const std::string& strVar )
{
    return us::toSessionValue( groundedTerm( solutions, idx, strVar ) );
}

/** Renders a Value for a failure message. */
std::string show( const us::Value& value )
{
    return us::toDisplayString( value );
}

/** Is there a `Kind::Var` anywhere in this value tree? */
bool hasVar( const us::Value& value )
{
    if( us::Value::Kind::Var == value.kind ) {
        return true;
    }
    for( size_t i = 0; i < value.args.size(); ++i ) {
        if( hasVar( value.args[i] ) ) { return true; }
    }
    for( size_t i = 0; i < value.pairs.size(); ++i ) {
        if( hasVar( value.pairs[i].second ) ) { return true; }
    }
    return false;
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

    /*
     * -----------------------------------------------------------------------
     * E7.2 -- getGroundedSolutions(): the bindings as terms that outlive the
     * job.
     * -----------------------------------------------------------------------
     */

    registry.add( "E7.2 grounded solutions outlive the job that produced them", []() {
        Fixture f;
        UT_CHECK_EQ( f.parseErrors, 0 );

        /*
         * THE case this phase exists for. runQueryGrounded() returns only
         * after waitForEngineIdle(), i.e. after Engine::executionLoop() has
         * dropped its last reference to the job and ~SolveJob() has freed
         * the whole arena the solution terms used to live in. Every read
         * below therefore happens on memory that would be freed if
         * GroundedSolutions were handing out arena pointers -- and the
         * weak_ptr check is what turns "would" into a fact rather than a
         * belief about the engine's loop.
         */
        const vault::unify::SolveJob::GroundedSolutions& solutions =
            f.runQueryGrounded( "query { wrap( $x ); }\n" );

        UT_CHECK_MSG( f.collector.wpLastJob.expired(),
                      "the solve job is still alive, so this case would pass "
                      "even if the terms pointed straight into its arena" );

        UT_CHECK_EQ( solutions.size(), (size_t) 2 );

        bool sawRed = false;
        bool sawGreen = false;
        for( size_t i = 0; i < solutions.size(); ++i ) {
            const us::Value value = groundedValue( solutions, i, "$x" );

            UT_CHECK_MSG( us::Value::Kind::Cons == value.kind,
                          "the structure was flattened: " << show( value ) );
            UT_CHECK_EQ( value.name, std::string( "wrapper" ) );
            UT_CHECK_EQ( value.args.size(), (size_t) 1 );

            /*
             * The E7.0 shape, asserted at term level this time: `$c` inside
             * `wrapper( $c )` is bound, so the grounded tree must contain
             * the COLOUR, not a VarTerm somebody still has to dereference.
             * getSolutionList()'s string form can only say "no VT appears";
             * here the absence of Kind::Var says it exactly.
             */
            UT_CHECK_MSG( !hasVar( value ),
                          "an unresolved variable survived grounding: "
                              << show( value ) );
            UT_CHECK_MSG( us::Value::Kind::Atom == value.args[0].kind,
                          "expected a bare colour atom, got "
                              << show( value.args[0] ) );

            if( "red" == value.args[0].name )   { sawRed = true; }
            if( "green" == value.args[0].name ) { sawGreen = true; }
        }

        UT_CHECK_MSG( sawRed && sawGreen,
                      "both colours should appear exactly once" );
    } );

    registry.add( "E7.2 the grounded binding is a term, not a VarTerm", []() {
        Fixture f;

        const vault::unify::SolveJob::GroundedSolutions& solutions =
            f.runQueryGrounded( "query { wrap( $x ); }\n" );
        UT_CHECK_EQ( solutions.size(), (size_t) 2 );

        /*
         * One assertion made against the TERM rather than the Value,
         * deliberately: toSessionValue() maps an unbound VarTerm to
         * Kind::Var, so the check above already implies this -- but if E7.1
         * ever changed that mapping, only this line would notice that
         * resolveTermGrounded() had stopped following the binding.
         */
        const vault::unify::AbstractTerm* pTerm =
            groundedTerm( solutions, 0, "$x" );
        const vault::unify::ConsTerm* pCons =
            dynamic_cast<const vault::unify::ConsTerm*>( pTerm );
        UT_CHECK_MSG( pCons != NULL,
                      "expected a ConsTerm, got " << describe( pTerm->toString() ) );
        UT_CHECK_EQ( pCons->getArity(), 1 );
        UT_CHECK_MSG( NULL == dynamic_cast<const vault::unify::VarTerm*>(
                          pCons->getTermAt( 0 ) ),
                      "the nested variable was cloned instead of resolved: "
                          << describe( pTerm->toString() ) );

        /* Asking for something nobody bound is a NULL, not a crash. */
        UT_CHECK( NULL == solutions.find( 0, "$nosuchvar" ) );
        UT_CHECK( NULL == solutions.find( 99, "$x" ) );
    } );

    registry.add( "E7.2 a nested compound keeps its shape", []() {
        Fixture f;

        const vault::unify::SolveJob::GroundedSolutions& solutions =
            f.runQueryGrounded( "query { shape( $s ); }\n" );
        UT_CHECK_EQ( solutions.size(), (size_t) 1 );

        const us::Value value = groundedValue( solutions, 0, "$s" );
        UT_CHECK_MSG( us::Value::Kind::Cons == value.kind, show( value ) );
        UT_CHECK_EQ( value.name, std::string( "point" ) );
        UT_CHECK_EQ( value.args.size(), (size_t) 3 );

        /*
         * The whole reason E7 exists: `1` arrives as a number and
         * `nested( 3 )` arrives as a tree, instead of both arriving as
         * characters the front end would have to re-parse.
         */
        UT_CHECK_MSG( us::Value::Kind::Int == value.args[0].kind,
                      show( value.args[0] ) );
        UT_CHECK_EQ( value.args[0].i, (std::int64_t) 1 );
        UT_CHECK_MSG( us::Value::Kind::Atom == value.args[1].kind,
                      show( value.args[1] ) );
        UT_CHECK_EQ( value.args[1].name, std::string( "two" ) );
        UT_CHECK_MSG( us::Value::Kind::Cons == value.args[2].kind,
                      show( value.args[2] ) );
        UT_CHECK_EQ( value.args[2].name, std::string( "nested" ) );
        UT_CHECK_EQ( value.args[2].args.size(), (size_t) 1 );
        UT_CHECK_EQ( value.args[2].args[0].i, (std::int64_t) 3 );
    } );

    registry.add( "E7.2 an array survives as an array", []() {
        Fixture f;

        const vault::unify::SolveJob::GroundedSolutions& solutions =
            f.runQueryGrounded( "query { items( $l ); }\n" );
        UT_CHECK_EQ( solutions.size(), (size_t) 1 );

        const us::Value value = groundedValue( solutions, 0, "$l" );
        UT_CHECK_MSG( us::Value::Kind::Array == value.kind, show( value ) );
        UT_CHECK_EQ( value.args.size(), (size_t) 3 );
        UT_CHECK_EQ( value.args[0].name, std::string( "a" ) );
        UT_CHECK_EQ( value.args[1].name, std::string( "b" ) );
        UT_CHECK_EQ( value.args[2].name, std::string( "c" ) );
    } );

    registry.add( "E7.2 a map survives as a map", []() {
        Fixture f;

        /*
         * The map case is the one where a mistake shows up as a crash
         * rather than as a wrong value: ~MapTerm() frees the Atom* keys it
         * was built with but NOT its value terms
         * (vault-unify-term-map.cpp), so GroundedSolutions has to free the
         * values and leave the keys alone. Under ASan this case is what
         * says it got that right.
         */
        const vault::unify::SolveJob::GroundedSolutions& solutions =
            f.runQueryGrounded( "query { coords( $m ); }\n" );
        UT_CHECK_EQ( solutions.size(), (size_t) 1 );

        const us::Value value = groundedValue( solutions, 0, "$m" );
        UT_CHECK_MSG( us::Value::Kind::Map == value.kind, show( value ) );
        UT_CHECK_EQ( value.pairs.size(), (size_t) 2 );
        /* std::map key order, per MapTerm's own storage. */
        UT_CHECK_EQ( value.pairs[0].first, std::string( "x" ) );
        UT_CHECK_EQ( value.pairs[0].second.i, (std::int64_t) 10 );
        UT_CHECK_EQ( value.pairs[1].first, std::string( "y" ) );
        UT_CHECK_EQ( value.pairs[1].second.i, (std::int64_t) 20 );
    } );

    registry.add( "E7.2 the string and term forms agree, and coexist", []() {
        Fixture f;

        /*
         * E7.3 switches LocalSession to the term form; unify-repl keeps the
         * string one. Both methods therefore have to keep working off the
         * same job, over the same variables, under the same key names --
         * which is what collectGoalVarNames() is for. This case is the
         * thing that fails if the two ever drift apart.
         *
         * `deep( $x )` on purpose: every variable in it is BOUND, which is
         * the only region where the two forms are still required to agree
         * exactly. E7.4 made them diverge for unbound variables -- the term
         * form emits a row, the string form omits it (see the divergence
         * note on getSolutionList() in vault-unify-solvejob.hpp) -- so the
         * size check below would be wrong for a goal like the E7.4 cases'.
         * That divergence is pinned by its own case, further down, rather
         * than weakened away here.
         */
        std::vector<SolutionRow> rows = f.runQuery( "query { deep( $x ); }\n" );
        const vault::unify::SolveJob::GroundedSolutions& solutions =
            f.collector.grounded.at( 0 );

        UT_CHECK_EQ( rows.size(), solutions.size() );
        for( size_t i = 0; i < rows.size(); ++i ) {
            UT_CHECK_EQ( rows[i].size(), solutions.at( i ).size() );
            SolutionRow::const_iterator it, itEnd = rows[i].end();
            for( it = rows[i].begin(); it != itEnd; ++it ) {
                UT_CHECK_MSG( solutions.find( i, it->first ) != NULL,
                              "the string form has a binding for " << it->first
                                  << " that the term form does not" );
            }
        }

        /* And the structure really is two levels deep on this side too. */
        const us::Value value = groundedValue( solutions, 0, "$x" );
        UT_CHECK_EQ( value.name, std::string( "outer" ) );
        UT_CHECK_EQ( value.args.size(), (size_t) 1 );
        UT_CHECK_EQ( value.args[0].name, std::string( "middle" ) );
        UT_CHECK_MSG( !hasVar( value ),
                      "an unresolved variable survived two levels of nesting: "
                          << show( value ) );
    } );

    registry.add( "E7.2 a binding the job itself allocated survives the job", []() {
        Fixture f;

        /*
         * The case that actually has teeth against a use-after-free, and
         * the reason it is written separately from the ones above.
         *
         * A binding to `wrapper( red )` points into the CLAUSE DATABASE,
         * which outlives every job -- so an implementation that handed out
         * raw arena pointers would give a wrong answer there but would not
         * touch freed memory. Arithmetic and findall are different: their
         * results are built during the solve and adopted into a
         * UnifyContext, whose destructor collects and deletes them
         * (~UnifyContext, vault-unify-unifycontext.cpp) when ~SolveJob()
         * frees the arena. Reading THOSE after the job is gone is a real
         * heap-use-after-free, which is what ASan is here to see.
         */
        const vault::unify::SolveJob::GroundedSolutions& solutions =
            f.runQueryGrounded(
                "query { $sum = 2 + 3 * 4; $all = findall( $c, colour( $c ) ); }\n" );

        UT_CHECK_MSG( f.collector.wpLastJob.expired(),
                      "the solve job is still alive; this case proves nothing "
                      "unless the arena is already gone" );
        UT_CHECK_EQ( solutions.size(), (size_t) 1 );

        const us::Value valSum = groundedValue( solutions, 0, "$sum" );
        UT_CHECK_MSG( us::Value::Kind::Int == valSum.kind, show( valSum ) );
        UT_CHECK_EQ( valSum.i, (std::int64_t) 14 );

        const us::Value valAll = groundedValue( solutions, 0, "$all" );
        UT_CHECK_MSG( us::Value::Kind::Array == valAll.kind, show( valAll ) );
        UT_CHECK_EQ( valAll.args.size(), (size_t) 2 );
        UT_CHECK_EQ( valAll.args[0].name, std::string( "red" ) );
        UT_CHECK_EQ( valAll.args[1].name, std::string( "green" ) );
    } );

    registry.add( "E7.2 GroundedSolutions is move-only and frees what it holds", []() {
        Fixture f;

        /*
         * No ASan finding here is the assertion. Moving the object must
         * hand the trees over exactly once -- the destructor of the
         * moved-FROM object must free nothing (double free) and the
         * destructor of the moved-TO object must free everything (leak).
         * The copy constructor is deleted, so there is no third case to
         * test; that is a compile-time property, asserted here in a
         * static_assert rather than in a comment.
         */
        static_assert(
            !std::is_copy_constructible<
                vault::unify::SolveJob::GroundedSolutions>::value,
            "GroundedSolutions must stay move-only: a copy would give two "
            "objects the same term pointers and double-free them" );

        f.runQuery( "query { shape( $s ); }\n" );
        vault::unify::SolveJob::GroundedSolutions moved(
            std::move( f.collector.grounded.at( 0 ) ) );

        UT_CHECK_EQ( f.collector.grounded.at( 0 ).size(), (size_t) 0 );
        UT_CHECK_EQ( moved.size(), (size_t) 1 );
        UT_CHECK_EQ( groundedValue( moved, 0, "$s" ).name,
                     std::string( "point" ) );

        /*
         * Move ASSIGNMENT has to free the target's own trees before taking
         * the source's, so `target` is deliberately non-empty here: an
         * implementation that just overwrote the vector would leak the
         * items() trees and ASan would say so.
         */
        f.runQuery( "query { items( $l ); }\n" );
        vault::unify::SolveJob::GroundedSolutions target(
            std::move( f.collector.grounded.at( 0 ) ) );
        UT_CHECK_EQ( target.size(), (size_t) 1 );

        target = std::move( moved );
        UT_CHECK_EQ( target.size(), (size_t) 1 );
        UT_CHECK_EQ( moved.size(), (size_t) 0 );
        UT_CHECK_EQ( groundedValue( target, 0, "$s" ).name,
                     std::string( "point" ) );
    } );

    /*
     * -----------------------------------------------------------------------
     * E7.4 -- an unbound variable is a row, not a silence.
     * -----------------------------------------------------------------------
     */

    registry.add( "E7.4 an unbound variable arrives as a Var with its own name", []() {
        Fixture f;
        UT_CHECK_EQ( f.parseErrors, 0 );

        /*
         * THE case for this phase. Before E7.4 the solution simply had no
         * `$u` in it: getGroundedSolutions() found no instance and stored
         * nothing, so a front end could not tell "you asked about $u and it
         * came back open" from "there is no $u". SESSION-API.md section 3
         * calls losing this unusable for the debugging cases that matter
         * most, and it is right -- `$u` staying open is often the ANSWER.
         *
         * Two goals in one query so that the case also pins CARDINALITY:
         * one bound variable and one unbound one must both be present, in
         * the same solution, twice over.
         */
        const vault::unify::SolveJob::GroundedSolutions& solutions =
            f.runQueryGrounded( "query { colour( $y ); unconstrained( $u ); }\n" );

        UT_CHECK_MSG( f.collector.wpLastJob.expired(),
                      "the solve job is still alive, so the VarTerm read "
                      "below might still be the job's own" );
        UT_CHECK_EQ( solutions.size(), (size_t) 2 );

        const char* const expected[] = { "red", "green" };
        for( size_t i = 0; i < solutions.size(); ++i ) {
            UT_CHECK_MSG( solutions.at( i ).size() == 2,
                          "solution " << i << " should carry both variables, "
                          "bound or not; it has "
                              << solutions.at( i ).size() );

            const us::Value valBound = groundedValue( solutions, i, "$y" );
            UT_CHECK_MSG( us::Value::Kind::Atom == valBound.kind,
                          show( valBound ) );
            UT_CHECK_EQ( valBound.name, std::string( expected[i] ) );

            const us::Value valOpen = groundedValue( solutions, i, "$u" );
            UT_CHECK_MSG( us::Value::Kind::Var == valOpen.kind,
                          "an unbound variable must arrive as a Var, got "
                              << show( valOpen ) );
            /*
             * The name is the whole point, and it must be the CALLER's.
             * `unconstrained( $ignored )` is what $u unified against; a
             * solution reporting `$ignored` would be naming a variable the
             * user never wrote, and one reporting `VT390` would be naming
             * nothing at all (toSessionValue()'s fallback for a VarTerm
             * that lost its original name).
             */
            UT_CHECK_EQ( valOpen.name, std::string( "$u" ) );

            /*
             * Asserted against the TERM as well: the Value above would
             * still read as a Var if the engine had shipped some other
             * VarTerm, but this says the tree really is a lone, unbound
             * variable and not, say, a compound that happens to contain
             * one.
             */
            const vault::unify::AbstractTerm* pTerm =
                groundedTerm( solutions, i, "$u" );
            const vault::unify::VarTerm* pVar =
                dynamic_cast<const vault::unify::VarTerm*>( pTerm );
            UT_CHECK_MSG( pVar != NULL,
                          "expected a VarTerm, got "
                              << describe( pTerm->toString() ) );
            UT_CHECK_EQ( pVar->getOriginalVarName(), std::string( "$u" ) );
        }
    } );

    registry.add( "E7.4 findall's template variable comes back open too", []() {
        Fixture f;

        /*
         * The shape that turns up in real programs without anybody writing
         * a variable-only fact. `findall`'s template `$c` is a variable of
         * the OUTER query -- the parser keeps the caller's own VarTerm in
         * `__builtin_findall( $c, colour( $c ), $all )` -- but it is bound
         * only inside the nested, fresh-top-level solve, whose whole arena
         * is gone by the time the outer solution is read (SPEC.md's
         * fresh-scope subgoal limitation). So the outer `$c` is genuinely
         * unbound, and before E7.4 it silently vanished from a solution
         * that did report `$all`.
         */
        const vault::unify::SolveJob::GroundedSolutions& solutions =
            f.runQueryGrounded(
                "query { $all = findall( $c, colour( $c ) ); }\n" );
        UT_CHECK_EQ( solutions.size(), (size_t) 1 );
        UT_CHECK_EQ( solutions.at( 0 ).size(), (size_t) 2 );

        const us::Value valAll = groundedValue( solutions, 0, "$all" );
        UT_CHECK_MSG( us::Value::Kind::Array == valAll.kind, show( valAll ) );
        UT_CHECK_EQ( valAll.args.size(), (size_t) 2 );

        const us::Value valTmpl = groundedValue( solutions, 0, "$c" );
        UT_CHECK_MSG( us::Value::Kind::Var == valTmpl.kind, show( valTmpl ) );
        UT_CHECK_EQ( valTmpl.name, std::string( "$c" ) );
    } );

    registry.add( "E7.4 find() answers NULL only for a variable that is not there", []() {
        Fixture f;

        /*
         * The contract change E7.4 makes to GroundedSolutions::find(). NULL
         * used to mean two different things -- "no such variable" and "that
         * variable is unbound" -- and a caller could not tell which. It now
         * means only the first, which is the one a front end can act on.
         */
        const vault::unify::SolveJob::GroundedSolutions& solutions =
            f.runQueryGrounded( "query { unconstrained( $u ); }\n" );
        UT_CHECK_EQ( solutions.size(), (size_t) 1 );

        UT_CHECK_MSG( solutions.find( 0, "$u" ) != NULL,
                      "an unbound variable must still have a row" );
        UT_CHECK( NULL == solutions.find( 0, "$nosuchvar" ) );
        UT_CHECK( NULL == solutions.find( 99, "$u" ) );
    } );

    registry.add( "E7.4 the string form deliberately still omits it", []() {
        Fixture f;

        /*
         * The divergence, pinned so that it is a decision and not a drift.
         * getSolutionList() is unchanged: it renders bound terms to text
         * and skips unbound variables, because there is no honest string
         * for one and its only remaining caller (unify-repl) has a
         * user-facing output format nobody has been asked about. If someone
         * later makes the REPL print open variables, this case is what
         * tells them the two methods were knowingly out of step and that
         * the header comment explaining why now needs deleting.
         */
        std::vector<SolutionRow> rows =
            f.runQuery( "query { colour( $y ); unconstrained( $u ); }\n" );
        const vault::unify::SolveJob::GroundedSolutions& solutions =
            f.collector.grounded.at( 0 );

        UT_CHECK_EQ( rows.size(), solutions.size() );
        for( size_t i = 0; i < rows.size(); ++i ) {
            UT_CHECK_EQ( rows[i].size(), (size_t) 1 );
            UT_CHECK_MSG( rows[i].find( "$u" ) == rows[i].end(),
                          "the string form gained an unbound binding: "
                              << bindingOf( rows[i], "$u" ) );
            UT_CHECK_EQ( solutions.at( i ).size(), (size_t) 2 );
        }
    } );

    return registry.run( "engine items E7.0/E7.2/E7.4 (nested bindings resolve, "
                         "come out as terms, and unbound ones come out at all)" ) == 0
               ? 0
               : 1;
}
