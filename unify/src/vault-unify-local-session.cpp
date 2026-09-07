/**
 * @file vault-unify-local-session.cpp
 *
 * See the header for the threading rules. The short version: public methods
 * run on the caller's thread and do nothing but allocate an id and post
 * work; everything else runs on the session thread; the engine worker only
 * ever pushes onto a queue.
 */

#include "vault-unify-local-session.hpp"
#include "vault-unify-session-value.hpp"

#include "vault-unify-debug.hpp"
#include "vault-unify-solvejob.hpp"

#include <algorithm>

namespace vault {
namespace unify {
namespace session {

namespace {

const char* const kSessionFileUri = "<session>";

/**
 * Wrap a goal in a `query { ... }` block unless it already is one.
 *
 * `solve` takes a goal, but the only way into the engine is the parser, and
 * the parser's entry point for "run this" is a query block. Rather than
 * building a Goal term tree by hand -- which would duplicate the parser and
 * get the desugaring wrong -- the text is wrapped and handed to the same
 * parseExecuteSegment() a file goes through.
 */
std::string wrapGoal( const std::string& strGoal )
{
    std::string strTrimmed = strGoal;
    const std::string::size_type first = strTrimmed.find_first_not_of( " \t\r\n" );
    if( std::string::npos == first ) {
        return "query { }\n";
    }
    strTrimmed = strTrimmed.substr( first );

    if( 0 == strTrimmed.compare( 0, 5, "query" ) ) {
        return strTrimmed + "\n";
    }

    std::string strBody = strTrimmed;
    const std::string::size_type last = strBody.find_last_not_of( " \t\r\n" );
    if( std::string::npos != last ) {
        strBody = strBody.substr( 0, last + 1 );
    }
    if( strBody.empty() || strBody[ strBody.size() - 1 ] != ';' ) {
        strBody += ";";
    }
    return "query {\n" + strBody + "\n}\n";
}

/**
 * True for variables the parser invents while desugaring (`$__0`, ...).
 *
 * They appear in a solution exactly like the ones the user typed, and are
 * an implementation detail of if/for/foreach lowering. unify-run's REPL
 * filters them out of its own output; a front end must not be handed them
 * either, or every `foreach` query grows noise bindings.
 */
bool isInternalVarName( const std::string& strName )
{
    return 0 == strName.compare( 0, 3, "$__" );
}

/*
 * A note on how thin the solutions are, since it is not obvious from the
 * code that flattens them below.
 *
 * Engine item E7 (structured solutions) is open: SolveJob::SolutionMap is
 * map<string,string>, so every binding arrives ALREADY RENDERED and the
 * best this adapter can produce is a Str leaf -- never a Cons, never a
 * Map, never a Var with its own name. That is a real degradation and it is
 * declared rather than hidden: Capabilities::structuredSolutions is false,
 * and inspect() therefore has nothing below the root to expand. When E7
 * lands, only the flattening in onJobFinished() and the inspector's
 * richness change; the wire format does not.
 */

} // namespace

// ---------------------------------------------------------------------------
// Sinks -- the engine's output and diagnostics, forwarded as events.
// ---------------------------------------------------------------------------

class LocalSession::Sinks
    : public OutputSink
    , public DiagnosticSink
{
public:
    explicit Sinks( LocalSession* pOwner ) : m_pOwner( pOwner ) {}

    virtual void onOutput( const std::string& strStream,
                           const std::string& strText )
    {
        m_pOwner->onEngineOutput( strStream, strText );
    }

    virtual void onDiagnostic( const vault::unify::Diagnostic& diagnostic )
    {
        m_pOwner->onEngineDiagnostic( diagnostic );
    }

private:
    LocalSession* m_pOwner;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

LocalSession::LocalSession()
    : m_started( false )
    , m_closed( false )
    , m_stopping( false )
    , m_pDefineFileDebugInfo( NULL )
    , m_pSinks( NULL )
    , m_currentRequest( kNoRequest )
    , m_pSink( NULL )
    , m_seq( 0 )
    , m_nextRequest( 1 )
    , m_nextQuery( 1 )
    , m_pendingJobs( 0 )
    , m_busy( false )
    , m_delivering( false )
{
}


LocalSession::~LocalSession()
{
    close();

    {
        Guard g( m_mutex );
        m_stopping = true;
        m_cond.notify_all();
    }
    if( m_sessionThread.joinable() ) {
        m_sessionThread.join();
    }

    if( m_pSinks ) {
        // Detach before the engine outlives us. RuntimeContext deliberately
        // leaks its Engine (no shutdown path, ROADMAP 5.1), so the Engine
        // WILL outlive this object and would otherwise hold a dangling sink.
        if( m_started ) {
            m_rt.getEngine()->setOutputSink( NULL );
            m_rt.getEngine()->setDiagnosticSink( NULL );
        }
        delete m_pSinks;
        m_pSinks = NULL;
    }
}


int LocalSession::start()
{
    if( m_started ) {
        return -EINVAL;
    }

    const int rc = m_rt.setupDone();
    if( 0 != rc ) {
        return rc;
    }
    m_started = true;

    m_pDefineFileDebugInfo = new vault::unify::FileDebugInfo( kSessionFileUri );
    m_rt.getWorld()->adoptFileDebugInfo( m_pDefineFileDebugInfo );

    m_pSinks = new Sinks( this );
    m_rt.getEngine()->setOutputSink( m_pSinks );
    m_rt.getEngine()->setDiagnosticSink( m_pSinks );

    m_sessionThread = boost::thread( &LocalSession::sessionLoop, this );
    m_sessionThreadId = m_sessionThread.get_id();
    return 0;
}


void LocalSession::sessionLoop()
{
    for( ;; ) {
        std::function<void()> work;
        Event event;
        bool haveWork = false;
        bool haveEvent = false;
        EventSink* pSink = NULL;

        {
            Guard g( m_mutex );
            while( !m_stopping && m_requests.empty() && m_events.empty() ) {
                m_cond.wait( g );
            }
            if( m_stopping ) {
                return;
            }

            // Events before requests: a front end that is behind should not
            // be made to fall further behind by new work, and delivering
            // first keeps the queue bounded.
            if( !m_events.empty() ) {
                event = m_events.front();
                m_events.pop_front();
                pSink = m_pSink;
                haveEvent = true;
                // Popping is not delivering. The sink callback runs below
                // with the lock released, so without this flag the queue
                // reads empty while an event is still being handed to the
                // front end -- and waitUntilQuiet() would return while the
                // caller's sink was still being written to. That is not a
                // theoretical gap: ThreadSanitizer reported the contract
                // suite's recorder being read by the test thread and
                // written by this one at the same moment.
                m_delivering = true;
            } else {
                work = m_requests.front();
                m_requests.pop_front();
                m_busy = true;
                haveWork = true;
            }
        }

        if( haveEvent ) {
            if( pSink ) {
                // Delivered WITHOUT the lock: a front end folding an event
                // and issuing a request from that fold is the normal case,
                // and holding the lock across the callback would deadlock
                // on it.
                pSink->onEvent( event );
            }
            // And notify AFTER, because draining the last event is a state
            // change someone may be waiting on. Omitting this deadlocks
            // waitUntilQuiet(): the queue goes empty inside the pop above,
            // with nothing to wake a waiter that had already checked.
            Guard g( m_mutex );
            m_delivering = false;
            m_cond.notify_all();
        }

        if( haveWork ) {
            work();
            Guard g( m_mutex );
            m_busy = false;
            m_cond.notify_all();
        }
    }
}


void LocalSession::post( const std::function<void()>& work )
{
    Guard g( m_mutex );
    if( m_closed ) {
        return;
    }
    m_requests.push_back( work );
    m_cond.notify_all();
}


void LocalSession::emit( const EventBody& body )
{
    Guard g( m_mutex );

    Event event;
    event.header.seq = ++m_seq;
    event.header.worldGeneration =
        m_started ? m_rt.getWorld()->currentGeneration() : 0;
    event.body = body;

    m_events.push_back( event );
    m_cond.notify_all();
}


void LocalSession::emitForQuery( QueryId query, const EventBody& body )
{
    Guard g( m_mutex );

    Event event;
    event.header.seq = ++m_seq;
    event.header.worldGeneration =
        m_started ? m_rt.getWorld()->currentGeneration() : 0;
    event.header.query = query;
    event.header.querySeq = ++m_queries[ query ].querySeq;
    event.body = body;

    m_events.push_back( event );
    m_cond.notify_all();
}


void LocalSession::waitUntilQuiet()
{
    for( ;; ) {
        {
            Guard g( m_mutex );
            while( !m_requests.empty() || m_busy || m_delivering ) {
                m_cond.wait( g );
            }
        }

        // A barrier job: queued behind everything already submitted, and
        // therefore finished only once they are. This is the one place the
        // barrier trick is used, and it is a harness hook -- see the
        // header for why the session itself must never do this.
        if( m_started ) {
            boost::mutex waitMutex;
            boost::condition_variable waitCond;
            bool barrierDone = false;

            vault::unify::SolveJob* pBarrier = new vault::unify::SolveJob();
            pBarrier->setWorld( m_rt.getWorld() );
            {
                vault::unify::Goal* pBarrierGoal = new vault::unify::Goal();
                pBarrier->setGoal( pBarrierGoal );
                pBarrier->adoptGoal( pBarrierGoal );
            }
            pBarrier->onFinished(
                [&waitMutex, &waitCond, &barrierDone](
                        boost::shared_ptr<vault::unify::Job> ) {
                    vault::unify::Guard g( waitMutex );
                    barrierDone = true;
                    waitCond.notify_one();
                } );
            pBarrier->startJob( m_rt.getEngine() );
            m_rt.getEngine()->addJob(
                boost::shared_ptr<vault::unify::Job>( pBarrier ) );

            vault::unify::Guard g( waitMutex );
            while( !barrierDone ) {
                waitCond.wait( g );
            }
        }

        {
            Guard g( m_mutex );
            // A finished job posts events, and those events may post more
            // requests; loop until everything really has drained.
            if( m_requests.empty() && m_events.empty()
                && !m_busy && !m_delivering && 0 == m_pendingJobs ) {
                return;
            }
            while( !m_events.empty() || m_busy || m_delivering ) {
                m_cond.wait( g );
            }
        }
    }
}


void LocalSession::close()
{
    Guard g( m_mutex );
    if( m_closed ) {
        return;
    }
    m_closed = true;
    m_pSink = NULL;
    m_requests.clear();
    m_events.clear();

    for( std::map<QueryId, QueryState>::iterator it = m_queries.begin();
         it != m_queries.end(); ++it ) {
        it->second.retained = false;
        it->second.released = true;
        it->second.solutions.clear();
    }
}


void LocalSession::subscribe( EventSink& sink, Seq resumeFrom )
{
    Guard g( m_mutex );
    m_pSink = &sink;

    if( 0 != resumeFrom ) {
        // No replay buffer in v1: an in-process session cannot lose its
        // connection, so the honest answer to "resume from N" is that the
        // resume point is gone. The front end's recovery is a full refresh,
        // which is a supported path -- see the contract suite.
        Event event;
        event.header.seq = ++m_seq;
        event.body = Failed{ kNoRequest, "resume point expired", false };
        m_events.push_back( event );
        m_cond.notify_all();
    }
}

// ---------------------------------------------------------------------------
// describe
// ---------------------------------------------------------------------------

Capabilities LocalSession::describe() const
{
    Guard g( m_mutex );

    Capabilities caps;
    caps.coreName = "vault-unify-core";
    caps.coreVersion = "0";
    caps.location = "local";

    // Stated as they actually are, not as we would like them. Every one of
    // these falses is an engine item the plan names, and a front end reads
    // them to decide what to promise the user.
    caps.debug = false;                 // xdebug not wired to this boundary
    caps.structuredSolutions = false;   // E7: SolutionMap is map<string,string>
    caps.realDemand = false;            // E11: solutions materialise at once
    caps.realCancel = false;            // E8: no bounded slices, no stop
    caps.images = false;                // E5/E6/E9/E12/E13
    caps.retention = true;              // solutions are copied out and kept

    if( m_started ) {
        std::vector<vault::unify::CatalogueEntry> entries;
        // const_cast: copyCatalogue takes the clause-db lock and so cannot
        // be const, but reading the catalogue is logically a const
        // operation and describe() is const by contract.
        const_cast<LocalSession*>( this )->m_rt.getWorld()
            ->copyCatalogue( entries );
        for( size_t i = 0; i < entries.size(); ++i ) {
            if( entries[i].kind == vault::unify::ClauseOrigin::BUILTIN ) {
                caps.builtins.push_back( toPredicateKey( entries[i].key ) );
            }
        }
    }

    for( std::map<QueryId, QueryState>::const_iterator it = m_queries.begin();
         it != m_queries.end(); ++it ) {
        if( it->second.retained && !it->second.released ) {
            ++caps.retainedQueries;
        }
    }
    caps.liveRequests = m_requests.size();

    return caps;
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

PredicateKey LocalSession::toPredicateKey(
        const vault::unify::PredicateKey& k ) const
{
    PredicateKey key;
    key.name = k.name;
    key.arity = (std::uint32_t) k.arity;
    key.module = (ModuleId) k.module;
    return key;
}


CatalogueEntry LocalSession::toCatalogueEntry(
        const vault::unify::CatalogueEntry& e ) const
{
    CatalogueEntry entry;
    entry.key = toPredicateKey( e.key );
    entry.clauseCount = e.clauseCount;
    entry.generation = e.generation;

    switch( e.kind ) {
    case vault::unify::ClauseOrigin::MODULE:
        entry.origin.kind = Origin::Kind::Module; break;
    case vault::unify::ClauseOrigin::ASSERTED:
        entry.origin.kind = Origin::Kind::Asserted; break;
    case vault::unify::ClauseOrigin::BUILTIN:
        entry.origin.kind = Origin::Kind::Builtin; break;
    case vault::unify::ClauseOrigin::SYNTHESIZED:
        entry.origin.kind = Origin::Kind::Synthesized; break;
    case vault::unify::ClauseOrigin::TRANSCRIPT:
        entry.origin.kind = Origin::Kind::Transcript; break;
    }

    entry.origin.module = (ModuleId) e.key.module;
    entry.origin.file = e.uriFile;
    entry.origin.line = (std::uint32_t) e.firstLine;
    return entry;
}

// ---------------------------------------------------------------------------
// Engine callbacks (worker thread)
// ---------------------------------------------------------------------------

QueryId LocalSession::currentQuery() const
{
    // Caller holds m_mutex.
    return m_running.empty() ? kNoQuery : m_running.front();
}


void LocalSession::onEngineOutput( const std::string& strStream,
                                   const std::string& strText )
{
    QueryId qid = kNoQuery;
    {
        Guard g( m_mutex );
        qid = currentQuery();
    }

    Output out;
    out.stream = strStream;
    out.text = strText;

    if( kNoQuery != qid ) {
        emitForQuery( qid, out );
    } else {
        emit( out );
    }
}


void LocalSession::onEngineDiagnostic( const vault::unify::Diagnostic& d )
{
    /*
     * Two sources, two attributions, told apart by which thread we are on.
     *
     * A parse diagnostic is produced synchronously inside
     * parseExecuteSegment() on the SESSION thread, and belongs to the
     * request that asked for the parse. A runtime diagnostic comes from a
     * job on the ENGINE WORKER thread, and belongs to the query that is
     * running. Guessing from content would be fragile; the thread is a
     * fact.
     */
    const bool onSessionThread =
        ( boost::this_thread::get_id() == m_sessionThreadId );

    Diagnostic diag;
    switch( d.severity ) {
    case vault::unify::Diagnostic::NOTE:    diag.sev = Severity::Note; break;
    case vault::unify::Diagnostic::WARNING: diag.sev = Severity::Warning; break;
    case vault::unify::Diagnostic::ERROR:   diag.sev = Severity::Error; break;
    }
    diag.file = d.uriFile;
    diag.line = (std::uint32_t) d.line;
    diag.column = (std::uint32_t) d.column;
    diag.message = d.message;
    diag.sourceLine = d.sourceLine;

    QueryId qid = kNoQuery;
    RequestId req = kNoRequest;
    {
        Guard g( m_mutex );
        if( onSessionThread ) {
            req = m_currentRequest;
        } else {
            qid = currentQuery();
        }
    }

    if( kNoRequest != req ) {
        diag.req = req;
        emit( diag );
    } else if( kNoQuery != qid ) {
        emitForQuery( qid, diag );
    } else {
        emit( diag );
    }
}


void LocalSession::onJobFinished( QueryId qid, boost::shared_ptr<Job> spJob )
{
    vault::unify::SolveJob* pSolveJob =
        dynamic_cast<vault::unify::SolveJob*>( spJob.get() );

    std::vector<Bindings> solutions;
    std::uint32_t errorCount = 0;

    if( pSolveJob ) {
        // EVERYTHING must be copied here. Engine::executionLoop() drops the
        // last reference to the job immediately after this callback returns
        // and ~SolveJob then frees the whole arena, including the term
        // trees the solutions were rendered from. Stashing the job and
        // reading it later is use-after-free.
        errorCount = (std::uint32_t) pSolveJob->getErrorCount();

        vault::unify::SolveJob::SolutionListPtr spSolutions =
            pSolveJob->getSolutionList();
        if( spSolutions ) {
            vault::unify::SolveJob::SolutionList::const_iterator it;
            for( it = spSolutions->begin(); it != spSolutions->end(); ++it ) {
                const vault::unify::SolveJob::SolutionMapPtr& spMap = *it;
                if( !spMap ) {
                    continue;
                }
                Bindings bindings;
                vault::unify::SolveJob::SolutionMap::const_iterator itVar;
                for( itVar = spMap->begin(); itVar != spMap->end(); ++itVar ) {
                    if( isInternalVarName( itVar->first ) ) {
                        continue;
                    }
                    Value v;
                    v.kind = Value::Kind::Str;
                    v.name = itVar->second;
                    bindings.push_back( std::make_pair( itVar->first, v ) );
                }
                solutions.push_back( bindings );
            }
        }
    }

    {
        Guard g( m_mutex );

        std::deque<QueryId>::iterator itRunning =
            std::find( m_running.begin(), m_running.end(), qid );
        if( itRunning != m_running.end() ) {
            m_running.erase( itRunning );
        }

        std::map<QueryId, QueryState>::iterator it = m_queries.find( qid );
        if( it != m_queries.end() ) {
            it->second.solutions = solutions;
            it->second.produced = solutions.size();
            it->second.errorCount = errorCount;
            it->second.finished = true;
        }

        if( m_pendingJobs > 0 ) {
            --m_pendingJobs;
        }
        m_cond.notify_all();
    }

    // Deliver the first batch, if one was asked for.
    QueryId captured = qid;
    post( [this, captured]() {
        std::uint32_t initial = 0;
        {
            Guard g( m_mutex );
            std::map<QueryId, QueryState>::const_iterator it =
                m_queries.find( captured );
            if( it == m_queries.end() ) {
                return;
            }
            initial = it->second.options.initialDemand;
        }
        pump( captured, initial );
    } );
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

RequestId LocalSession::define( std::string text, Origin origin,
                                OverwritePolicy policy )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    post( [this, req, text, origin, policy]() {
        doDefine( req, text, origin, policy );
    } );
    return req;
}


void LocalSession::doDefine( RequestId req, std::string text, Origin origin,
                             OverwritePolicy policy )
{
    if( OverwritePolicy::ReplacePredicates == policy
        || OverwritePolicy::Fail == policy ) {
        // Engine item E3 (predicate-scoped retract) is not done, so
        // "replace" cannot be honoured. Refusing is the only honest answer:
        // silently appending would give the user two definitions where they
        // asked for one, which is worse than an error.
        emit( Failed{ req,
            "overwrite policy needs engine item E3 (predicate-scoped retract)",
            false } );
        return;
    }

    std::vector<vault::unify::CatalogueEntry> before;
    m_rt.getWorld()->copyCatalogue( before );

    vault::unify::ClauseOrigin::Kind kind = vault::unify::ClauseOrigin::MODULE;
    if( Origin::Kind::Transcript == origin.kind ) {
        kind = vault::unify::ClauseOrigin::TRANSCRIPT;
    }

    /*
     * Deliberately NOT counted in m_pendingJobs. Defined text may contain
     * `query { ... }` blocks, and parseExecuteSegment() gives no way to know
     * how many jobs it will start before it starts them -- so an increment
     * here could not be balanced against the callbacks, and would corrupt
     * the count a concurrent solve depends on. waitUntilQuiet()'s barrier
     * job is what actually establishes that these have finished: it is
     * queued behind them and the engine drains FIFO.
     */
    {
        Guard g( m_mutex );
        m_currentRequest = req;
    }

    const int errorCount = m_rt.parseExecuteSegment(
        text.begin(), text.end(),
        [this]( boost::shared_ptr<vault::unify::Job> ) {
            Guard g( m_mutex );
            m_cond.notify_all();
        },
        m_pDefineFileDebugInfo, kind );

    {
        Guard g( m_mutex );
        m_currentRequest = kNoRequest;
    }

    std::vector<vault::unify::CatalogueEntry> after;
    m_rt.getWorld()->copyCatalogue( after );

    Defined defined;
    defined.req = req;
    defined.module = origin.module;
    defined.errorCount = (std::uint32_t) errorCount;

    // Diff the catalogue rather than asking the parser what it did: the
    // parser reports a count, and a clause it desugars produces predicates
    // nobody named in the text. The catalogue is the ground truth for what
    // the database now holds.
    std::map<vault::unify::PredicateKey, std::uint32_t> countBefore;
    for( size_t i = 0; i < before.size(); ++i ) {
        countBefore[ before[i].key ] = before[i].clauseCount;
    }
    for( size_t i = 0; i < after.size(); ++i ) {
        if( after[i].kind == vault::unify::ClauseOrigin::BUILTIN
            || after[i].kind == vault::unify::ClauseOrigin::SYNTHESIZED ) {
            continue;
        }
        std::map<vault::unify::PredicateKey, std::uint32_t>::const_iterator
            itBefore = countBefore.find( after[i].key );
        if( itBefore == countBefore.end()
            || itBefore->second != after[i].clauseCount ) {
            /*
             * `added`, not `replaced`, even when the predicate already
             * existed. Under OverwritePolicy::Append a second `colour( x );`
             * ADDS a clause -- reporting that as "replaced" tells the user
             * their first clause is gone, which is the opposite of what
             * happened. `replaced` is reserved for ReplacePredicates, which
             * needs engine item E3 and currently answers Failed rather than
             * pretending.
             */
            defined.added.push_back( toPredicateKey( after[i].key ) );
        }
    }

    emit( defined );

    if( 0 == errorCount ) {
        WorldChanged changed;
        changed.generation = m_rt.getWorld()->currentGeneration();
        changed.predicateCount = (std::uint32_t) after.size();
        for( size_t i = 0; i < after.size(); ++i ) {
            changed.clauseCount += after[i].clauseCount;
        }
        emit( changed );
    }
}


RequestId LocalSession::undefine( PredicateKey key, ModuleId scope )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    post( [this, req, key, scope]() { doUndefine( req, key, scope ); } );
    return req;
}


void LocalSession::doUndefine( RequestId req, PredicateKey, ModuleId )
{
    emit( Failed{ req,
        "undefine needs engine item E3 (predicate-scoped retract)", false } );
}


RequestId LocalSession::listing( ListingFilter filter )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    post( [this, req, filter]() { doListing( req, filter ); } );
    return req;
}


void LocalSession::doListing( RequestId req, ListingFilter filter )
{
    std::vector<vault::unify::CatalogueEntry> entries;
    m_rt.getWorld()->copyCatalogue( entries );

    std::vector<CatalogueEntry> matched;
    for( size_t i = 0; i < entries.size(); ++i ) {
        const vault::unify::CatalogueEntry& e = entries[i];

        const bool isBuiltin = e.kind == vault::unify::ClauseOrigin::BUILTIN;
        const bool isSynth = e.kind == vault::unify::ClauseOrigin::SYNTHESIZED;
        if( isBuiltin && !filter.includeBuiltins ) { continue; }
        if( isSynth && !filter.includeSynthesized ) { continue; }

        if( filter.module && *filter.module != (ModuleId) e.key.module ) {
            continue;
        }
        if( filter.arity && *filter.arity != (std::uint32_t) e.key.arity ) {
            continue;
        }
        if( !filter.namePrefix.empty()
            && e.key.name.compare( 0, filter.namePrefix.size(),
                                   filter.namePrefix ) != 0 ) {
            continue;
        }
        matched.push_back( toCatalogueEntry( e ) );
    }

    Listing listing;
    listing.req = req;

    const size_t offset = filter.offset;
    const size_t limit = filter.limit ? filter.limit : matched.size();
    for( size_t i = offset; i < matched.size() && listing.entries.size() < limit;
         ++i ) {
        listing.entries.push_back( matched[i] );
    }
    listing.more = ( offset + listing.entries.size() ) < matched.size();

    emit( listing );
}


RequestId LocalSession::source( PredicateKey key )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    post( [this, req, key]() { doSource( req, key ); } );
    return req;
}


void LocalSession::doSource( RequestId req, PredicateKey key )
{
    vault::unify::PredicateKey engineKey;
    engineKey.name = key.name;
    engineKey.arity = (int) key.arity;
    engineKey.module = (vault::unify::ModuleId) key.module;

    vault::unify::CatalogueEntry entry;
    if( !m_rt.getWorld()->findCatalogueEntry( engineKey, entry ) ) {
        emit( Failed{ req, "no such predicate", false } );
        return;
    }

    /*
     * Engine item E5 (module source retention) is not done, so there is no
     * stored source text and this reconstructs a listing from the clauses
     * instead -- Clause::toString(), one per line, in definition order.
     *
     * That is not the same thing, and the difference is visible: `for`,
     * `foreach` and `if` desugar at PARSE time, so a rule that used one
     * comes back referring to a `__fe__N` predicate rather than showing the
     * loop the user wrote. Until E5 lands, a source panel built on this
     * shows the database's view of a definition, not the program's.
     */
    std::string strText;
    vault::unify::ExecutionState* pRoot = m_rt.getWorld()->getRootState();
    vault::unify::ExecutionState::ClauseIterator it = pRoot->clauseIterator();
    for( ; it.isValid(); it.next() ) {
        const vault::unify::Clause* pClause = it.getClause();
        if( !pClause || !pClause->leftHandTerm() ) { continue; }
        if( pClause->leftHandTerm()->getName().value() != key.name ) { continue; }
        if( (std::uint32_t) pClause->leftHandTerm()->getArity() != key.arity ) {
            continue;
        }
        strText += pClause->toString();
        strText += "\n";
    }

    SourceText text;
    text.req = req;
    text.key = key;
    text.text = strText;
    text.origin = toCatalogueEntry( entry ).origin;
    emit( text );
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

QueryId LocalSession::solve( std::string goalText, QueryOptions options )
{
    QueryId qid;
    {
        Guard g( m_mutex );
        qid = m_nextQuery++;
        QueryState state;
        state.goalText = goalText;
        state.options = options;
        m_queries[ qid ] = state;
    }
    post( [this, qid]() { doSolve( qid ); } );
    return qid;
}


void LocalSession::doSolve( QueryId qid )
{
    std::string strGoal;
    {
        Guard g( m_mutex );
        std::map<QueryId, QueryState>::const_iterator it = m_queries.find( qid );
        if( it == m_queries.end() ) {
            return;
        }
        strGoal = it->second.goalText;
        ++m_pendingJobs;
        m_running.push_back( qid );
    }

    const std::string strSegment = wrapGoal( strGoal );

    const int errorCount = m_rt.parseExecuteSegment(
        strSegment.begin(), strSegment.end(),
        [this, qid]( boost::shared_ptr<vault::unify::Job> spJob ) {
            onJobFinished( qid, spJob );
        },
        m_pDefineFileDebugInfo, vault::unify::ClauseOrigin::TRANSCRIPT );

    if( 0 != errorCount ) {
        // The goal did not parse, so no job was ever queued and no callback
        // will arrive; unwind the bookkeeping and report a terminal status.
        // The Diagnostic itself has already been emitted by the sink.
        {
            Guard g( m_mutex );
            std::deque<QueryId>::iterator itRunning =
                std::find( m_running.begin(), m_running.end(), qid );
            if( itRunning != m_running.end() ) {
                m_running.erase( itRunning );
            }
            if( m_pendingJobs > 0 ) { --m_pendingJobs; }
            std::map<QueryId, QueryState>::iterator it = m_queries.find( qid );
            if( it != m_queries.end() ) {
                it->second.finished = true;
                it->second.terminal = true;
                it->second.errorCount = (std::uint32_t) errorCount;
            }
            m_cond.notify_all();
        }

        QueryStatus status;
        status.state = QueryStatus::State::Failed;
        status.produced = 0;
        status.retained = false;
        status.detail = "the goal did not parse";
        emitForQuery( qid, status );
    }
}


void LocalSession::pump( QueryId qid, std::uint32_t n )
{
    std::vector<Bindings> toEmit;
    QueryStatus status;
    bool emitStatus = false;
    std::uint64_t firstIndex = 0;
    ValueBudget budget;

    {
        Guard g( m_mutex );
        std::map<QueryId, QueryState>::iterator it = m_queries.find( qid );
        if( it == m_queries.end() ) {
            return;
        }
        QueryState& q = it->second;
        if( q.terminal ) {
            return;
        }
        if( !q.finished ) {
            // The job has not called back yet. Nothing to deliver and
            // nothing to say -- the eventual callback will pump again.
            return;
        }

        const std::uint64_t available = q.solutions.size() - q.delivered;
        const std::uint64_t take = std::min<std::uint64_t>( n, available );
        firstIndex = q.delivered;
        budget = q.options.budget;
        for( std::uint64_t i = 0; i < take; ++i ) {
            toEmit.push_back( q.solutions[ (size_t)( q.delivered + i ) ] );
        }
        q.delivered += take;

        status.produced = q.delivered;
        if( q.delivered >= q.solutions.size() ) {
            q.terminal = true;
            q.retained = q.options.retain;
            status.state = q.errorCount > 0
                ? QueryStatus::State::Failed
                : QueryStatus::State::Exhausted;
            if( q.errorCount > 0 ) {
                status.detail = "the goal reported errors while solving";
            }
        } else {
            status.state = QueryStatus::State::Running;
        }
        status.retained = q.retained;
        emitStatus = true;
    }

    for( size_t i = 0; i < toEmit.size(); ++i ) {
        Solution solution;
        solution.index = firstIndex + i;
        for( size_t b = 0; b < toEmit[i].size(); ++b ) {
            solution.bindings.push_back( std::make_pair(
                toEmit[i][b].first,
                applyBudget( toEmit[i][b].second, budget ) ) );
        }
        emitForQuery( qid, solution );
    }

    if( emitStatus ) {
        emitForQuery( qid, status );
    }
}


RequestId LocalSession::demand( QueryId query, Stream stream, std::uint32_t n )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    post( [this, req, query, stream, n]() { doDemand( req, query, stream, n ); } );
    return req;
}


void LocalSession::doDemand( RequestId req, QueryId qid, Stream stream,
                             std::uint32_t n )
{
    bool known = false;
    {
        Guard g( m_mutex );
        known = m_queries.find( qid ) != m_queries.end();
    }
    if( !known ) {
        emit( Failed{ req, "no such query", false } );
        return;
    }

    if( Stream::Trace == stream ) {
        emit( Failed{ req, "trace needs the xdebug backend (not wired)", false } );
        return;
    }

    pump( qid, n );
}


RequestId LocalSession::cancel( QueryId query )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    post( [this, req, query]() { doCancel( req, query ); } );
    return req;
}


void LocalSession::doCancel( RequestId req, QueryId qid )
{
    QueryStatus status;
    bool found = false;

    {
        Guard g( m_mutex );
        std::map<QueryId, QueryState>::iterator it = m_queries.find( qid );
        if( it != m_queries.end() ) {
            found = true;
            QueryState& q = it->second;
            status.produced = q.delivered;
            if( q.terminal ) {
                // Already finished. Cancel still answers with a terminal
                // status -- that is what lets a front end detach
                // unconditionally instead of racing the engine -- so it
                // restates the outcome rather than inventing an Aborted.
                status.state = QueryStatus::State::Exhausted;
            } else {
                status.state = QueryStatus::State::Aborted;
                q.terminal = true;
                q.cancelled = true;
                q.retained = q.options.retain && q.finished;
            }
            status.retained = q.retained;
        }
    }

    if( !found ) {
        emit( Failed{ req, "no such query", false } );
        return;
    }

    // Best effort, and the front end is told so in as many words. The
    // engine has one worker, no shutdown path, and performSlice() runs a
    // query to completion in a single call, so a running goal keeps running
    // until the process exits. Saying otherwise would be shipping a control
    // that lies (plan section 6); when ROADMAP Phase 3's bounded slices
    // land this becomes real with no change above this line.
    status.detail = "detached; the engine cannot stop a running goal yet";
    emitForQuery( qid, status );
}


RequestId LocalSession::release( QueryId query )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    post( [this, req, query]() { doRelease( req, query ); } );
    return req;
}


void LocalSession::doRelease( RequestId, QueryId qid )
{
    Guard g( m_mutex );
    std::map<QueryId, QueryState>::iterator it = m_queries.find( qid );
    if( it != m_queries.end() ) {
        it->second.retained = false;
        it->second.released = true;
        it->second.solutions.clear();
    }
}


RequestId LocalSession::inspect( QueryId query, std::uint64_t solutionIndex,
                                 ValuePath path, ValueBudget budget )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    post( [this, req, query, solutionIndex, path, budget]() {
        doInspect( req, query, solutionIndex, path, budget );
    } );
    return req;
}


void LocalSession::doInspect( RequestId req, QueryId qid, std::uint64_t index,
                              ValuePath path, ValueBudget budget )
{
    Bindings bindings;
    bool available = false;
    bool haveSolution = false;

    {
        Guard g( m_mutex );
        std::map<QueryId, QueryState>::const_iterator it = m_queries.find( qid );
        available = ( it != m_queries.end() ) && !it->second.released;
        if( available && index < it->second.solutions.size() ) {
            bindings = it->second.solutions[ (size_t) index ];
            haveSolution = true;
        }
    }

    if( !available ) {
        emit( Failed{ req, "query released", false } );
        return;
    }
    if( !haveSolution ) {
        emit( Failed{ req, "no such solution", false } );
        return;
    }
    if( path.empty() ) {
        emit( Failed{ req, "path must name a binding", false } );
        return;
    }
    if( path.front() >= bindings.size() ) {
        emit( Failed{ req, "no such binding", false } );
        return;
    }

    const ValuePath rest( path.begin() + 1, path.end() );
    const std::optional<Value> node =
        valueAtPath( bindings[ path.front() ].second, rest );
    if( !node ) {
        emit( Failed{ req, "path leaves the value", false } );
        return;
    }

    Expanded expanded;
    expanded.req = req;
    expanded.path = path;
    expanded.value = applyBudget( *node, budget );
    emit( expanded );
}

// ---------------------------------------------------------------------------
// Image and debug -- declared, honestly unimplemented
// ---------------------------------------------------------------------------

RequestId LocalSession::save( std::string, SaveOptions )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    emit( Failed{ req, "images need engine items E5, E6 and E13", false } );
    return req;
}


RequestId LocalSession::load( std::string )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    emit( Failed{ req, "load needs engine item E9 (world reset)", false } );
    return req;
}


RequestId LocalSession::insert( std::string, OverwritePolicy )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    emit( Failed{ req, "insert needs engine item E12 (staged parse)", false } );
    return req;
}


RequestId LocalSession::debug( DebugCommand )
{
    RequestId req;
    {
        Guard g( m_mutex );
        req = m_nextRequest++;
    }
    emit( Started{ req } );
    // describe() reports debug:false, and this is the other half of that
    // promise: answer Failed, never crash.
    emit( Failed{ req, "debug backend is not wired to this boundary", false } );
    return req;
}

} // namespace session
} // namespace unify
} // namespace vault
