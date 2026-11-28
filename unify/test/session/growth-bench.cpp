/**
 * @file growth-bench.cpp
 *
 * Gate G3.7 -- growth under redefinition -- measured at the session
 * boundary.
 *
 * Redefines one 10-clause predicate under ReplacePredicates, 10,000 times,
 * and at checkpoints measures three things against the bounds agreed on
 * 2026-11-28 (plans/todo/lens/ACCEPTANCE.md G3.7):
 *
 *   B1  a query on an UNRELATED predicate stays within 1.5x its starting
 *       time;
 *   B2  a query on the REDEFINED predicate stays within 2x its starting
 *       time;
 *   B3  resident memory stays within the starting figure plus what an undo
 *       history of kUndoDepth redefinitions may keep, plus kRssSlackKb of
 *       allocator slack -- i.e. it does not grow with the number of edits.
 *       Net of what the measuring queries themselves leak; see the control
 *       in main().
 *
 * It talks to the engine only through `Session` -- define, solve, release --
 * and never through World or the clause store. That is the point: the
 * bounds describe behaviour, not mechanism, so the same program measures
 * whatever core sits behind the boundary, including a redesigned one. A
 * bench that called World::clauseStoreSize() would have to be rewritten
 * exactly when it matters most.
 *
 * It is NOT a CTest test, on purpose. The current engine fails B1-B3 by
 * design (tombstoned clauses, linear lookup) and the maintainer chose not to
 * force an optimisation of an engine that may be redesigned. So it reports a
 * verdict and exits 0; `--enforce` makes a failed bound exit 1, for the day
 * the gate is meant to pass. `--quick` stops at 1,000 for a fast look.
 *
 * Run it from an optimised build without sanitizers. Under ASan it runs and
 * even "passes": the instrumentation's own cost swamps the lookup it is
 * meant to time, and the quarantine swamps resident memory.
 *
 * Timing: 100 queries are submitted, then the session is waited on ONCE, and
 * the total is divided by 100. Waiting after every query would add the
 * barrier's fixed cost to each one and dilute every ratio towards 1. Each
 * figure is the best of seven such rounds, after a warm-up.
 */

#include <vault-unify-session.hpp>
#include "vault-unify-local-session.hpp"
#include <vault-unify-debug.hpp>   // only to silence the trace; see main()

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined( __linux__ )
#include <unistd.h>
#endif

namespace {

namespace us = vault::unify::session;

const int kClauses = 10;              //!< size of the predicate redefined
const int kQueriesPerSample = 100;
const int kUndoDepth = 100;           //!< provisional; G3.8 fixes the real one
const long kBytesPerClauseAllowance = 1024; //!< generous: 630 B measured
const long kRssSlackKb = 1024;

const double kB1Ratio = 1.5;
const double kB2Ratio = 2.0;

/** Resident set in KB, or -1 where there is no cheap way to ask. */
long rssKb()
{
#if defined( __linux__ )
    std::ifstream f( "/proc/self/statm" );
    long size = 0, resident = 0;
    if( !( f >> size >> resident ) ) { return -1; }
    return resident * ( sysconf( _SC_PAGESIZE ) / 1024 );
#else
    return -1;
#endif
}

/** Counts what the measurement needs to know it measured something real. */
struct Sink : us::EventSink {
    std::atomic<long> solutions{ 0 };
    std::atomic<long> defineErrors{ 0 };

    void onEvent( const us::Event& e ) override
    {
        if( std::get_if<us::Solution>( &e.body ) ) {
            ++solutions;
        } else if( const us::Defined* d = std::get_if<us::Defined>( &e.body ) ) {
            defineErrors += d->errorCount;
        }
    }
};

struct Sample {
    int redefinitions;
    double edited;     //!< ms per query on the redefined predicate
    double unrelated;  //!< ms per query on an untouched one
    long rss;          //!< KB, -1 if unknown; taken BEFORE this sample's queries
    long queries;      //!< queries run before `rss` was taken
};

} // namespace

int main( int argc, char** argv )
{
    bool enforce = false;
    bool quick = false;
    for( int i = 1; i < argc; ++i ) {
        if( std::strcmp( argv[i], "--enforce" ) == 0 ) { enforce = true; }
        else if( std::strcmp( argv[i], "--quick" ) == 0 ) { quick = true; }
        else {
            std::fprintf( stderr, "usage: %s [--quick] [--enforce]\n", argv[0] );
            return 2;
        }
    }

    /* The one engine call: the debug trace writes per solve step and
     * would be most of what is timed. A redesigned core drops this line. */
    vault::unify::setDebugTraceEnabled( false );

    us::LocalSession session;
    if( session.start() != 0 ) {
        std::fprintf( stderr, "LocalSession::start() failed\n" );
        return 1;
    }
    Sink sink;
    session.subscribe( sink );

    us::Origin origin;
    origin.kind = us::Origin::Kind::Transcript;

    std::string definition;
    for( int i = 0; i < kClauses; ++i ) {
        definition += "p( " + std::to_string( i ) + " );\n";
    }
    session.define( definition, origin, us::OverwritePolicy::Append );
    session.define( "other( a );\n", origin, us::OverwritePolicy::Append );
    session.waitUntilQuiet();

    us::QueryOptions options;
    options.initialDemand = 1000;
    options.retain = false;

    bool sane = true;
    long queriesRun = 0;
    auto timeQuery = [&]( const char* goal, long expected ) {
        const long before = sink.solutions.load();
        std::vector<us::QueryId> ids;
        const auto t0 = std::chrono::steady_clock::now();
        for( int k = 0; k < kQueriesPerSample; ++k ) {
            ids.push_back( session.solve( goal, options ) );
        }
        queriesRun += kQueriesPerSample;
        session.waitUntilQuiet();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0 ).count();
        for( us::QueryId q : ids ) { session.release( q ); }
        session.waitUntilQuiet();
        if( sink.solutions.load() - before != expected * kQueriesPerSample ) {
            sane = false;
        }
        return ms / kQueriesPerSample;
    };

    /* Best of kRounds: the minimum is the latency the world allows, and
     * scheduler noise only ever adds to it. */
    const int kRounds = 7;
    auto best = [&]( const char* goal, long expected ) {
        double m = timeQuery( goal, expected );
        for( int r = 1; r < kRounds; ++r ) {
            const double t = timeQuery( goal, expected );
            if( t < m ) { m = t; }
        }
        return m;
    };
    /* Warm up the allocator, the caches and the worker thread before the
     * baseline every ratio is taken against. */
    best( "p( $x )", kClauses );
    best( "other( $y )", 1 );

    /*
     * Control: what running and releasing a query costs in memory with NO
     * redefinition. Measured 2026-11-28 at roughly a third of a KB per query
     * that is never given back -- and not LocalSession's query map, which
     * was tried and ruled out. That is a leak for engine item E9, not
     * growth under redefinition, so B3 subtracts it rather than charging it
     * to the edits; it is still printed, so it cannot hide in the subtraction.
     */
    const long controlBefore = rssKb();
    const long controlQueriesBefore = queriesRun;
    for( int r = 0; r < 4; ++r ) {
        best( "p( $x )", kClauses );
        best( "other( $y )", 1 );
    }
    const long controlQueries = queriesRun - controlQueriesBefore;
    const double perQueryKb = ( controlBefore >= 0 )
        ? double( rssKb() - controlBefore ) / double( controlQueries )
        : 0.0;

    const int last = quick ? 1000 : 10000;
    std::vector<Sample> samples;
    std::printf( "%8s %12s %12s %10s\n", "redefs", "edited ms", "other ms", "RSS KB" );
    for( int n = 0; n <= last; ++n ) {
        if( n == 0 || n == 100 || n == 1000 || n == 5000 || n == 10000 ) {
            Sample s;
            s.redefinitions = n;
            s.rss = rssKb();
            s.queries = queriesRun;
            s.edited = best( "p( $x )", kClauses );
            s.unrelated = best( "other( $y )", 1 );
            samples.push_back( s );
            std::printf( "%8d %12.3f %12.3f %10ld\n", n, s.edited, s.unrelated, s.rss );
            std::fflush( stdout );
        }
        if( n < last ) {
            session.define( definition, origin, us::OverwritePolicy::ReplacePredicates );
            /* Keep the request queue short; the bench measures the world,
             * not how deep a backlog the session will hold. */
            if( n % 100 == 99 ) { session.waitUntilQuiet(); }
        }
    }
    session.waitUntilQuiet();
    session.close();

    if( !sane || sink.defineErrors.load() != 0 ) {
        std::printf( "INVALID: a query answered the wrong number of solutions "
                     "or a define reported errors; nothing above means anything\n" );
        return 1;
    }

    const Sample& base = samples.front();
    const long undoAllowanceKb =
        long( kUndoDepth ) * kClauses * kBytesPerClauseAllowance / 1024;
    bool pass = true;
    std::printf( "\nnot a G3.7 bound -- per-query memory never given back "
                 "(engine item E9): %.3f KB per query, over %ld queries\n",
                 perQueryKb, controlQueries );
    std::printf( "\nbounds (ACCEPTANCE.md G3.7), against redefs=0; B3 net of "
                 "the per-query figure:\n" );
    for( size_t i = 1; i < samples.size(); ++i ) {
        const Sample& s = samples[i];
        if( s.redefinitions < 1000 ) { continue; }
        const double r1 = s.unrelated / base.unrelated;
        const double r2 = s.edited / base.edited;
        const bool b1 = r1 <= kB1Ratio;
        const bool b2 = r2 <= kB2Ratio;
        std::printf( "  at %5d  B1 unrelated %5.2fx (<= %.1fx) %s"
                     "   B2 edited %5.2fx (<= %.1fx) %s",
                     s.redefinitions, r1, kB1Ratio, b1 ? "pass" : "FAIL",
                     r2, kB2Ratio, b2 ? "pass" : "FAIL" );
        pass = pass && b1 && b2;
        if( s.rss >= 0 && base.rss >= 0 ) {
            const long queryKb = long( perQueryKb * double( s.queries - base.queries ) );
            const long net = s.rss - queryKb;
            const long limit = base.rss + undoAllowanceKb + kRssSlackKb;
            const bool b3 = net <= limit;
            std::printf( "   B3 rss %ld-%ld KB (<= %ld) %s",
                         s.rss, queryKb, limit, b3 ? "pass" : "FAIL" );
            pass = pass && b3;
        } else {
            std::printf( "   B3 not measured on this platform" );
        }
        std::printf( "\n" );
    }
    std::printf( "G3.7: %s%s\n", pass ? "PASS" : "FAIL",
                 enforce ? "" : " (reported, not enforced)" );
    return ( enforce && !pass ) ? 1 : 0;
}
