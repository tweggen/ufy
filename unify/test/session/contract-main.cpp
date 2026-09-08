/**
 * @file contract-main.cpp
 *
 * Runs the session contract suite against every available subject.
 *
 * Today that is FakeSession under two policies and LocalSession over a real
 * in-process engine. Adding a subject is one registration call, which is the
 * point of parameterising the suite over a driver rather than over a
 * Session.
 *
 * Why the fake is run TWICE: the second run reorders every independent
 * reply. Ordering must not change outcomes, so every case must pass
 * identically under both -- and a case that quietly depends on replies
 * arriving in request order fails in the second run only. Running one
 * policy would leave that dependence undetected, which is exactly the
 * in-process assumption the whole exercise exists to catch.
 */

#include "contract-suite.hpp"
#include "fake-session.hpp"

#include "vault-unify-local-session.hpp"

#include <iostream>
#include <memory>
#include <sstream>

namespace {

using namespace unify_test;

/** Build a value nested `depth` levels deep, for truncation and inspect. */
us::Value deepValue( std::uint32_t depth )
{
    us::Value leaf;
    leaf.kind = us::Value::Kind::Atom;
    leaf.name = "bottom";

    us::Value current = leaf;
    for ( std::uint32_t i = 0; i < depth; ++i ) {
        us::Value node;
        node.kind = us::Value::Kind::Cons;
        node.name = "layer";

        us::Value tag;
        tag.kind = us::Value::Kind::Int;
        tag.i = static_cast<std::int64_t>( i );

        node.args.push_back( current );
        node.args.push_back( tag );
        current = node;
    }
    return current;
}

/**
 * Wrap a FakeSession as a SessionDriver.
 *
 * Everything implementation-specific about the fake is confined to this
 * function; the suite itself never names FakeSession.
 */
SessionDriver makeFakeDriver( FakeSession::Policy policy )
{
    auto fake = std::make_shared<FakeSession>( policy );

    /* A small builtin catalogue, so listing has something to filter out. */
    us::Origin builtinOrigin;
    builtinOrigin.kind = us::Origin::Kind::Builtin;
    for ( const char* n : { "print", "emit", "assert", "retract" } ) {
        us::PredicateKey key;
        key.name = n;
        key.arity = 1;
        fake->addPredicate( key, builtinOrigin, 1, std::string() );
    }

    /* And one synthesized clause, to prove the filter is not a name test. */
    us::Origin synthOrigin;
    synthOrigin.kind = us::Origin::Kind::Synthesized;
    us::PredicateKey synthKey;
    synthKey.name = "__fe__0";
    synthKey.arity = 2;
    fake->addPredicate( synthKey, synthOrigin, 1, "synthesized" );

    SessionDriver d;
    d.session = fake;

    d.settle = [ fake ]() {
        /*
         * Pump to quiescence. Bounded rather than `while(pump())` so a bug
         * that makes the fake generate work forever fails the case instead
         * of hanging the suite.
         */
        for ( int i = 0; i < 100000; ++i ) {
            if ( fake->pump( 1 ) == 0 ) {
                return;
            }
        }
        UT_FAIL( "the fake never went quiet: settle() pumped 100000 actions" );
    };

    d.scriptCountingGoal = [ fake ]( const std::string& goal,
                                     std::uint32_t count ) {
        fake->scriptCountingGoal( goal, count );
    };

    d.scriptNoisyGoal = [ fake ]( const std::string& goal,
                                  std::uint32_t count,
                                  std::uint32_t outputs,
                                  std::uint32_t diagnostics ) {
        ScriptedGoal g;
        for ( std::uint32_t i = 0; i < count; ++i ) {
            us::Value v;
            v.kind = us::Value::Kind::Int;
            v.i = static_cast<std::int64_t>( i );
            g.solutions.push_back( { { "$n", v } } );
        }
        for ( std::uint32_t i = 0; i < outputs; ++i ) {
            us::Output o;
            o.stream = "stdout";
            o.text = "print: noise\n";
            g.outputs.push_back( o );
        }
        for ( std::uint32_t i = 0; i < diagnostics; ++i ) {
            us::Diagnostic diag;
            diag.sev = us::Severity::Warning;
            diag.file = "<goal>";
            diag.line = 1;
            diag.column = 1;
            diag.message = "unknown predicate";
            diag.sourceLine = goal;
            g.diagnostics.push_back( diag );
        }
        fake->scriptGoal( goal, g );
    };

    d.scriptDeepGoal = [ fake ]( const std::string& goal,
                                 std::uint32_t depth ) {
        ScriptedGoal g;
        g.solutions.push_back( { { "$deep", deepValue( depth ) } } );
        fake->scriptGoal( goal, g );
    };

    /*
     * The fake produces two diagnostics for this text, so the suite's
     * "errorCount matches the diagnostics emitted" assertion is checked
     * against a number greater than one -- which is where an implementation
     * that reports a boolean dressed up as a count would slip through.
     */
    d.badDefineText = "this is not a program";
    {
        std::vector<us::Diagnostic> diags;
        for ( int i = 0; i < 2; ++i ) {
            us::Diagnostic diag;
            diag.sev = us::Severity::Error;
            diag.file = "<transcript>";
            diag.line = static_cast<std::uint32_t>( i + 1 );
            diag.column = 1;
            diag.message = "syntax error";
            diag.sourceLine = d.badDefineText;
            diags.push_back( diag );
        }
        fake->scriptDefineError( d.badDefineText, diags );
    }

    d.goodDefineText = "colour( red ).";
    d.goodDefineKey.name = "colour";
    d.goodDefineKey.arity = 0;   /* the fake does not parse arity; see define() */
    d.goodDefineKey.module = us::kNoModule;

    d.forceDisconnect = [ fake ]() { fake->forceDisconnect(); };
    d.injectFailure   = [ fake ]() { fake->failNextRequest(); };

    d.settleSlowly = [ fake ]( unsigned ms ) {
        /*
         * Real wall-clock delivery, for the one obligation that is about
         * time rather than order. One millisecond per action with a floor on
         * total elapsed time, so the case measures a genuinely slow reply
         * without the suite paying 500 ms for every test.
         */
        const auto started = std::chrono::steady_clock::now();
        fake->startBackgroundPump( std::chrono::milliseconds( 1 ) );
        for ( ;; ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started ).count();
            if ( fake->idle() && elapsed >= ms ) {
                break;
            }
            if ( elapsed > ms + 5000 ) {
                fake->stopBackgroundPump();
                UT_FAIL( "settleSlowly timed out after " << elapsed << " ms" );
            }
        }
        fake->stopBackgroundPump();
    };

    return d;
}

/**
 * A Session that rewrites goal text on its way to a real core.
 *
 * The suite says `scriptCountingGoal( "ten.", 10 )` and then
 * `solve( "ten." )`. Against a fake, "ten." is just a key. Against a real
 * engine there is no such goal until a program makes one, and the program's
 * predicate cannot be called "ten." So the driver defines a predicate and
 * records what the suite's name should become; this wrapper applies that
 * mapping, and nothing else, so the suite still calls a plain Session.
 *
 * Everything else forwards unchanged -- which is also a small proof that the
 * boundary is composable, since a proxy is the same shape.
 */
class ScriptedSession : public us::Session {
public:
    explicit ScriptedSession( std::shared_ptr<us::Session> inner )
        : m_inner( std::move( inner ) ) {}

    void mapGoal( const std::string& from, const std::string& to )
    {
        m_goals[ from ] = to;
    }

    us::Capabilities describe() const override { return m_inner->describe(); }
    void close() override { m_inner->close(); }
    void subscribe( us::EventSink& sink, us::Seq resumeFrom ) override
    {
        m_inner->subscribe( sink, resumeFrom );
    }

    us::RequestId define( std::string text, us::Origin origin,
                          us::OverwritePolicy policy ) override
    {
        return m_inner->define( std::move( text ), origin, policy );
    }
    us::RequestId undefine( us::PredicateKey key, us::ModuleId scope ) override
    {
        return m_inner->undefine( key, scope );
    }
    us::RequestId listing( us::ListingFilter filter ) override
    {
        return m_inner->listing( filter );
    }
    us::RequestId source( us::PredicateKey key ) override
    {
        return m_inner->source( key );
    }

    us::QueryId solve( std::string goalText, us::QueryOptions o ) override
    {
        const auto it = m_goals.find( goalText );
        return m_inner->solve( it == m_goals.end() ? goalText : it->second, o );
    }
    us::RequestId demand( us::QueryId q, us::Stream s, std::uint32_t n ) override
    {
        return m_inner->demand( q, s, n );
    }
    us::RequestId cancel( us::QueryId q ) override { return m_inner->cancel( q ); }
    us::RequestId release( us::QueryId q ) override { return m_inner->release( q ); }
    us::RequestId inspect( us::QueryId q, std::uint64_t i, us::ValuePath p,
                           us::ValueBudget b ) override
    {
        return m_inner->inspect( q, i, std::move( p ), b );
    }

    us::RequestId save( std::string path, us::SaveOptions o ) override
    {
        return m_inner->save( std::move( path ), o );
    }
    us::RequestId load( std::string path ) override
    {
        return m_inner->load( std::move( path ) );
    }
    us::RequestId insert( std::string path, us::OverwritePolicy p ) override
    {
        return m_inner->insert( std::move( path ), p );
    }
    us::RequestId debug( us::DebugCommand c ) override
    {
        return m_inner->debug( std::move( c ) );
    }

private:
    std::shared_ptr<us::Session> m_inner;
    std::map<std::string, std::string> m_goals;
};


/**
 * Wrap a real in-process engine as a SessionDriver.
 *
 * The subject differences live here, not in the suite. Two are worth naming:
 *
 *  - "Scripting a goal" means DEFINING one. The fake is told what a goal
 *    answers; a real engine has to be given a program that makes it true, so
 *    scriptCountingGoal( g, 10 ) writes ten facts and points the goal at
 *    them. That is a stronger test of the same criterion -- the ten
 *    solutions have to be produced by resolution rather than handed over.
 *
 *  - settle() is LocalSession::waitUntilQuiet(), which drains the request
 *    queue, puts a barrier job behind everything already submitted, and then
 *    drains the event queue. Crucially it never satisfies demand the suite
 *    did not ask for, which is the property the "delivers exactly 3, then
 *    stops" cases rest on and which a settle() that merely slept would
 *    silently break.
 */
SessionDriver makeLocalDriver()
{
    auto local = std::make_shared<vault::unify::session::LocalSession>();
    if ( local->start() != 0 ) {
        UT_FAIL( "LocalSession::start() failed" );
    }
    auto scripted = std::make_shared<ScriptedSession>( local );

    SessionDriver d;
    d.session = scripted;
    d.settle = [ local ]() { local->waitUntilQuiet(); };

    /* A fresh predicate per scripted goal, so cases cannot collide. */
    auto counter = std::make_shared<int>( 0 );

    auto definePredicate = [ local, counter ]( std::uint32_t count )
        -> std::string
    {
        const std::string pred = "g" + std::to_string( ++( *counter ) );
        std::ostringstream program;
        for ( std::uint32_t i = 0; i < count; ++i ) {
            program << pred << "( " << i << " );\n";
        }
        us::Origin origin;
        origin.kind = us::Origin::Kind::Transcript;
        local->define( program.str(), origin, us::OverwritePolicy::Append );
        local->waitUntilQuiet();
        return pred;
    };

    d.scriptCountingGoal = [ scripted, definePredicate ](
            const std::string& goal, std::uint32_t count ) {
        const std::string pred = definePredicate( count );
        scripted->mapGoal( goal, pred + "( $n );" );
    };

    d.scriptNoisyGoal = [ scripted, definePredicate ](
            const std::string& goal, std::uint32_t count,
            std::uint32_t outputs, std::uint32_t diagnostics ) {
        /*
         * canScriptQueryDiagnostics is false for this subject, so the suite
         * asks for none; if that ever changes, fail loudly rather than
         * quietly under-delivering and letting the attribution case pass
         * for the wrong reason.
         */
        if ( diagnostics != 0 ) {
            UT_FAIL( "the in-process subject cannot script query diagnostics" );
        }
        const std::string pred = definePredicate( count );

        /*
         * The prints come first and run once: `print` leaves no choice
         * point, so backtracking into the facts below it does not re-run
         * them. That gives exactly `outputs` Output events for the query,
         * followed by `count` solutions.
         */
        std::ostringstream body;
        for ( std::uint32_t i = 0; i < outputs; ++i ) {
            body << "print( \"noise\" ); ";
        }
        body << pred << "( $n );";
        scripted->mapGoal( goal, body.str() );
    };

    d.scriptDeepGoal = [ local, scripted, counter ](
            const std::string& goal, std::uint32_t depth ) {
        /*
         * One fact, one solution, one binding -- the suite reads
         * sols[ 0 ].bindings[ 0 ], so a second variable in the goal would
         * make the assertions depend on std::map's ordering of variable
         * names rather than on the value model.
         *
         * `depth` counts LEVELS INCLUDING THE LEAF, which is what
         * applyBudget()'s maxDepth counts: depth 1 is the bare atom
         * `bottom`, depth 3 is n( n( bottom ) ) -- a Cons whose only child
         * is a Cons whose only child is an Atom. A tight budget of
         * maxDepth 1 therefore marks the depth-5 term the truncation case
         * asks for, and maxDepth 32 leaves it alone, which is exactly the
         * pair that case compares.
         *
         * A CHAIN rather than a bush on purpose. maxNodes would otherwise
         * be the limit that fires first and the cases would be testing the
         * wrong rule; one child per level makes depth the only thing the
         * budget can run out of.
         */
        const std::string pred = "deep" + std::to_string( ++( *counter ) );

        std::string term = "bottom";
        for ( std::uint32_t i = 1; i < depth; ++i ) {
            term = "n( " + term + " )";
        }

        std::ostringstream program;
        program << pred << "( " << term << " );\n";

        us::Origin origin;
        origin.kind = us::Origin::Kind::Transcript;
        local->define( program.str(), origin, us::OverwritePolicy::Append );
        local->waitUntilQuiet();

        scripted->mapGoal( goal, pred + "( $deep );" );
    };

    d.badDefineText = "this is not a program";
    d.goodDefineText = "colour( red );\n";
    d.goodDefineKey.name = "colour";
    d.goodDefineKey.arity = 1;
    d.goodDefineKey.module = 1;   /* the session's own pseudo-module */

    /* A real UnifyError makes its goal fail, so a query cannot both report
     * a diagnostic and keep producing solutions. See the field's comment. */
    d.canScriptQueryDiagnostics = false;

    /* An in-process session has no connection to drop and no transport to
     * fail; the cases that need those skip rather than pretend. */
    d.forceDisconnect = nullptr;
    d.injectFailure = nullptr;
    d.settleSlowly = nullptr;

    return d;
}

} // namespace

int main()
{
    /*
     * The engine's stderr trace is on by default and prints several lines
     * per resolution step. Useful when debugging the engine, and it buries
     * the suite's own output completely -- unify-run's REPL turns it off
     * for exactly the same reason.
     */
    vault::unify::setDebugTraceEnabled( false );

    Registry registry;

    {
        FakeSession::Policy benign;
        benign.seed = 20260906;
        registerContractSuite( registry, "fake", [ benign ]() {
            return makeFakeDriver( benign );
        } );
    }

    {
        FakeSession::Policy reordering;
        reordering.seed = 20260906;
        reordering.reorderWindow = 4;
        registerContractSuite( registry, "fake-reordered", [ reordering ]() {
            return makeFakeDriver( reordering );
        } );
    }

    registerContractSuite( registry, "local", []() {
        return makeLocalDriver();
    } );

    const int failures = registry.run( "session contract suite (G0)" );

    if ( failures != 0 ) {
        std::cout << "\n"
                  << "One or more session contract obligations are unmet.\n"
                  << "See plans/todo/lens/ACCEPTANCE.md gate G0 and\n"
                  << "plans/todo/lens/SESSION-API.md section 8.\n";
    }
    return failures == 0 ? 0 : 1;
}
