/**
 * @file contract-main.cpp
 *
 * Runs the session contract suite against every available subject.
 *
 * Today that is FakeSession under two policies. LocalSession joins the list
 * as soon as the engine items it needs are in place; the registration is
 * one call, which is the point of parameterising the suite over a driver.
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

#include <iostream>
#include <memory>

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

    d.scriptDefineError = [ fake ]( const std::string& text,
                                    std::uint32_t count ) {
        std::vector<us::Diagnostic> diags;
        for ( std::uint32_t i = 0; i < count; ++i ) {
            us::Diagnostic diag;
            diag.sev = us::Severity::Error;
            diag.file = "<transcript>";
            diag.line = static_cast<std::uint32_t>( i + 1 );
            diag.column = 1;
            diag.message = "syntax error";
            diag.sourceLine = text;
            diags.push_back( diag );
        }
        fake->scriptDefineError( text, diags );
    };

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

} // namespace

int main()
{
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

    const int failures = registry.run( "session contract suite (G0)" );

    if ( failures != 0 ) {
        std::cout << "\n"
                  << "One or more session contract obligations are unmet.\n"
                  << "See plans/todo/lens/ACCEPTANCE.md gate G0 and\n"
                  << "plans/todo/lens/SESSION-API.md section 8.\n";
    }
    return failures == 0 ? 0 : 1;
}
