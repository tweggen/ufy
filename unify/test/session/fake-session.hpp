#if !defined( _UNIFY_TEST_FAKE_SESSION_HPP )
#define _UNIFY_TEST_FAKE_SESSION_HPP

/**
 * @file fake-session.hpp
 *
 * A Session implementation with no engine behind it, whose purpose is to be
 * DIFFICULT.
 *
 * SESSION-API.md section 8 and ACCEPTANCE.md G0.4 ask for a fake that
 * "reorders independent replies, delays them up to 500 ms, injects Failed,
 * and disconnects mid-query", and that the same unmodified contract suite
 * passes against it. That is the whole point of the exercise: a front end
 * that quietly assumed in-process behaviour -- synchronous answers, replies
 * in request order, a connection that never drops -- fails here, in a test,
 * instead of failing in front of a user with a remote core.
 *
 * Two design choices worth defending:
 *
 *  1. **It is pumped, not threaded, by default.** Adversarial timing that
 *     depends on real sleeps produces a suite that is slow and flaky, and
 *     flaky adversarial tests get deleted. So misordering and delay are
 *     LOGICAL here: `pump()` executes pending work under a seeded policy, so
 *     "reply B overtook reply A" is reproducible from the seed and a failing
 *     run can be replayed exactly. A background-thread mode exists as well
 *     (`startBackgroundPump`) for the one obligation that genuinely needs
 *     wall-clock time -- "a 500 ms delayed reply hangs nothing".
 *
 *  2. **Reordering never crosses a stream.** Independent replies are
 *     interleaved freely, but two events of the same query, or two events of
 *     the same request, keep their order. That is not the fake being gentle:
 *     it is the contract (SESSION-API.md section 5 -- `querySeq` is gapless
 *     per query, and no Solution may follow its query's terminal status). A
 *     fake that violated it would be testing the front end against a core
 *     that is not allowed to exist.
 */

#include "vault-unify-session.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace unify_test {

namespace us = vault::unify::session;

/**
 * A scripted answer for one goal.
 *
 * `outputs` and `diagnostics` are emitted interleaved with the solutions, so
 * the attribution obligation (G0.8) has something to attribute.
 */
struct ScriptedGoal {
    std::vector<std::vector<std::pair<std::string, us::Value>>> solutions;
    std::vector<us::Output>     outputs;
    std::vector<us::Diagnostic> diagnostics;

    /** If set, the query ends Failed with this detail instead of Exhausted. */
    std::string failWith;

    /** If true, the goal never exhausts: demand always yields more. */
    bool infinite = false;
};

class FakeSession : public us::Session {
public:
    /**
     * How unpleasant to be. All of it is seeded and reproducible; none of it
     * is on by default, so a test opts into exactly the adversity it means
     * to assert against and a failure names one cause.
     */
    struct Policy {
        std::uint64_t seed = 1;

        /**
         * How many independent pending replies may overtake each other. 0 is
         * strict FIFO across all streams; 4 is enough to shuffle two or three
         * concurrent queries thoroughly.
         */
        std::uint32_t reorderWindow = 0;

        /** Answer every Nth request with Failed instead. 0 disables. */
        std::uint32_t failEveryNthRequest = 0;

        /**
         * Drop the connection once this many events have been delivered.
         * 0 disables. After the drop, no events are delivered until
         * subscribe() is called again.
         */
        std::uint64_t disconnectAfterEvents = 0;

        /** How far back subscribe(resumeFrom) can replay. */
        std::uint32_t resumeBufferEvents = 256;

        /** Reported by describe(); lets a test drive the honesty paths. */
        bool debug = true;
        bool structuredSolutions = true;
        bool retention = true;
        bool realDemand = false;
        bool realCancel = false;
        bool images = true;
    };

    /*
     * Two constructors rather than one with a defaulted argument: a default
     * argument of `Policy()` inside this class body is a complete-class
     * context, where Policy's own member initialisers are not yet usable.
     */
    FakeSession();
    explicit FakeSession( Policy policy );
    ~FakeSession() override;

    // -- scripting (test-facing; not part of the Session interface) --------

    /** Add a predicate to the fake catalogue. */
    void addPredicate( const us::PredicateKey& key,
                       const us::Origin& origin,
                       std::uint32_t clauseCount,
                       std::string sourceText );

    /** Script the answer to a goal, matched by exact text. */
    void scriptGoal( const std::string& goalText, ScriptedGoal goal );

    /**
     * Make `define` of this exact text fail to parse, producing the given
     * diagnostics AND a Defined with a matching errorCount -- never one
     * without the other.
     */
    void scriptDefineError( const std::string& text,
                            std::vector<us::Diagnostic> diagnostics );

    /** Convenience: a goal producing `count` solutions binding `$n` to 0..n-1. */
    void scriptCountingGoal( const std::string& goalText, std::uint32_t count );

    // -- driving -----------------------------------------------------------

    /**
     * Execute up to `maxActions` pending actions. Returns how many ran.
     *
     * Call it until it returns 0 to reach quiescence. A test that wants to
     * observe a query mid-flight pumps a bounded number instead.
     */
    std::size_t pump( std::size_t maxActions = ~std::size_t( 0 ) );

    /** True when nothing is pending. */
    bool idle() const;

    /**
     * Run a background thread that pumps one action every `perAction`, until
     * stopBackgroundPump(). Only for the wall-clock obligation; everything
     * else should use pump().
     */
    void startBackgroundPump( std::chrono::milliseconds perAction );
    void stopBackgroundPump();

    /** Events delivered so far, for assertions about the stream itself. */
    std::uint64_t deliveredEvents() const;

    /** True once the scripted disconnect has fired and not been resubscribed. */
    bool disconnected() const;

    /**
     * Answer the next request with Failed instead of doing its job.
     *
     * On-demand rather than via Policy::failEveryNthRequest, because the
     * contract suite needs to fail one named request and then assert the
     * session still works -- "every 7th request" cannot express that.
     */
    void failNextRequest();

    /** Drop the connection now, as if the transport had died. */
    void forceDisconnect();

    // -- Session -----------------------------------------------------------

    us::Capabilities describe() const override;
    void close() override;
    void subscribe( us::EventSink& sink, us::Seq resumeFrom = 0 ) override;

    us::RequestId define( std::string text, us::Origin origin,
                          us::OverwritePolicy policy ) override;
    us::RequestId undefine( us::PredicateKey key, us::ModuleId scope ) override;
    us::RequestId listing( us::ListingFilter filter ) override;
    us::RequestId source( us::PredicateKey key ) override;

    us::QueryId   solve( std::string goalText, us::QueryOptions options ) override;
    us::RequestId demand( us::QueryId query, us::Stream stream,
                          std::uint32_t n ) override;
    us::RequestId cancel( us::QueryId query ) override;
    us::RequestId release( us::QueryId query ) override;
    us::RequestId inspect( us::QueryId query, std::uint64_t solutionIndex,
                           us::ValuePath path, us::ValueBudget budget ) override;

    us::RequestId save  ( std::string path, us::SaveOptions options ) override;
    us::RequestId load  ( std::string path ) override;
    us::RequestId insert( std::string path, us::OverwritePolicy policy ) override;

    us::RequestId debug( us::DebugCommand command ) override;

private:
    /**
     * One unit of pending work: an event body to deliver, tagged with the
     * stream whose order it must not escape.
     */
    struct Action {
        std::uint64_t  streamKey;   //!< request or query it belongs to
        us::EventBody  body;
        std::optional<us::QueryId> query;
        /** Runs before the body is delivered; used for bookkeeping. */
        std::function<void()> before;
    };

    struct QueryState {
        std::string   goalText;
        ScriptedGoal  script;
        us::QueryOptions options;
        std::uint64_t produced = 0;      //!< solutions delivered
        std::uint64_t pendingDemand = 0;
        std::uint64_t querySeq = 0;
        bool          terminal = false;
        bool          retained = false;
        bool          released = false;
        /** Full, untruncated solutions, kept so inspect() can expand. */
        std::vector<std::vector<std::pair<std::string, us::Value>>> delivered;
    };

    struct CatalogueRow {
        us::CatalogueEntry entry;
        std::string        sourceText;
    };

    // -- internals ---------------------------------------------------------

    us::RequestId nextRequest();
    void   enqueue( Action action );
    void   deliver( const us::Event& event );
    bool   maybeFailRequest( us::RequestId req );
    void   emitStarted( us::RequestId req );
    void   pushSolutions( us::QueryId qid, std::uint32_t howMany );
    void   finishQuery( us::QueryId qid );
    std::size_t pumpLocked( std::size_t maxActions,
                            std::unique_lock<std::mutex>& lock );

    mutable std::mutex m_mutex;

    Policy         m_policy;
    std::mt19937_64 m_rng;

    us::EventSink* m_sink = nullptr;      //!< not owned; null when detached
    us::Seq        m_seq = 0;
    us::Gen        m_generation = 1;
    std::uint64_t  m_delivered = 0;
    bool           m_disconnected = false;
    bool           m_closed = false;

    us::RequestId  m_nextRequest = 1;
    us::QueryId    m_nextQuery = 1;
    std::uint32_t  m_requestCount = 0;
    bool           m_failNext = false;

    std::deque<Action> m_pending;
    std::vector<us::Event> m_replayBuffer;

    std::map<us::PredicateKey, CatalogueRow> m_catalogue;
    std::map<std::string, ScriptedGoal>      m_goals;
    std::map<std::string, std::vector<us::Diagnostic>> m_defineErrors;
    std::map<us::QueryId, QueryState>        m_queries;

    std::thread m_pumpThread;
    bool        m_pumpStop = false;
};

} // namespace unify_test

#endif // _UNIFY_TEST_FAKE_SESSION_HPP
