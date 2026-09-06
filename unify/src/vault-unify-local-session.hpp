#if !defined( _VAULT_UNIFY_LOCAL_SESSION_HPP )
#define _VAULT_UNIFY_LOCAL_SESSION_HPP

/**
 * @file vault-unify-local-session.hpp
 *
 * `Session` over an in-process engine -- plans/todo/lens ARCHITECTURE.md
 * section 2.1.
 *
 * This is the ONLY place in the tree that knows about `Clause`,
 * `UnifyContext`, `SolveJob` or `WorldPtr` on one side and the session
 * boundary on the other. Everything above it sees values.
 *
 * It could answer synchronously. It deliberately does not: requests are
 * posted to a session thread and every answer, diagnostic and solution
 * comes back on the same ordered event queue a socket would deliver. The
 * extra indirection is the price of the abstraction being real rather than
 * nominal, and it is what lets one contract suite run against this, against
 * FakeSession, and against a proxy.
 *
 * Three threads, one rule (ARCHITECTURE.md section 3):
 *
 *   caller thread   posts requests, never blocks, never touches engine state
 *   session thread  owns the RuntimeContext; executes requests; delivers events
 *   engine worker   Engine::executionLoop(), unchanged
 *
 * The rule is that the caller never blocks on the session. Note what is
 * NOT here as a consequence: no `waitForEngineIdle()`. That barrier is what
 * the current engine's thread-safety argument used to rest on, and dropping
 * it is what made engine items E14 and E16 prerequisites rather than
 * cleanups.
 */

#include "vault-unify-session.hpp"

#include <vault-unify.hpp>

#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vault {
namespace unify {

class FileDebugInfo;

namespace session {

class LocalSession
    : public Session
{
public:
    LocalSession();
    virtual ~LocalSession();

    /**
     * Create the world, the engine and the worker thread, and start the
     * session thread. Must be called exactly once, before anything else.
     *
     * Separate from the constructor because it can fail and because
     * RuntimeContext::setupDone() has to run before any member of it is
     * touched -- RuntimeContext has no constructor at all and leaves its
     * members uninitialised until then.
     */
    int start();

    /**
     * Block until every request posted so far has been executed, every job
     * they started has finished, and every event has been delivered.
     *
     * A HARNESS hook, not part of the boundary: the contract suite needs to
     * be able to say "let the core do all the work it can, then look", and
     * "sleep and hope" is not a test. It is deliberately not used by
     * LocalSession itself for anything -- a session that waited for engine
     * idle would be the design ARCHITECTURE.md section 3.1 rejects.
     */
    void waitUntilQuiet();

    // -- Session -----------------------------------------------------------

    virtual Capabilities describe() const;
    virtual void close();
    virtual void subscribe( EventSink& sink, Seq resumeFrom = 0 );

    virtual RequestId define( std::string text, Origin origin,
                              OverwritePolicy policy );
    virtual RequestId undefine( PredicateKey key, ModuleId scope );
    virtual RequestId listing( ListingFilter filter );
    virtual RequestId source( PredicateKey key );

    virtual QueryId   solve( std::string goalText, QueryOptions options );
    virtual RequestId demand( QueryId query, Stream stream, std::uint32_t n );
    virtual RequestId cancel( QueryId query );
    virtual RequestId release( QueryId query );
    virtual RequestId inspect( QueryId query, std::uint64_t solutionIndex,
                               ValuePath path, ValueBudget budget );

    virtual RequestId save  ( std::string path, SaveOptions options );
    virtual RequestId load  ( std::string path );
    virtual RequestId insert( std::string path, OverwritePolicy policy );

    virtual RequestId debug( DebugCommand command );

private:
    LocalSession( const LocalSession& );
    LocalSession& operator = ( const LocalSession& );

    /** One solution, already flattened out of the job's arena. */
    typedef std::vector<std::pair<std::string, Value>> Bindings;

    struct QueryState {
        QueryState() : produced( 0 ), delivered( 0 ), querySeq( 0 )
                     , finished( false ), terminal( false )
                     , retained( false ), released( false )
                     , cancelled( false ), errorCount( 0 ) {}

        std::string  goalText;
        QueryOptions options;

        /**
         * Every solution the job produced, copied out of the arena.
         *
         * This buffer is why `demand` is honest about being buffered rather
         * than throttling: performSlice() runs a query to completion in one
         * call and the solutions only become reachable inside onFinished,
         * after which ~SolveJob frees the whole arena. So the core computes
         * everything, copies it here, and `demand` drains it. The API is
         * right and the back-pressure is not yet real -- engine item E11,
         * and Capabilities::realDemand says so.
         */
        std::vector<Bindings> solutions;

        std::uint64_t produced;      //!< how many the job made
        std::uint64_t delivered;     //!< how many have been emitted
        std::uint64_t querySeq;

        bool finished;   //!< the job called back
        bool terminal;   //!< a terminal QueryStatus has been emitted
        bool retained;
        bool released;
        bool cancelled;

        std::uint32_t errorCount;
        std::string   failure;
    };

    // -- plumbing ----------------------------------------------------------

    void sessionLoop();
    void post( const std::function<void()>& work );
    void emit( const EventBody& body );
    void emitForQuery( QueryId query, const EventBody& body );

    /** Executed on the session thread. */
    void doDefine( RequestId req, std::string text, Origin origin,
                   OverwritePolicy policy );
    void doUndefine( RequestId req, PredicateKey key, ModuleId scope );
    void doListing( RequestId req, ListingFilter filter );
    void doSource( RequestId req, PredicateKey key );
    void doSolve( QueryId qid );
    void doDemand( RequestId req, QueryId qid, Stream stream, std::uint32_t n );
    void doCancel( RequestId req, QueryId qid );
    void doRelease( RequestId req, QueryId qid );
    void doInspect( RequestId req, QueryId qid, std::uint64_t index,
                    ValuePath path, ValueBudget budget );

    /** Emit up to `n` buffered solutions plus the resulting status. */
    void pump( QueryId qid, std::uint32_t n );

    /** Called on the ENGINE WORKER thread when a solve job finishes. */
    void onJobFinished( QueryId qid, boost::shared_ptr<Job> spJob );

    /** Called on the engine worker thread by the sinks. */
    void onEngineOutput( const std::string& strStream, const std::string& strText );
    void onEngineDiagnostic( const vault::unify::Diagnostic& diagnostic );

    /** The query the engine is currently running, or kNoQuery. */
    QueryId currentQuery() const;

    CatalogueEntry toCatalogueEntry( const vault::unify::CatalogueEntry& e ) const;
    PredicateKey   toPredicateKey( const vault::unify::PredicateKey& k ) const;

    class Sinks;
    friend class Sinks;

    mutable boost::mutex m_mutex;
    boost::condition_variable m_cond;      //!< work available / quiet reached

    RuntimeContext m_rt;
    bool           m_started;
    bool           m_closed;
    bool           m_stopping;

    vault::unify::FileDebugInfo* m_pDefineFileDebugInfo;
    Sinks*                       m_pSinks;

    boost::thread    m_sessionThread;
    boost::thread::id m_sessionThreadId;

    /**
     * The request currently being executed on the session thread, or
     * kNoRequest. Used to attribute parse diagnostics; see
     * onEngineDiagnostic().
     */
    RequestId m_currentRequest;

    std::deque<std::function<void()>> m_requests;
    std::deque<Event>                 m_events;

    EventSink* m_pSink;
    Seq        m_seq;

    RequestId m_nextRequest;
    QueryId   m_nextQuery;

    std::map<QueryId, QueryState> m_queries;

    /**
     * Queries submitted to the engine and not yet finished, in submission
     * order.
     *
     * The engine drains its job list strictly FIFO from a single worker, so
     * the front of this deque is the query whose job is running -- which is
     * how program output gets attributed. That inference is exactly as
     * sound as the single-FIFO-worker guarantee it rests on, which is why
     * the QueryId lives on the event header rather than being reconstructed
     * by the front end: when a query becomes a process, this deque is
     * replaced and nothing above the boundary changes.
     */
    std::deque<QueryId> m_running;

    /** Requests executed but whose engine work may still be outstanding. */
    std::uint64_t m_pendingJobs;
    bool          m_busy;   //!< the session thread is inside a request

    /**
     * True while an event is out at the sink.
     *
     * Distinct from the event queue being non-empty: the event is popped
     * before the callback runs, so "queue empty" is not "everything
     * delivered", and waitUntilQuiet() has to wait for the callback to
     * return or it hands the caller back control while still writing to it.
     */
    bool          m_delivering;
};

} // namespace session
} // namespace unify
} // namespace vault

#endif // _VAULT_UNIFY_LOCAL_SESSION_HPP
