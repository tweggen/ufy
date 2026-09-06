#if !defined( _UNIFY_TEST_CONTRACT_SUITE_HPP )
#define _UNIFY_TEST_CONTRACT_SUITE_HPP

/**
 * @file contract-suite.hpp
 *
 * The session contract suite -- deliverable D7, gates G0.3 through G0.11.
 *
 * One suite, several subjects. A boundary feature exists when it passes on
 * all of them; a case that needs to know which implementation it is running
 * against is testing an implementation, not the contract, and belongs
 * elsewhere.
 *
 * The suite is parameterised over a *driver* rather than over a Session,
 * because the obligations are not only about what a session answers but
 * about when: "delivers exactly 3 solutions and then stops" is only
 * assertable if the suite can say "let the core do as much work as it wants
 * to, then look". An in-process session, a fake and a socket each need a
 * different way to be told that, and the driver is that difference -- the
 * one place an implementation gets to be special.
 */

#include "test-harness.hpp"

#include "vault-unify-session.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace unify_test {

namespace us = vault::unify::session;

/**
 * Everything the suite needs from an implementation beyond the interface.
 */
struct SessionDriver {
    /** The session under test. */
    std::shared_ptr<us::Session> session;

    /**
     * Let the core do all the work it currently can, then return.
     *
     * For the fake this pumps the queue; for LocalSession it drains the
     * event queue until the engine goes quiet; for a proxy it round-trips a
     * ping. It must NOT be "sleep and hope", and it must not invent work
     * that the core would not otherwise do -- in particular it must never
     * satisfy demand the suite has not asked for.
     */
    std::function<void()> settle;

    /**
     * Script a goal that produces exactly `count` solutions, each binding
     * `$n` to 0..count-1. Every subject can arrange this; how differs.
     */
    std::function<void( const std::string& goalText, std::uint32_t count )>
        scriptCountingGoal;

    /**
     * Script a goal that emits `outputCount` Output events and
     * `diagnosticCount` Diagnostic events while producing `count` solutions.
     * Used by the attribution obligation.
     */
    std::function<void( const std::string& goalText,
                        std::uint32_t count,
                        std::uint32_t outputCount,
                        std::uint32_t diagnosticCount )>
        scriptNoisyGoal;

    /**
     * Script a goal whose single solution binds `$deep` to a value at least
     * `depth` levels deep, so truncation and inspect() have something to
     * work on.
     */
    std::function<void( const std::string& goalText, std::uint32_t depth )>
        scriptDeepGoal;

    /** Script `define` of this text to produce `count` diagnostics. */
    std::function<void( const std::string& text, std::uint32_t count )>
        scriptDefineError;

    /**
     * Text that `define` accepts, and the PredicateKey it should define.
     * Differs per subject because a real core actually parses it.
     */
    std::string        goodDefineText;
    us::PredicateKey   goodDefineKey;

    /**
     * Optional capabilities of the harness itself, as opposed to the core.
     * A subject that cannot drop its own connection skips the resume case
     * rather than pretending to test it -- and says so in the run output.
     */
    std::function<void()> forceDisconnect;   //!< null if unsupported

    /**
     * Make the next request answer Failed instead of doing its job.
     *
     * Null if the subject cannot arrange it. Note that this is a DEDICATED
     * case rather than a policy the whole suite runs under, and the
     * distinction is deliberate: reordering and delay must never change an
     * outcome, so the entire suite runs under them; injected failure changes
     * outcomes by definition, so a suite that ran under it would have to
     * weaken every assertion it makes into "either the right answer or a
     * Failed", which is not an assertion. ACCEPTANCE.md carries this as a
     * revision note against G0.4.
     */
    std::function<void()> injectFailure;     //!< null if unsupported

    /**
     * Settle with real wall-clock delay between deliveries -- at least
     * `ms` in total. Null if the subject cannot arrange it.
     */
    std::function<void( unsigned ms )> settleSlowly;
};

using DriverFactory = std::function<SessionDriver()>;

/**
 * Register every contract case against `factory` into `registry`.
 *
 * `subject` prefixes each case name, so one run reports
 * `fake/G0.6 initial demand is exact` alongside
 * `local/G0.6 initial demand is exact`.
 */
void registerContractSuite( Registry& registry,
                            const std::string& subject,
                            DriverFactory factory );

// ---------------------------------------------------------------------------
// A recording sink, shared by every case.
// ---------------------------------------------------------------------------

/**
 * Collects events and continuously checks the invariants that must hold for
 * EVERY event of EVERY case -- so they are asserted about eighty times per
 * run rather than in one test that could be deleted.
 */
class Recorder : public us::EventSink {
public:
    /**
     * `resumed` says this recorder attached with a non-zero `resumeFrom`, so
     * it is seeing a SUFFIX of the stream rather than the whole of it.
     *
     * The distinction is load-bearing, and getting it wrong is how a resume
     * bug hides: `querySeq` counts a query's events for the life of the
     * SESSION, not of a subscription, so a front end that reconnects mid-query
     * legitimately sees its first event at querySeq 7. A fresh subscriber
     * seeing 7 is a bug; a resumed one seeing 7 is the feature working. A
     * recorder that could not tell them apart would have to stop checking
     * either, which is how the invariant quietly stops being enforced.
     */
    explicit Recorder( bool resumed = false ) : m_resumed( resumed ) {}

    void onEvent( const us::Event& event ) override;

    /** Throws if any invariant was violated; call at the end of a case. */
    void checkInvariants() const;

    const std::vector<us::Event>& events() const { return m_events; }

    /** All events whose body is `T`. */
    template <typename T>
    std::vector<T> all() const
    {
        std::vector<T> out;
        for ( const us::Event& e : m_events ) {
            if ( const T* body = std::get_if<T>( &e.body ) ) {
                out.push_back( *body );
            }
        }
        return out;
    }

    /** All events whose body is `T` and whose header names `query`. */
    template <typename T>
    std::vector<T> allFor( us::QueryId query ) const
    {
        std::vector<T> out;
        for ( const us::Event& e : m_events ) {
            if ( !e.header.query || *e.header.query != query ) { continue; }
            if ( const T* body = std::get_if<T>( &e.body ) ) {
                out.push_back( *body );
            }
        }
        return out;
    }

    template <typename T>
    std::size_t count() const { return all<T>().size(); }

    /** Forget everything, but keep the invariant state (seq must continue). */
    void clear() { m_events.clear(); }

    std::string violation;   //!< non-empty once an invariant broke

private:
    std::vector<us::Event> m_events;
    us::Seq m_lastSeq = 0;
    std::map<us::QueryId, std::uint64_t> m_lastQuerySeq;
    std::map<us::QueryId, bool>          m_terminal;
    bool m_resumed = false;
};

} // namespace unify_test

#endif // _UNIFY_TEST_CONTRACT_SUITE_HPP
