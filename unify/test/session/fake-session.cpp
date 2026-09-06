/**
 * @file fake-session.cpp
 *
 * Implementation of the adversarial fake. See the header for why it is
 * pumped rather than threaded, and why its misordering stops at a stream
 * boundary.
 */

#include "fake-session.hpp"

#include "vault-unify-session-value.hpp"

#include <algorithm>
#include <set>

namespace unify_test {

namespace {

/**
 * Stream keys. Requests and queries share one number space here, so the
 * top bit separates them; nothing outside this file ever sees the value.
 */
constexpr std::uint64_t kQueryStreamBit = std::uint64_t( 1 ) << 63;

std::uint64_t requestStream( us::RequestId req ) { return req; }
std::uint64_t queryStream( us::QueryId qid ) { return kQueryStreamBit | qid; }

} // namespace

FakeSession::FakeSession()
    : FakeSession( Policy() )
{
}

FakeSession::FakeSession( Policy policy )
    : m_policy( policy )
    , m_rng( policy.seed )
{
}

FakeSession::~FakeSession()
{
    stopBackgroundPump();
}

// ---------------------------------------------------------------------------
// Scripting
// ---------------------------------------------------------------------------

void FakeSession::addPredicate( const us::PredicateKey& key,
                                const us::Origin& origin,
                                std::uint32_t clauseCount,
                                std::string sourceText )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    CatalogueRow row;
    row.entry.key = key;
    row.entry.origin = origin;
    row.entry.clauseCount = clauseCount;
    row.entry.generation = m_generation;
    row.sourceText = std::move( sourceText );
    m_catalogue[ key ] = std::move( row );
}

void FakeSession::scriptGoal( const std::string& goalText, ScriptedGoal goal )
{
    std::lock_guard<std::mutex> guard( m_mutex );
    m_goals[ goalText ] = std::move( goal );
}

void FakeSession::scriptDefineError( const std::string& text,
                                     std::vector<us::Diagnostic> diagnostics )
{
    std::lock_guard<std::mutex> guard( m_mutex );
    m_defineErrors[ text ] = std::move( diagnostics );
}

void FakeSession::scriptCountingGoal( const std::string& goalText,
                                      std::uint32_t count )
{
    ScriptedGoal goal;
    for ( std::uint32_t i = 0; i < count; ++i ) {
        us::Value v;
        v.kind = us::Value::Kind::Int;
        v.i = static_cast<std::int64_t>( i );
        goal.solutions.push_back( { { "$n", v } } );
    }
    scriptGoal( goalText, std::move( goal ) );
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

us::RequestId FakeSession::nextRequest()
{
    ++m_requestCount;
    return m_nextRequest++;
}

void FakeSession::enqueue( Action action )
{
    m_pending.push_back( std::move( action ) );
}

void FakeSession::emitStarted( us::RequestId req )
{
    Action a;
    a.streamKey = requestStream( req );
    a.body = us::Started{ req };
    enqueue( std::move( a ) );
}

/**
 * Injected infrastructure failure. Returns true if this request was turned
 * into a Failed, in which case the caller must not enqueue its real answer.
 */
bool FakeSession::maybeFailRequest( us::RequestId req )
{
    bool fail = m_failNext;
    m_failNext = false;

    if ( !fail ) {
        if ( m_policy.failEveryNthRequest == 0 ) {
            return false;
        }
        if ( ( m_requestCount % m_policy.failEveryNthRequest ) != 0 ) {
            return false;
        }
    }

    Action a;
    a.streamKey = requestStream( req );
    a.body = us::Failed{ req, "injected failure", false };
    enqueue( std::move( a ) );
    return true;
}

void FakeSession::pushSolutions( us::QueryId qid, std::uint32_t howMany )
{
    QueryState& q = m_queries[ qid ];
    if ( q.terminal ) {
        return;
    }

    const std::uint64_t total = q.script.solutions.size();

    /*
     * Ambient events first, once, at the head of the first batch. They are
     * attributed to the query, which is the whole point of G0.8: a core that
     * emitted them unattributed would be indistinguishable here from one
     * that got it right.
     */
    if ( q.produced == 0 ) {
        for ( const us::Output& o : q.script.outputs ) {
            Action a;
            a.streamKey = queryStream( qid );
            a.query = qid;
            a.body = o;
            enqueue( std::move( a ) );
        }
        for ( const us::Diagnostic& d : q.script.diagnostics ) {
            Action a;
            a.streamKey = queryStream( qid );
            a.query = qid;
            a.body = d;
            enqueue( std::move( a ) );
        }
    }

    std::uint32_t emitted = 0;
    while ( emitted < howMany ) {
        if ( !q.script.infinite && q.produced >= total ) {
            break;
        }

        us::Solution s;
        s.index = q.produced;

        std::vector<std::pair<std::string, us::Value>> full;
        if ( q.script.infinite ) {
            us::Value v;
            v.kind = us::Value::Kind::Int;
            v.i = static_cast<std::int64_t>( q.produced );
            full.push_back( { "$n", v } );
        } else {
            full = q.script.solutions[ static_cast<std::size_t>( q.produced ) ];
        }

        /* Retain the untruncated form so inspect() has something to expand. */
        if ( q.options.retain && m_policy.retention ) {
            q.delivered.push_back( full );
        }

        for ( const auto& binding : full ) {
            s.bindings.push_back(
                { binding.first,
                  us::applyBudget( binding.second, q.options.budget ) } );
        }

        Action a;
        a.streamKey = queryStream( qid );
        a.query = qid;
        a.body = s;
        enqueue( std::move( a ) );

        ++q.produced;
        ++emitted;
    }

    const bool exhausted = !q.script.infinite && q.produced >= total;

    Action status;
    status.streamKey = queryStream( qid );
    status.query = qid;

    us::QueryStatus qs;
    qs.produced = q.produced;

    if ( exhausted && !q.script.failWith.empty() ) {
        qs.state = us::QueryStatus::State::Failed;
        qs.detail = q.script.failWith;
        q.terminal = true;
    } else if ( exhausted ) {
        qs.state = us::QueryStatus::State::Exhausted;
        q.terminal = true;
    } else {
        qs.state = us::QueryStatus::State::Running;
    }

    if ( q.terminal ) {
        q.retained = q.options.retain && m_policy.retention;
    }
    qs.retained = q.retained;

    status.body = qs;
    enqueue( std::move( status ) );
}

void FakeSession::deliver( const us::Event& event )
{
    m_replayBuffer.push_back( event );
    if ( m_replayBuffer.size() > m_policy.resumeBufferEvents ) {
        m_replayBuffer.erase(
            m_replayBuffer.begin(),
            m_replayBuffer.begin()
                + ( m_replayBuffer.size() - m_policy.resumeBufferEvents ) );
    }
}

// ---------------------------------------------------------------------------
// Pumping
// ---------------------------------------------------------------------------

std::size_t FakeSession::pump( std::size_t maxActions )
{
    std::unique_lock<std::mutex> lock( m_mutex );
    return pumpLocked( maxActions, lock );
}

std::size_t FakeSession::pumpLocked( std::size_t maxActions,
                                     std::unique_lock<std::mutex>& lock )
{
    std::size_t ran = 0;

    while ( ran < maxActions && !m_pending.empty() && !m_closed ) {
        /*
         * Choose which pending action runs next.
         *
         * An action is eligible only if it is the earliest pending one for
         * its stream: that is what keeps reordering "independent replies
         * only". Among the eligible ones inside the reorder window, pick at
         * random from the seeded generator, so a failing interleaving is
         * reproducible from the seed alone.
         */
        std::vector<std::size_t> eligible;
        std::set<std::uint64_t> seenStreams;
        for ( std::size_t i = 0; i < m_pending.size(); ++i ) {
            const std::uint64_t key = m_pending[ i ].streamKey;
            if ( seenStreams.insert( key ).second ) {
                eligible.push_back( i );
                if ( eligible.size() > m_policy.reorderWindow ) {
                    break;
                }
            }
        }
        if ( eligible.empty() ) {
            break;
        }

        std::size_t chosen = eligible.front();
        if ( eligible.size() > 1 ) {
            std::uniform_int_distribution<std::size_t> dist( 0, eligible.size() - 1 );
            chosen = eligible[ dist( m_rng ) ];
        }

        Action action = std::move( m_pending[ chosen ] );
        m_pending.erase( m_pending.begin() + static_cast<long>( chosen ) );

        if ( action.before ) {
            action.before();
        }

        us::Event event;
        event.header.seq = ++m_seq;
        event.header.worldGeneration = m_generation;
        if ( action.query ) {
            event.header.query = action.query;
            event.header.querySeq = ++m_queries[ *action.query ].querySeq;
        }
        event.body = std::move( action.body );

        deliver( event );

        us::EventSink* sink = ( m_disconnected ? nullptr : m_sink );
        if ( sink != nullptr ) {
            ++m_delivered;

            /*
             * The sink is called WITHOUT the lock. A front end folding an
             * event and immediately issuing a request from that fold is the
             * normal case, not an exotic one, and holding the lock across
             * the callback would deadlock on it. Discovering that here, in
             * the fake, is much cheaper than discovering it in a proxy.
             */
            lock.unlock();
            sink->onEvent( event );
            lock.lock();

            if ( m_policy.disconnectAfterEvents != 0
                 && m_delivered >= m_policy.disconnectAfterEvents
                 && !m_disconnected ) {
                m_disconnected = true;
                m_sink = nullptr;
            }
        }

        ++ran;
    }

    return ran;
}

bool FakeSession::idle() const
{
    std::lock_guard<std::mutex> guard( m_mutex );
    return m_pending.empty();
}

void FakeSession::startBackgroundPump( std::chrono::milliseconds perAction )
{
    stopBackgroundPump();
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        m_pumpStop = false;
    }
    m_pumpThread = std::thread( [this, perAction]() {
        for ( ;; ) {
            {
                std::lock_guard<std::mutex> guard( m_mutex );
                if ( m_pumpStop ) {
                    return;
                }
            }
            std::this_thread::sleep_for( perAction );
            std::unique_lock<std::mutex> lock( m_mutex );
            if ( m_pumpStop ) {
                return;
            }
            pumpLocked( 1, lock );
        }
    } );
}

void FakeSession::stopBackgroundPump()
{
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        m_pumpStop = true;
    }
    if ( m_pumpThread.joinable() ) {
        m_pumpThread.join();
    }
}

std::uint64_t FakeSession::deliveredEvents() const
{
    std::lock_guard<std::mutex> guard( m_mutex );
    return m_delivered;
}

bool FakeSession::disconnected() const
{
    std::lock_guard<std::mutex> guard( m_mutex );
    return m_disconnected;
}

void FakeSession::failNextRequest()
{
    std::lock_guard<std::mutex> guard( m_mutex );
    m_failNext = true;
}

void FakeSession::forceDisconnect()
{
    std::lock_guard<std::mutex> guard( m_mutex );
    m_disconnected = true;
    m_sink = nullptr;
}

// ---------------------------------------------------------------------------
// Session: lifecycle
// ---------------------------------------------------------------------------

us::Capabilities FakeSession::describe() const
{
    std::lock_guard<std::mutex> guard( m_mutex );

    us::Capabilities caps;
    caps.coreName = "fake-session";
    caps.coreVersion = "1";
    caps.location = "local";
    caps.debug = m_policy.debug;
    caps.structuredSolutions = m_policy.structuredSolutions;
    caps.realDemand = m_policy.realDemand;
    caps.realCancel = m_policy.realCancel;
    caps.images = m_policy.images;
    caps.retention = m_policy.retention;
    caps.resumeBufferEvents = m_policy.resumeBufferEvents;

    for ( const auto& entry : m_catalogue ) {
        if ( entry.second.entry.origin.kind == us::Origin::Kind::Builtin ) {
            caps.builtins.push_back( entry.first );
        }
    }

    for ( const auto& entry : m_queries ) {
        if ( entry.second.retained && !entry.second.released ) {
            ++caps.retainedQueries;
        }
    }

    std::set<std::uint64_t> live;
    for ( const Action& a : m_pending ) {
        if ( ( a.streamKey & kQueryStreamBit ) == 0 ) {
            live.insert( a.streamKey );
        }
    }
    caps.liveRequests = live.size();

    return caps;
}

void FakeSession::close()
{
    std::lock_guard<std::mutex> guard( m_mutex );
    m_closed = true;
    m_sink = nullptr;
    m_pending.clear();
    for ( auto& entry : m_queries ) {
        entry.second.retained = false;
        entry.second.released = true;
        entry.second.delivered.clear();
    }
}

void FakeSession::subscribe( us::EventSink& sink, us::Seq resumeFrom )
{
    std::unique_lock<std::mutex> lock( m_mutex );

    m_sink = &sink;
    m_disconnected = false;

    if ( resumeFrom == 0 ) {
        return;
    }

    /*
     * Resume, or say honestly that we cannot. The front end's recovery from
     * "expired" is a full refresh, so answering Failed here is a supported
     * outcome rather than an error -- and both branches are exercised.
     */
    const bool haveIt =
        m_replayBuffer.empty()
            ? ( resumeFrom == m_seq )
            : ( m_replayBuffer.front().header.seq <= resumeFrom + 1 );

    if ( !haveIt ) {
        us::Event ev;
        ev.header.seq = ++m_seq;
        ev.header.worldGeneration = m_generation;
        ev.body = us::Failed{ us::kNoRequest, "resume point expired", false };
        deliver( ev );
        ++m_delivered;
        lock.unlock();
        sink.onEvent( ev );
        return;
    }

    std::vector<us::Event> replay;
    for ( const us::Event& ev : m_replayBuffer ) {
        if ( ev.header.seq > resumeFrom ) {
            replay.push_back( ev );
        }
    }
    m_delivered += replay.size();

    lock.unlock();
    for ( const us::Event& ev : replay ) {
        sink.onEvent( ev );
    }
}

// ---------------------------------------------------------------------------
// Session: program
// ---------------------------------------------------------------------------

us::RequestId FakeSession::define( std::string text, us::Origin origin,
                                   us::OverwritePolicy policy )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    const us::RequestId req = nextRequest();
    emitStarted( req );
    if ( maybeFailRequest( req ) ) {
        return req;
    }

    us::Defined defined;
    defined.req = req;
    defined.module = origin.module;

    const auto errors = m_defineErrors.find( text );
    if ( errors != m_defineErrors.end() ) {
        /*
         * A bad define produces diagnostics AND a Defined with a matching
         * errorCount. Never silence, and never only one of the two: a front
         * end that renders only Defined must still be able to tell the user
         * something went wrong.
         */
        for ( us::Diagnostic d : errors->second ) {
            d.req = req;
            Action a;
            a.streamKey = requestStream( req );
            a.body = d;
            enqueue( std::move( a ) );
        }
        defined.errorCount =
            static_cast<std::uint32_t>( errors->second.size() );
    } else {
        /*
         * The fake does not parse. It records one predicate named after the
         * text's first token, which is enough for the catalogue obligations
         * and honest about being a fake.
         */
        us::PredicateKey key;
        key.name = text.substr( 0, text.find_first_of( " (\t\n" ) );
        key.arity = 0;
        key.module = origin.module;

        const bool existed = m_catalogue.count( key ) != 0;
        if ( existed && policy == us::OverwritePolicy::Fail ) {
            Action a;
            a.streamKey = requestStream( req );
            a.body = us::Failed{ req, "predicate exists", false };
            enqueue( std::move( a ) );
            return req;
        }

        CatalogueRow row;
        row.entry.key = key;
        row.entry.origin = origin;
        row.entry.clauseCount =
            ( existed && policy == us::OverwritePolicy::Append )
                ? m_catalogue[ key ].entry.clauseCount + 1
                : 1;
        row.entry.generation = ++m_generation;
        row.sourceText = text;
        m_catalogue[ key ] = row;

        if ( existed && policy == us::OverwritePolicy::ReplacePredicates ) {
            defined.replaced.push_back( key );
        } else if ( existed ) {
            defined.added.push_back( key );
        } else {
            defined.added.push_back( key );
        }
    }

    Action a;
    a.streamKey = requestStream( req );
    a.body = defined;
    enqueue( std::move( a ) );

    if ( defined.errorCount == 0 ) {
        us::WorldChanged wc;
        wc.generation = m_generation;
        wc.predicateCount = static_cast<std::uint32_t>( m_catalogue.size() );
        for ( const auto& e : m_catalogue ) {
            wc.clauseCount += e.second.entry.clauseCount;
        }
        Action w;
        w.streamKey = requestStream( req );
        w.body = wc;
        enqueue( std::move( w ) );
    }

    return req;
}

us::RequestId FakeSession::undefine( us::PredicateKey key, us::ModuleId scope )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    const us::RequestId req = nextRequest();
    emitStarted( req );
    if ( maybeFailRequest( req ) ) {
        return req;
    }

    key.module = scope;

    us::Defined defined;
    defined.req = req;
    defined.module = scope;
    if ( m_catalogue.erase( key ) != 0 ) {
        defined.removed.push_back( key );
        ++m_generation;
    }

    Action a;
    a.streamKey = requestStream( req );
    a.body = defined;
    enqueue( std::move( a ) );
    return req;
}

us::RequestId FakeSession::listing( us::ListingFilter filter )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    const us::RequestId req = nextRequest();
    emitStarted( req );
    if ( maybeFailRequest( req ) ) {
        return req;
    }

    std::vector<us::CatalogueEntry> matched;
    for ( const auto& entry : m_catalogue ) {
        const us::CatalogueEntry& e = entry.second.entry;
        if ( filter.module && *filter.module != e.key.module ) { continue; }
        if ( filter.arity && *filter.arity != e.key.arity ) { continue; }
        if ( !filter.namePrefix.empty()
             && e.key.name.compare( 0, filter.namePrefix.size(),
                                    filter.namePrefix ) != 0 ) {
            continue;
        }
        if ( e.origin.kind == us::Origin::Kind::Builtin
             && !filter.includeBuiltins ) {
            continue;
        }
        if ( e.origin.kind == us::Origin::Kind::Synthesized
             && !filter.includeSynthesized ) {
            continue;
        }
        matched.push_back( e );
    }

    us::Listing listing;
    listing.req = req;

    const std::size_t offset = filter.offset;
    const std::size_t limit =
        filter.limit ? filter.limit : matched.size() - std::min( offset, matched.size() );

    for ( std::size_t i = offset; i < matched.size() && listing.entries.size() < limit; ++i ) {
        listing.entries.push_back( matched[ i ] );
    }
    listing.more = ( offset + listing.entries.size() ) < matched.size();

    Action a;
    a.streamKey = requestStream( req );
    a.body = listing;
    enqueue( std::move( a ) );
    return req;
}

us::RequestId FakeSession::source( us::PredicateKey key )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    const us::RequestId req = nextRequest();
    emitStarted( req );
    if ( maybeFailRequest( req ) ) {
        return req;
    }

    Action a;
    a.streamKey = requestStream( req );

    const auto found = m_catalogue.find( key );
    if ( found == m_catalogue.end() ) {
        a.body = us::Failed{ req, "no such predicate", false };
    } else {
        us::SourceText st;
        st.req = req;
        st.key = key;
        st.text = found->second.sourceText;
        st.origin = found->second.entry.origin;
        a.body = st;
    }
    enqueue( std::move( a ) );
    return req;
}

// ---------------------------------------------------------------------------
// Session: queries
// ---------------------------------------------------------------------------

us::QueryId FakeSession::solve( std::string goalText, us::QueryOptions options )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    const us::QueryId qid = m_nextQuery++;

    QueryState q;
    q.goalText = goalText;
    q.options = options;

    const auto scripted = m_goals.find( goalText );
    if ( scripted != m_goals.end() ) {
        q.script = scripted->second;
    }
    m_queries[ qid ] = q;

    /*
     * There is no Started for solve: solve returns a QueryId, not a
     * RequestId, and its acknowledgement is its first QueryStatus. With
     * initialDemand == 0 that status is the only event until a demand
     * arrives, which is exactly the "nothing until demand" obligation.
     */
    pushSolutions( qid, options.initialDemand );
    return qid;
}

us::RequestId FakeSession::demand( us::QueryId query, us::Stream stream,
                                   std::uint32_t n )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    const us::RequestId req = nextRequest();
    emitStarted( req );

    const auto found = m_queries.find( query );
    if ( found == m_queries.end() ) {
        Action a;
        a.streamKey = requestStream( req );
        a.body = us::Failed{ req, "no such query", false };
        enqueue( std::move( a ) );
        return req;
    }

    if ( stream == us::Stream::Trace ) {
        /* No trace scripted: answer honestly rather than silently. */
        Action a;
        a.streamKey = requestStream( req );
        a.body = us::Failed{ req, "no trace for this query", false };
        enqueue( std::move( a ) );
        return req;
    }

    pushSolutions( query, n );
    return req;
}

us::RequestId FakeSession::cancel( us::QueryId query )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    const us::RequestId req = nextRequest();
    emitStarted( req );

    const auto found = m_queries.find( query );
    if ( found == m_queries.end() ) {
        Action a;
        a.streamKey = requestStream( req );
        a.body = us::Failed{ req, "no such query", false };
        enqueue( std::move( a ) );
        return req;
    }

    QueryState& q = found->second;

    us::QueryStatus qs;
    qs.produced = q.produced;

    if ( q.terminal ) {
        /*
         * Already finished. Cancel still answers with a terminal status --
         * "cancel always produces a terminal QueryStatus" is what lets a
         * front end detach unconditionally instead of racing the engine --
         * so it restates the outcome the query already reached rather than
         * inventing an Aborted that did not happen.
         */
        qs.state = us::QueryStatus::State::Exhausted;
    } else {
        qs.state = us::QueryStatus::State::Aborted;
        q.terminal = true;
        q.retained = q.options.retain && m_policy.retention;
    }
    qs.retained = q.retained;
    qs.detail = m_policy.realCancel
                    ? std::string()
                    : std::string( "detached; core cannot stop the work yet" );

    Action a;
    a.streamKey = queryStream( query );
    a.query = query;
    a.body = qs;
    enqueue( std::move( a ) );
    return req;
}

us::RequestId FakeSession::release( us::QueryId query )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    const us::RequestId req = nextRequest();
    emitStarted( req );

    const auto found = m_queries.find( query );
    if ( found != m_queries.end() ) {
        found->second.retained = false;
        found->second.released = true;
        found->second.delivered.clear();
    }
    return req;
}

us::RequestId FakeSession::inspect( us::QueryId query,
                                    std::uint64_t solutionIndex,
                                    us::ValuePath path,
                                    us::ValueBudget budget )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    const us::RequestId req = nextRequest();
    emitStarted( req );

    Action a;
    a.streamKey = requestStream( req );

    const auto found = m_queries.find( query );
    if ( !m_policy.retention ) {
        a.body = us::Failed{ req, "retention unavailable", false };
    } else if ( found == m_queries.end() || found->second.released ) {
        a.body = us::Failed{ req, "query released", false };
    } else if ( solutionIndex >= found->second.delivered.size() ) {
        a.body = us::Failed{ req, "no such solution", false };
    } else if ( path.empty() ) {
        a.body = us::Failed{ req, "path must name a binding", false };
    } else {
        const auto& bindings =
            found->second.delivered[ static_cast<std::size_t>( solutionIndex ) ];
        const std::uint32_t which = path.front();
        if ( which >= bindings.size() ) {
            a.body = us::Failed{ req, "no such binding", false };
        } else {
            const us::ValuePath rest( path.begin() + 1, path.end() );
            const auto node = us::valueAtPath( bindings[ which ].second, rest );
            if ( !node ) {
                a.body = us::Failed{ req, "path leaves the value", false };
            } else {
                us::Expanded ex;
                ex.req = req;
                ex.path = path;
                ex.value = us::applyBudget( *node, budget );
                a.body = ex;
            }
        }
    }

    enqueue( std::move( a ) );
    return req;
}

// ---------------------------------------------------------------------------
// Session: image
// ---------------------------------------------------------------------------

us::RequestId FakeSession::save( std::string path, us::SaveOptions )
{
    std::lock_guard<std::mutex> guard( m_mutex );
    const us::RequestId req = nextRequest();
    emitStarted( req );

    Action a;
    a.streamKey = requestStream( req );
    if ( !m_policy.images ) {
        a.body = us::Failed{ req, "images unavailable", false };
    } else {
        us::Defined d;
        d.req = req;
        a.body = d;
    }
    enqueue( std::move( a ) );
    ( void ) path;
    return req;
}

us::RequestId FakeSession::load( std::string path )
{
    std::lock_guard<std::mutex> guard( m_mutex );
    const us::RequestId req = nextRequest();
    emitStarted( req );

    Action a;
    a.streamKey = requestStream( req );
    if ( !m_policy.images ) {
        a.body = us::Failed{ req, "images unavailable", false };
    } else {
        m_catalogue.clear();
        ++m_generation;
        us::WorldChanged wc;
        wc.generation = m_generation;
        a.body = wc;
    }
    enqueue( std::move( a ) );
    ( void ) path;
    return req;
}

us::RequestId FakeSession::insert( std::string path, us::OverwritePolicy )
{
    std::lock_guard<std::mutex> guard( m_mutex );
    const us::RequestId req = nextRequest();
    emitStarted( req );

    Action a;
    a.streamKey = requestStream( req );
    if ( !m_policy.images ) {
        a.body = us::Failed{ req, "images unavailable", false };
    } else {
        us::Defined d;
        d.req = req;
        a.body = d;
    }
    enqueue( std::move( a ) );
    ( void ) path;
    return req;
}

// ---------------------------------------------------------------------------
// Session: debug
// ---------------------------------------------------------------------------

us::RequestId FakeSession::debug( us::DebugCommand command )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    const us::RequestId req = nextRequest();
    emitStarted( req );

    if ( !m_policy.debug ) {
        /*
         * A core built without the debugger answers Failed and never
         * crashes. This is the whole of G0.11's negative half, and it is
         * one line here precisely because the interface made it expressible.
         */
        Action a;
        a.streamKey = requestStream( req );
        a.body = us::Failed{ req, "debug unavailable", false };
        enqueue( std::move( a ) );
        return req;
    }

    if ( const us::SetTrace* setTrace = std::get_if<us::SetTrace>( &command ) ) {
        if ( setTrace->on ) {
            const us::QueryId qid = setTrace->query;
            for ( int i = 0; i < 3; ++i ) {
                us::TraceEvent te;
                te.kind = ( i == 0 ) ? us::TraceEvent::Kind::Call
                                     : us::TraceEvent::Kind::Exit;
                te.depth = static_cast<std::uint32_t>( i );
                te.key.name = "traced";
                te.key.arity = 1;
                Action a;
                a.streamKey = queryStream( qid );
                a.query = qid;
                a.body = te;
                enqueue( std::move( a ) );
            }
        }
    }

    return req;
}

} // namespace unify_test
