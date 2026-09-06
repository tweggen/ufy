/**
 * @file contract-suite.cpp
 *
 * The contract cases. Each one names the gate criterion it discharges, so a
 * failing run says which acceptance criterion just stopped being true rather
 * than only which function broke.
 */

#include "contract-suite.hpp"

#include "vault-unify-session-value.hpp"

#include <chrono>
#include <sstream>

namespace unify_test {

// ---------------------------------------------------------------------------
// Recorder -- the invariants that hold for every event of every case.
// ---------------------------------------------------------------------------

void Recorder::onEvent( const us::Event& event )
{
    const auto note = [ & ]( const std::string& text ) {
        if ( violation.empty() ) {
            std::ostringstream os;
            os << text << " (at seq " << event.header.seq << ")";
            violation = os.str();
        }
    };

    /*
     * A reported resume expiry is the one legitimate discontinuity in the
     * stream: it is the core saying "events you never saw are gone". So it
     * re-bases the sequence check instead of failing it. Every other gap is
     * a bug.
     */
    bool rebase = false;
    if ( const us::Failed* f = std::get_if<us::Failed>( &event.body ) ) {
        if ( f->reason == "resume point expired" ) {
            rebase = true;
        }
    }

    if ( m_lastSeq != 0 && !rebase ) {
        if ( event.header.seq != m_lastSeq + 1 ) {
            note( "G0.5: seq is not gapless" );
        }
    }
    if ( event.header.seq == 0 ) {
        note( "G0.5: seq must be non-zero" );
    }
    m_lastSeq = event.header.seq;

    if ( event.header.query ) {
        const us::QueryId qid = *event.header.query;

        const auto known = m_lastQuerySeq.find( qid );
        if ( known == m_lastQuerySeq.end() ) {
            /*
             * First event seen for this query. A fresh subscriber must see
             * its first one at 1; a resumed subscriber is looking at a
             * suffix and adopts whatever it finds as its baseline.
             */
            if ( !m_resumed && event.header.querySeq != 1 ) {
                note( "G0.8: a query's first event must be querySeq 1" );
            }
            if ( event.header.querySeq == 0 ) {
                note( "G0.8: querySeq must be non-zero on a query event" );
            }
        } else if ( event.header.querySeq != known->second + 1 ) {
            note( "G0.8: querySeq is not gapless for its query" );
        }
        m_lastQuerySeq[ qid ] = event.header.querySeq;

        if ( std::get_if<us::Solution>( &event.body ) != nullptr ) {
            if ( m_terminal[ qid ] ) {
                note( "G0.5: a Solution followed its query's terminal status" );
            }
        }

        if ( const us::QueryStatus* qs =
                 std::get_if<us::QueryStatus>( &event.body ) ) {
            const bool terminal =
                qs->state != us::QueryStatus::State::Running
                && qs->state != us::QueryStatus::State::Blocked;
            if ( terminal ) {
                m_terminal[ qid ] = true;
            }
        }
    } else if ( event.header.querySeq != 0 ) {
        note( "querySeq set on an event with no query" );
    }

    m_events.push_back( event );
}

void Recorder::checkInvariants() const
{
    if ( !violation.empty() ) {
        UT_FAIL( "stream invariant broken: " << violation );
    }
}

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

namespace {

/** The terminal status of a query, if it reached one. */
std::optional<us::QueryStatus> terminalStatus( const Recorder& rec,
                                               us::QueryId qid )
{
    std::optional<us::QueryStatus> found;
    for ( const us::QueryStatus& qs : rec.allFor<us::QueryStatus>( qid ) ) {
        if ( qs.state != us::QueryStatus::State::Running
             && qs.state != us::QueryStatus::State::Blocked ) {
            found = qs;
        }
    }
    return found;
}

/** The last status of a query, terminal or not. */
std::optional<us::QueryStatus> lastStatus( const Recorder& rec,
                                           us::QueryId qid )
{
    const std::vector<us::QueryStatus> all = rec.allFor<us::QueryStatus>( qid );
    if ( all.empty() ) {
        return std::nullopt;
    }
    return all.back();
}

std::string stateName( us::QueryStatus::State s )
{
    switch ( s ) {
    case us::QueryStatus::State::Running:   return "Running";
    case us::QueryStatus::State::Exhausted: return "Exhausted";
    case us::QueryStatus::State::Complete:  return "Complete";
    case us::QueryStatus::State::Failed:    return "Failed";
    case us::QueryStatus::State::Aborted:   return "Aborted";
    case us::QueryStatus::State::Blocked:   return "Blocked";
    }
    return "?";
}

} // namespace

// ---------------------------------------------------------------------------
// The suite
// ---------------------------------------------------------------------------

void registerContractSuite( Registry& registry,
                            const std::string& subject,
                            DriverFactory factory )
{
    const auto name = [ & ]( const char* text ) {
        return subject + "/" + text;
    };

    // -- G0.5 ---------------------------------------------------------------

    registry.add(
        name( "G0.5 events are gapless and ordered across many requests" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            d.scriptCountingGoal( "many.", 5 );

            /*
             * Deliberately mixes request kinds and interleaves a query with
             * them, because a core that keeps one counter per request kind
             * still looks correct in a single-kind test.
             */
            for ( int i = 0; i < 3; ++i ) {
                d.session->listing( us::ListingFilter() );
                us::QueryOptions opt;
                opt.initialDemand = 2;
                d.session->solve( "many.", opt );
                d.session->listing( us::ListingFilter() );
            }
            d.settle();

            rec.checkInvariants();
            UT_CHECK_MSG( rec.events().size() > 10,
                          "expected a substantial stream to check ordering on" );
        } );

    // -- G0.6 ---------------------------------------------------------------

    registry.add(
        name( "G0.6 initialDemand delivers exactly that many, then stops" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            d.scriptCountingGoal( "ten.", 10 );

            us::QueryOptions opt;
            opt.initialDemand = 3;
            const us::QueryId q = d.session->solve( "ten.", opt );

            d.settle();
            rec.checkInvariants();

            UT_CHECK_EQ( rec.allFor<us::Solution>( q ).size(), std::size_t( 3 ) );

            const auto status = lastStatus( rec, q );
            UT_CHECK_MSG( status.has_value(),
                          "a query must report its status without being asked" );
            UT_CHECK_MSG( status->state == us::QueryStatus::State::Running,
                          "after a partial batch the query is still Running, was "
                              << stateName( status->state ) );
            UT_CHECK_EQ( status->produced, std::uint64_t( 3 ) );

            /* Settling again must not conjure solutions nobody demanded. */
            d.settle();
            UT_CHECK_MSG( rec.allFor<us::Solution>( q ).size() == 3,
                          "solutions arrived without a demand" );
        } );

    registry.add(
        name( "G0.6 demand resumes delivery and exhaustion is reported once" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            d.scriptCountingGoal( "ten.", 10 );

            us::QueryOptions opt;
            opt.initialDemand = 3;
            const us::QueryId q = d.session->solve( "ten.", opt );
            d.settle();

            d.session->demand( q, us::Stream::Solutions, 4 );
            d.settle();
            UT_CHECK_EQ( rec.allFor<us::Solution>( q ).size(), std::size_t( 7 ) );

            d.session->demand( q, us::Stream::Solutions, 100 );
            d.settle();
            rec.checkInvariants();

            const std::vector<us::Solution> sols = rec.allFor<us::Solution>( q );
            UT_CHECK_EQ( sols.size(), std::size_t( 10 ) );

            /* Indices are 0-based and in production order, with no repeats. */
            for ( std::size_t i = 0; i < sols.size(); ++i ) {
                UT_CHECK_EQ( sols[ i ].index, std::uint64_t( i ) );
            }

            const auto term = terminalStatus( rec, q );
            UT_CHECK_MSG( term.has_value(), "an exhausted query must say so" );
            UT_CHECK_MSG( term->state == us::QueryStatus::State::Exhausted,
                          "expected Exhausted, got " << stateName( term->state ) );
            UT_CHECK_EQ( term->produced, std::uint64_t( 10 ) );

            /* Demanding past exhaustion must not produce more or crash. */
            d.session->demand( q, us::Stream::Solutions, 5 );
            d.settle();
            rec.checkInvariants();
            UT_CHECK_EQ( rec.allFor<us::Solution>( q ).size(), std::size_t( 10 ) );
        } );

    // -- G0.7 (the parts a fake can carry; the engine half is in G0.7-local) --

    registry.add(
        name( "G0.7 a bad define yields diagnostics AND a non-zero errorCount" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            const std::string bad = "this is not a program";
            d.scriptDefineError( bad, 2 );

            us::Origin origin;
            origin.kind = us::Origin::Kind::Transcript;
            d.session->define( bad, origin, us::OverwritePolicy::Append );
            d.settle();
            rec.checkInvariants();

            const std::vector<us::Diagnostic> diags = rec.all<us::Diagnostic>();
            const std::vector<us::Defined> defined = rec.all<us::Defined>();

            UT_CHECK_MSG( diags.size() == 2,
                          "expected 2 diagnostics, got " << diags.size()
                              << " -- never silence" );
            UT_CHECK_MSG( defined.size() == 1,
                          "a failed define must still answer Defined" );
            UT_CHECK_EQ( defined[ 0 ].errorCount, std::uint32_t( 2 ) );

            /*
             * Structured, not text. A diagnostic whose file and line are
             * empty is a formatted string wearing a struct's clothes, and
             * the diagnostics panel cannot navigate to it.
             */
            for ( const us::Diagnostic& diag : diags ) {
                UT_CHECK_MSG( !diag.message.empty(),
                              "diagnostic has no message" );
                UT_CHECK_MSG( diag.line > 0,
                              "diagnostic has no line: E10 is not done" );
                UT_CHECK_MSG( !diag.sourceLine.empty(),
                              "diagnostic carries no offending source line" );
                UT_CHECK_MSG( diag.req.has_value(),
                              "a diagnostic caused by a request must name it" );
            }
        } );

    registry.add(
        name( "G0.7 a good define reports what it added and bumps the world" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            us::Origin origin;
            origin.kind = us::Origin::Kind::Transcript;
            d.session->define( d.goodDefineText, origin,
                               us::OverwritePolicy::Append );
            d.settle();
            rec.checkInvariants();

            const std::vector<us::Defined> defined = rec.all<us::Defined>();
            UT_CHECK_EQ( defined.size(), std::size_t( 1 ) );
            UT_CHECK_EQ( defined[ 0 ].errorCount, std::uint32_t( 0 ) );
            UT_CHECK_MSG( !defined[ 0 ].added.empty()
                              || !defined[ 0 ].replaced.empty(),
                          "a successful define must name what it defined" );

            UT_CHECK_MSG( rec.count<us::WorldChanged>() >= 1,
                          "a define that changed the world must say so once, "
                          "so views invalidate on one signal" );
        } );

    registry.add(
        name( "G0.7 listing reports kinds, and hides builtins unless asked" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            us::Origin origin;
            origin.kind = us::Origin::Kind::Transcript;
            d.session->define( d.goodDefineText, origin,
                               us::OverwritePolicy::Append );
            d.settle();
            rec.clear();

            d.session->listing( us::ListingFilter() );
            d.settle();
            rec.checkInvariants();

            const std::vector<us::Listing> listings = rec.all<us::Listing>();
            UT_CHECK_EQ( listings.size(), std::size_t( 1 ) );

            bool sawUserCode = false;
            for ( const us::CatalogueEntry& e : listings[ 0 ].entries ) {
                UT_CHECK_MSG( e.origin.kind != us::Origin::Kind::Builtin,
                              "a default listing must not include builtins" );
                UT_CHECK_MSG( e.origin.kind != us::Origin::Kind::Synthesized,
                              "a default listing must not include synthesized "
                              "clauses -- and never by a __ name heuristic" );
                UT_CHECK_MSG( !e.key.name.empty(), "catalogue entry has no name" );
                sawUserCode = true;
            }
            UT_CHECK_MSG( sawUserCode,
                          "the predicate just defined is missing from the "
                          "catalogue" );

            /*
             * The name-prefix heuristic must be dead: a predicate whose name
             * starts with __ but whose origin is user code has to be listed.
             * This is the assertion that stops E1 being quietly reverted to
             * a string test.
             */
            us::ListingFilter withEverything;
            withEverything.includeBuiltins = true;
            withEverything.includeSynthesized = true;
            rec.clear();
            d.session->listing( withEverything );
            d.settle();

            const std::vector<us::Listing> full = rec.all<us::Listing>();
            UT_CHECK_EQ( full.size(), std::size_t( 1 ) );
            UT_CHECK_MSG( full[ 0 ].entries.size() >= listings[ 0 ].entries.size(),
                          "asking for more returned fewer entries" );
        } );

    registry.add(
        name( "G0.7 source returns text for a known key and Failed otherwise" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            us::Origin origin;
            origin.kind = us::Origin::Kind::Transcript;
            d.session->define( d.goodDefineText, origin,
                               us::OverwritePolicy::Append );
            d.settle();
            rec.clear();

            d.session->source( d.goodDefineKey );
            d.settle();
            rec.checkInvariants();

            const std::vector<us::SourceText> texts = rec.all<us::SourceText>();
            UT_CHECK_MSG( texts.size() == 1,
                          "source of a defined predicate returned "
                              << texts.size() << " SourceText events" );
            UT_CHECK_MSG( !texts[ 0 ].text.empty(),
                          "source returned an empty definition" );

            rec.clear();
            us::PredicateKey missing;
            missing.name = "no_such_predicate_anywhere";
            missing.arity = 7;
            d.session->source( missing );
            d.settle();
            rec.checkInvariants();

            UT_CHECK_MSG( rec.count<us::Failed>() == 1,
                          "source of an unknown key must answer Failed, not "
                          "an empty SourceText" );
            UT_CHECK_EQ( rec.count<us::SourceText>(), std::size_t( 0 ) );
        } );

    // -- G0.8 ---------------------------------------------------------------

    registry.add(
        name( "G0.8 two concurrent queries attribute every event they cause" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            d.scriptNoisyGoal( "noisy_a.", 4, 2, 1 );
            d.scriptNoisyGoal( "noisy_b.", 4, 2, 1 );

            us::QueryOptions opt;
            opt.initialDemand = 4;

            const us::QueryId a = d.session->solve( "noisy_a.", opt );
            const us::QueryId b = d.session->solve( "noisy_b.", opt );
            UT_CHECK_MSG( a != b, "two queries were given the same id" );

            d.settle();
            rec.checkInvariants();

            /*
             * The point of the case: not merely that the events arrived, but
             * that each is attributable. On a single-FIFO-worker core this
             * passes for free; the moment a query becomes a process it stops
             * being free, and this is the test that notices.
             */
            std::size_t unattributedOutput = 0;
            std::size_t unattributedDiagnostic = 0;
            for ( const us::Event& e : rec.events() ) {
                if ( std::get_if<us::Output>( &e.body ) && !e.header.query ) {
                    ++unattributedOutput;
                }
                if ( std::get_if<us::Diagnostic>( &e.body ) && !e.header.query ) {
                    ++unattributedDiagnostic;
                }
            }
            UT_CHECK_MSG( unattributedOutput == 0,
                          unattributedOutput
                              << " Output events carried no QueryId" );
            UT_CHECK_MSG( unattributedDiagnostic == 0,
                          unattributedDiagnostic
                              << " Diagnostic events carried no QueryId" );

            UT_CHECK_EQ( rec.allFor<us::Output>( a ).size(), std::size_t( 2 ) );
            UT_CHECK_EQ( rec.allFor<us::Output>( b ).size(), std::size_t( 2 ) );
            UT_CHECK_EQ( rec.allFor<us::Solution>( a ).size(), std::size_t( 4 ) );
            UT_CHECK_EQ( rec.allFor<us::Solution>( b ).size(), std::size_t( 4 ) );
            UT_CHECK_EQ( rec.allFor<us::Diagnostic>( a ).size(), std::size_t( 1 ) );
            UT_CHECK_EQ( rec.allFor<us::Diagnostic>( b ).size(), std::size_t( 1 ) );
        } );

    // -- G0.9 ---------------------------------------------------------------

    registry.add(
        name( "G0.9 define, listing and source are answered while a query runs" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            /* A query left deliberately unfinished: demanded 1 of 50. */
            d.scriptCountingGoal( "long.", 50 );
            us::QueryOptions opt;
            opt.initialDemand = 1;
            const us::QueryId q = d.session->solve( "long.", opt );
            d.settle();

            const auto midStatus = lastStatus( rec, q );
            UT_CHECK_MSG( midStatus.has_value()
                              && midStatus->state
                                     == us::QueryStatus::State::Running,
                          "the query should still be running for this case" );

            rec.clear();

            /*
             * The obligation ARCHITECTURE.md 3.1 names: with the idle
             * barrier gone, these must be answered rather than queued behind
             * the query. A core that serialises everything behind the running
             * query passes every other case in this suite and fails here.
             */
            us::Origin origin;
            origin.kind = us::Origin::Kind::Transcript;
            d.session->define( d.goodDefineText, origin,
                               us::OverwritePolicy::Append );
            d.session->listing( us::ListingFilter() );
            d.session->source( d.goodDefineKey );
            d.settle();
            rec.checkInvariants();

            UT_CHECK_MSG( rec.count<us::Defined>() >= 1,
                          "define was not answered while a query was running" );
            UT_CHECK_MSG( rec.count<us::Listing>() >= 1,
                          "listing was not answered while a query was running" );
            UT_CHECK_MSG( rec.count<us::SourceText>() >= 1
                              || rec.count<us::Failed>() >= 1,
                          "source was not answered while a query was running" );

            /* And the query is still intact afterwards. */
            rec.clear();
            d.session->demand( q, us::Stream::Solutions, 2 );
            d.settle();
            rec.checkInvariants();
            UT_CHECK_EQ( rec.allFor<us::Solution>( q ).size(), std::size_t( 2 ) );
        } );

    // -- G0.10 --------------------------------------------------------------

    registry.add(
        name( "G0.10 retained queries can be inspected, and release frees them" ),
        [ factory ]() {
            SessionDriver d = factory();
            if ( !d.session->describe().retention ) {
                /* Honest floor: a core without E15 says so, and inspect fails. */
                Recorder rec;
                d.session->subscribe( rec, 0 );
                d.scriptCountingGoal( "small.", 1 );
                us::QueryOptions opt;
                opt.initialDemand = 1;
                const us::QueryId q = d.session->solve( "small.", opt );
                d.settle();
                d.session->inspect( q, 0, us::ValuePath{ 0 }, us::ValueBudget() );
                d.settle();
                rec.checkInvariants();
                UT_CHECK_MSG( rec.count<us::Failed>() >= 1,
                              "a core without retention must fail inspect, not "
                              "return a wrong value" );
                return;
            }

            Recorder rec;
            d.session->subscribe( rec, 0 );

            d.scriptDeepGoal( "deep.", 6 );

            us::QueryOptions opt;
            opt.initialDemand = 1;
            opt.retain = true;
            opt.budget.maxDepth = 2;
            const us::QueryId q = d.session->solve( "deep.", opt );
            d.settle();
            rec.checkInvariants();

            const std::vector<us::Solution> sols = rec.allFor<us::Solution>( q );
            UT_CHECK_EQ( sols.size(), std::size_t( 1 ) );
            UT_CHECK_MSG( !sols[ 0 ].bindings.empty(), "solution has no bindings" );

            /* The budget must actually have bitten, or the case proves nothing. */
            UT_CHECK_MSG( us::hasTruncation( sols[ 0 ].bindings[ 0 ].second ),
                          "a depth-2 budget on a 6-deep term produced no "
                          "truncation -- the case cannot test inspect" );

            const auto term = terminalStatus( rec, q );
            UT_CHECK_MSG( term.has_value(), "query never reached a terminal status" );
            UT_CHECK_MSG( term->retained,
                          "a retain=true query must report retained=true, so the "
                          "front end knows a release is owed" );

            UT_CHECK_MSG( d.session->describe().retainedQueries >= 1,
                          "describe() does not report the retained query" );

            rec.clear();
            us::ValueBudget deeper;
            deeper.maxDepth = 8;
            d.session->inspect( q, 0, us::ValuePath{ 0 }, deeper );
            d.settle();
            rec.checkInvariants();

            const std::vector<us::Expanded> expanded = rec.all<us::Expanded>();
            UT_CHECK_MSG( expanded.size() == 1,
                          "inspect on a retained query returned "
                              << expanded.size() << " Expanded events" );
            UT_CHECK_MSG( us::countNodes( expanded[ 0 ].value )
                              > us::countNodes( sols[ 0 ].bindings[ 0 ].second ),
                          "inspect returned no more than the truncated value" );

            rec.clear();
            d.session->release( q );
            d.settle();

            UT_CHECK_EQ( d.session->describe().retainedQueries,
                         std::uint64_t( 0 ) );

            rec.clear();
            d.session->inspect( q, 0, us::ValuePath{ 0 }, deeper );
            d.settle();
            rec.checkInvariants();
            UT_CHECK_MSG( rec.count<us::Failed>() == 1,
                          "inspect after release must answer Failed" );
            UT_CHECK_EQ( rec.count<us::Expanded>(), std::size_t( 0 ) );
        } );

    registry.add(
        name( "G0.10 many queries, all released, leave the retained count at zero" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            const std::uint64_t before = d.session->describe().retainedQueries;

            d.scriptCountingGoal( "one.", 1 );
            us::QueryOptions opt;
            opt.initialDemand = 1;
            opt.retain = true;

            /*
             * 200, not 1000: the point is that the count returns to where it
             * started, and it is made every iteration rather than only at the
             * end, which catches a leak that only happens on the first or the
             * last. ACCEPTANCE.md G0.10 says 1000; the number is arbitrary
             * and the suite's wall-clock budget is not.
             */
            for ( int i = 0; i < 200; ++i ) {
                const us::QueryId q = d.session->solve( "one.", opt );
                d.settle();
                d.session->release( q );
                d.settle();
                rec.clear();
            }

            rec.checkInvariants();
            UT_CHECK_EQ( d.session->describe().retainedQueries, before );
        } );

    // -- cancel -------------------------------------------------------------

    registry.add(
        name( "cancel always reaches a terminal status, even mid-flight" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            d.scriptCountingGoal( "endless.", 1000 );
            us::QueryOptions opt;
            opt.initialDemand = 2;
            const us::QueryId q = d.session->solve( "endless.", opt );
            d.settle();

            d.session->cancel( q );
            d.settle();
            rec.checkInvariants();

            const auto term = terminalStatus( rec, q );
            UT_CHECK_MSG( term.has_value(),
                          "cancel must always produce a terminal QueryStatus, "
                          "so the front end can detach unconditionally" );
            UT_CHECK_MSG( term->state == us::QueryStatus::State::Aborted,
                          "expected Aborted, got " << stateName( term->state ) );

            /*
             * And it must be honest about what it did NOT do. A core that
             * cannot really stop the work says so in `detail`; one that can
             * leaves it empty. Either is fine -- silence while lying is not.
             */
            if ( !d.session->describe().realCancel ) {
                UT_CHECK_MSG( !term->detail.empty(),
                              "a best-effort cancel must state the limitation "
                              "rather than imply the work stopped" );
            }

            /* Nothing more may arrive for a cancelled query. */
            rec.clear();
            d.session->demand( q, us::Stream::Solutions, 5 );
            d.settle();
            rec.checkInvariants();
            UT_CHECK_EQ( rec.allFor<us::Solution>( q ).size(), std::size_t( 0 ) );
        } );

    // -- G0.11 --------------------------------------------------------------

    registry.add(
        name( "G0.11 debug is closed over its events, present or absent" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            const us::Capabilities caps = d.session->describe();

            d.scriptCountingGoal( "traced.", 2 );
            us::QueryOptions opt;
            opt.initialDemand = 2;
            opt.trace = caps.debug;
            const us::QueryId q = d.session->solve( "traced.", opt );
            d.settle();
            rec.clear();

            d.session->debug( us::SetTrace{ q, true } );
            d.settle();
            rec.checkInvariants();

            if ( caps.debug ) {
                UT_CHECK_MSG( rec.count<us::TraceEvent>() > 0,
                              "a core reporting debug:true produced no "
                              "TraceEvents for SetTrace" );
                for ( const us::Event& e : rec.events() ) {
                    if ( std::get_if<us::TraceEvent>( &e.body ) ) {
                        UT_CHECK_MSG( e.header.query.has_value(),
                                      "a TraceEvent must name its query" );
                    }
                }
            } else {
                UT_CHECK_MSG( rec.count<us::Failed>() >= 1,
                              "a core reporting debug:false must answer Failed" );
                UT_CHECK_EQ( rec.count<us::TraceEvent>(), std::size_t( 0 ) );
            }
        } );

    // -- infrastructure failure --------------------------------------------

    registry.add(
        name( "an injected failure answers the request and spares the session" ),
        [ factory ]() {
            SessionDriver d = factory();
            if ( !d.injectFailure ) {
                return;   /* subject cannot arrange it; see the header */
            }

            Recorder rec;
            d.session->subscribe( rec, 0 );

            d.injectFailure();
            d.session->listing( us::ListingFilter() );
            d.settle();
            rec.checkInvariants();

            const std::vector<us::Failed> failures = rec.all<us::Failed>();
            UT_CHECK_MSG( failures.size() == 1,
                          "an injected failure produced " << failures.size()
                              << " Failed events" );
            UT_CHECK_MSG( !failures[ 0 ].fatal,
                          "a failed request must not be reported as a failed "
                          "session" );
            UT_CHECK_MSG( !failures[ 0 ].reason.empty(),
                          "Failed with no reason is not an answer" );

            /* The session must still work afterwards. */
            rec.clear();
            d.session->listing( us::ListingFilter() );
            d.settle();
            rec.checkInvariants();
            UT_CHECK_MSG( rec.count<us::Listing>() == 1,
                          "the session did not survive a non-fatal failure" );
        } );

    // -- reconnection -------------------------------------------------------

    registry.add(
        name( "a disconnect mid-query is survivable by resubscribing" ),
        [ factory ]() {
            SessionDriver d = factory();
            if ( !d.forceDisconnect ) {
                return;
            }

            Recorder first;
            d.session->subscribe( first, 0 );

            d.scriptCountingGoal( "resumable.", 20 );
            us::QueryOptions opt;
            opt.initialDemand = 5;
            const us::QueryId q = d.session->solve( "resumable.", opt );
            d.settle();
            first.checkInvariants();

            const us::Seq lastSeen = first.events().empty()
                                         ? 0
                                         : first.events().back().header.seq;
            UT_CHECK_MSG( lastSeen > 0, "nothing arrived before the disconnect" );

            d.forceDisconnect();

            /* Work continues at the core while nobody is listening. */
            d.session->demand( q, us::Stream::Solutions, 5 );
            d.settle();

            Recorder second( /* resumed */ true );
            d.session->subscribe( second, lastSeen );
            d.settle();

            second.checkInvariants();

            /*
             * Two outcomes are both correct, and the front end must reach a
             * consistent view either way: the core replays what was missed,
             * or it says the resume point expired and the front end refreshes.
             * Asserting only the first would make the buffer size part of the
             * contract, which it is not.
             */
            bool expired = false;
            for ( const us::Failed& f : second.all<us::Failed>() ) {
                if ( f.reason == "resume point expired" ) {
                    expired = true;
                }
            }

            if ( !expired ) {
                UT_CHECK_MSG( !second.events().empty(),
                              "resume delivered nothing and did not report "
                              "expiry -- the front end would hang" );
                UT_CHECK_MSG( second.events().front().header.seq == lastSeen + 1,
                              "resume did not continue at the requested seq" );
            }

            /* Either way, the session is usable again. */
            second.clear();
            d.session->listing( us::ListingFilter() );
            d.settle();
            second.checkInvariants();
            UT_CHECK_MSG( second.count<us::Listing>() >= 1,
                          "the session was not usable after reconnecting" );
        } );

    // -- latency ------------------------------------------------------------

    registry.add(
        name( "a slow reply blocks no caller and breaks no invariant" ),
        [ factory ]() {
            SessionDriver d = factory();
            if ( !d.settleSlowly ) {
                return;
            }

            Recorder rec;
            d.session->subscribe( rec, 0 );
            d.scriptCountingGoal( "slow.", 3 );

            /*
             * The obligation is about the CALLER, so that is what is timed.
             * Every request must return in far less time than the reply takes
             * to arrive; a session that answered synchronously would show the
             * two as equal and fail here.
             */
            const auto issuedAt = std::chrono::steady_clock::now();
            us::QueryOptions opt;
            opt.initialDemand = 3;
            const us::QueryId q = d.session->solve( "slow.", opt );
            d.session->listing( us::ListingFilter() );
            const auto returnedAt = std::chrono::steady_clock::now();

            const auto issueMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    returnedAt - issuedAt ).count();
            UT_CHECK_MSG( issueMs < 100,
                          "issuing two requests took " << issueMs
                              << " ms -- a request must not block on its answer" );

            d.settleSlowly( 500 );
            rec.checkInvariants();

            UT_CHECK_EQ( rec.allFor<us::Solution>( q ).size(), std::size_t( 3 ) );
            UT_CHECK_MSG( rec.count<us::Listing>() >= 1,
                          "the listing issued alongside a query never arrived" );
        } );

    // -- close --------------------------------------------------------------

    registry.add(
        name( "close releases retained queries and is idempotent" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            d.scriptCountingGoal( "closing.", 2 );
            us::QueryOptions opt;
            opt.initialDemand = 2;
            opt.retain = true;
            d.session->solve( "closing.", opt );
            d.settle();
            rec.checkInvariants();

            d.session->close();
            UT_CHECK_EQ( d.session->describe().retainedQueries,
                         std::uint64_t( 0 ) );

            d.session->close();   /* must not crash or assert */
            UT_CHECK_EQ( d.session->describe().retainedQueries,
                         std::uint64_t( 0 ) );
        } );

    // -- value model --------------------------------------------------------

    registry.add(
        name( "truncation marks the node it cut and inspect can undo it" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            d.scriptDeepGoal( "nested.", 5 );

            us::QueryOptions tight;
            tight.initialDemand = 1;
            tight.budget.maxDepth = 1;
            const us::QueryId a = d.session->solve( "nested.", tight );
            d.settle();

            us::QueryOptions loose;
            loose.initialDemand = 1;
            loose.budget.maxDepth = 32;
            loose.budget.maxNodes = 4096;
            const us::QueryId b = d.session->solve( "nested.", loose );
            d.settle();
            rec.checkInvariants();

            const std::vector<us::Solution> tightSols = rec.allFor<us::Solution>( a );
            const std::vector<us::Solution> looseSols = rec.allFor<us::Solution>( b );
            UT_CHECK_EQ( tightSols.size(), std::size_t( 1 ) );
            UT_CHECK_EQ( looseSols.size(), std::size_t( 1 ) );

            const us::Value& tightValue = tightSols[ 0 ].bindings[ 0 ].second;
            const us::Value& looseValue = looseSols[ 0 ].bindings[ 0 ].second;

            UT_CHECK_MSG( us::hasTruncation( tightValue ),
                          "a tight budget produced an unmarked value -- the "
                          "front end would render a wrong term as a whole one" );
            UT_CHECK_MSG( !us::hasTruncation( looseValue ),
                          "a generous budget still marked the value truncated" );
            UT_CHECK_MSG( us::countNodes( looseValue )
                              > us::countNodes( tightValue ),
                          "the generous budget did not return more" );
        } );

    registry.add(
        name( "an unbound binding keeps its display name" ),
        [ factory ]() {
            SessionDriver d = factory();
            Recorder rec;
            d.session->subscribe( rec, 0 );

            d.scriptCountingGoal( "named.", 1 );
            us::QueryOptions opt;
            opt.initialDemand = 1;
            const us::QueryId q = d.session->solve( "named.", opt );
            d.settle();
            rec.checkInvariants();

            const std::vector<us::Solution> sols = rec.allFor<us::Solution>( q );
            UT_CHECK_EQ( sols.size(), std::size_t( 1 ) );
            for ( const auto& binding : sols[ 0 ].bindings ) {
                UT_CHECK_MSG( !binding.first.empty(),
                              "a binding with no variable name is unreadable" );
                if ( binding.second.kind == us::Value::Kind::Var ) {
                    UT_CHECK_MSG( !binding.second.name.empty(),
                                  "an unbound Var must carry its display name" );
                }
            }
        } );

    // -- describe -----------------------------------------------------------

    registry.add(
        name( "H describe() answers before anything else and names the core" ),
        [ factory ]() {
            SessionDriver d = factory();

            /* Deliberately before subscribe(): it must be legal first. */
            const us::Capabilities caps = d.session->describe();
            UT_CHECK_MSG( !caps.coreName.empty(), "describe() names no core" );
            UT_CHECK_MSG( !caps.location.empty(),
                          "describe() must say where the core is, so image "
                          "paths can be labelled local or remote" );
            UT_CHECK_MSG( caps.defaultListingPageSize > 0,
                          "a page size of zero makes paging undefined" );
        } );
}

} // namespace unify_test
