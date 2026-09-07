/**
 * @file session-bridge.cpp
 */

#include "session-bridge.hpp"

namespace lens {

/**
 * The sink the session delivers to.
 *
 * Called on the SESSION thread, so it does exactly one thing -- push onto a
 * guarded queue -- and never touches the model. Doing anything more here is
 * how a UI framework ends up being mutated from a background thread.
 */
class SessionBridge::Sink : public us::EventSink {
public:
    explicit Sink( SessionBridge* owner ) : m_owner( owner ) {}

    void onEvent( const us::Event& event ) override
    {
        std::lock_guard<std::mutex> guard( m_owner->m_mutex );
        m_owner->m_events.push_back( event );
    }

private:
    SessionBridge* m_owner;
};


SessionBridge::SessionBridge() = default;


SessionBridge::~SessionBridge()
{
    if( m_session ) {
        m_session->close();
    }
}


std::string SessionBridge::start()
{
    /*
     * The engine's stderr trace prints several lines per resolution step and
     * is on by default. unify-run's REPL turns it off for exactly this
     * reason: it is invaluable when debugging the engine and it makes any
     * interactive front end unusable. Redirecting stderr instead would take
     * the diagnostics with it -- which is why the engine grew this switch in
     * the first place.
     */
    vault::unify::setDebugTraceEnabled( false );

    m_session.reset( new vault::unify::session::LocalSession() );
    if( 0 != m_session->start() ) {
        m_session.reset();
        return "cannot start the Unify engine";
    }

    m_sink.reset( new Sink( this ) );
    m_session->subscribe( *m_sink, 0 );
    m_caps = m_session->describe();
    return std::string();
}


void SessionBridge::issue( Model& model, const CommandRequest& request )
{
    if( !m_session ) {
        model.setMessage( "no session: lens was started without an engine" );
        return;
    }

    if( request.kind == "solve" ) {
        us::QueryOptions options;
        /*
         * A screenful, not everything. The transcript shows what fits and
         * asks for more when the user scrolls -- which is the back-pressure
         * of ARCHITECTURE.md section 5 made visible, and the reason a goal
         * with ten thousand solutions does not freeze the UI (G2.4).
         */
        options.initialDemand = 20;
        options.retain = true;

        Buffer* transcript = model.transcript();
        const us::QueryId query = m_session->solve( request.text, options );
        if( transcript ) {
            transcript->transcript.liveQuery = query;
        }
        return;
    }

    if( request.kind == "define" ) {
        us::Origin origin;
        origin.kind = us::Origin::Kind::Transcript;
        m_session->define( request.text, origin, us::OverwritePolicy::Append );
        return;
    }

    if( request.kind == "cancel" ) {
        m_session->cancel( (us::QueryId) request.handle );
        return;
    }

    if( request.kind == "demand" ) {
        m_session->demand( (us::QueryId) request.handle,
                           us::Stream::Solutions, 20 );
        return;
    }

    /*
     * Reported rather than dropped. A command that asked for something the
     * bridge does not implement is a bug in one of them, and silence makes
     * it look like the engine ignored the user.
     */
    model.setMessage( "unhandled request kind '" + request.kind + "'" );
}


std::vector<us::Event> SessionBridge::drain()
{
    std::lock_guard<std::mutex> guard( m_mutex );
    std::vector<us::Event> out( m_events.begin(), m_events.end() );
    m_events.clear();
    return out;
}


void SessionBridge::settle()
{
    if( m_session ) {
        m_session->waitUntilQuiet();
    }
}

} // namespace lens
