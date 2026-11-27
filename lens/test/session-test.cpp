/**
 * @file session-test.cpp
 *
 * The transcript against a REAL engine, asserted on the model.
 *
 * Every other model test in lens runs with no engine at all, which is what
 * the boundary was drawn for -- and it means none of them can say what
 * happens when ten thousand solutions are actually on their way, or when a
 * `cancel` actually crosses to a core that cannot stop. Those are claims
 * about the pair, so this suite holds the pair: a Model and a SessionBridge
 * over a LocalSession, driven exactly the way `--script` drives them --
 * fold a key, issue what it asked for, let the engine settle, fold what came
 * back.
 *
 * Still rule 6: the assertions are on the model (which rows the transcript
 * holds, what the status line says), never on a rendered grid.
 */

#include "../../unify/test/session/test-harness.hpp"

#include "../src/app/layouts.hpp"
#include "../src/app/session-bridge.hpp"
#include "../src/model/model.hpp"

#include <chrono>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace unify_test;
using namespace lens;

/** A model wired to a live engine, the way the composition root wires it. */
class Rig {
public:
    explicit Rig( const char* layout = "run", int w = 120, int h = 40 )
    {
        m_model.setGeometry( w, h );
        registerShellCommands( m_model );
        m_model.keymap() = defaultKeymap();
        applyStockLayout( m_model, layout );
        m_model.rebuildHelp();

        const std::string error = m_bridge.start();
        if ( !error.empty() ) {
            UT_FAIL( "cannot start the engine: " << error );
        }
        m_model.setCapabilities( m_bridge.capabilities() );

        Buffer* transcript = m_model.transcript();
        if ( !transcript ) {
            UT_FAIL( "layout '" << layout << "' has no transcript" );
        }
        while ( m_model.focusedBuffer() != transcript ) {
            press( "Tab" );
        }
    }

    Model& model() { return m_model; }
    TranscriptState& transcript() { return m_model.transcript()->transcript; }

    /** One key sequence, exactly as runScript() does it. */
    void press( const char* keys )
    {
        KeySeq seq;
        if ( !parseKeySeq( keys, seq ) ) {
            UT_FAIL( "the test itself has a bad key sequence: '" << keys << "'" );
        }
        for ( const Key& key : seq ) {
            Event event;
            event.kind = Event::Kind::Key;
            event.key = key;
            m_issued = fold( m_model, event );
            for ( const CommandRequest& request : m_issued ) {
                m_bridge.issue( m_model, request );
                /* Read back BEFORE the engine settles: a query that finishes
                 * clears liveQuery, and a test that wants to ask "how many
                 * rows did that query get?" still needs to know its id. */
                if ( request.kind == "solve" ) {
                    m_lastQuery = transcript().liveQuery;
                }
            }
            if ( !m_issued.empty() ) {
                settle();
            }
        }
    }

    /** Type a line and press Enter. */
    void submit( const std::string& line )
    {
        for ( char c : line ) {
            Event event;
            event.kind = Event::Kind::Key;
            event.key = Key::character( (char32_t) (unsigned char) c );
            ( void ) fold( m_model, event );
        }
        press( "Enter" );
    }

    void settle()
    {
        m_bridge.settle();
        for ( const us::Event& event : m_bridge.drain() ) {
            m_model.foldSession( event );
        }
    }

    /** Issue a request directly, as a command handler's result would be. */
    void issue( const CommandRequest& request )
    {
        m_bridge.issue( m_model, request );
        settle();
    }

    /** The query the most recent solve started. */
    us::QueryId lastQuery() const { return m_lastQuery; }

    /** What the last key asked for. */
    const std::vector<CommandRequest>& issued() const { return m_issued; }

    std::size_t count( TranscriptEntry::Kind kind, us::QueryId query = us::kNoQuery )
    {
        std::size_t n = 0;
        for ( const TranscriptEntry& entry : transcript().entries ) {
            if ( entry.kind == kind
                 && ( query == us::kNoQuery || entry.query == query ) ) {
                ++n;
            }
        }
        return n;
    }

    /** The transcript, one entry per line, for a failure message. */
    std::string dump( std::size_t tail = 12 )
    {
        std::ostringstream os;
        const std::vector<TranscriptEntry>& entries = transcript().entries;
        const std::size_t from = entries.size() > tail ? entries.size() - tail : 0;
        for ( std::size_t i = from; i < entries.size(); ++i ) {
            os << "\n          | " << entries[i].text;
        }
        return os.str();
    }

private:
    Model m_model;
    SessionBridge m_bridge;
    std::vector<CommandRequest> m_issued;
    us::QueryId m_lastQuery = us::kNoQuery;
};

/**
 * Ten thousand solutions from four digits, so the program is one line a
 * reader can check by eye rather than a generated file.
 */
void defineTenThousand( Rig& rig )
{
    rig.submit( "d( 0 ); d( 1 ); d( 2 ); d( 3 ); d( 4 ); "
                "d( 5 ); d( 6 ); d( 7 ); d( 8 ); d( 9 );" );
    rig.submit( "n( $a, $b, $c, $d ) { d( $a ); d( $b ); d( $c ); d( $d ); }" );
}

const char* const kTenThousandGoal = "n( $a, $b, $c, $d )";

/** The batch the bridge asks for -- SessionBridge::issue, "a screenful". */
const std::size_t kInitialDemand = 20;

} // namespace

int main()
{
    Registry registry;

    // -- G2.4: ten thousand solutions do not freeze the UI ------------------

    registry.add( "G2.4 a 10,000-solution goal delivers one batch, and keys "
                  "are still serviced", []() {
        Rig rig;
        defineTenThousand( rig );

        const auto started = std::chrono::steady_clock::now();
        rig.submit( kTenThousandGoal );
        const us::QueryId query = rig.lastQuery();
        UT_CHECK_MSG( query != us::kNoQuery, "the solve was never issued" );

        /*
         * The transcript holds initialDemand rows, not ten thousand: the core
         * buffers (E11 is outstanding), and the front end asks for a
         * screenful. A transcript that held every row is the frozen UI this
         * criterion exists to prevent -- it is also the one that takes
         * seconds to render each frame.
         */
        UT_CHECK_MSG( rig.count( TranscriptEntry::Kind::Solution, query )
                          == kInitialDemand,
                      "expected " << kInitialDemand << " solution rows, found "
                          << rig.count( TranscriptEntry::Kind::Solution, query )
                          << rig.dump() );

        /* Still live: the other 9,980 are waiting for demand, not lost. */
        UT_CHECK( !rig.transcript().finished );
        UT_CHECK_MSG( rig.model().sessionStatus().find( "20 so far" )
                          != std::string::npos,
                      "status line: " << rig.model().sessionStatus() );

        /* And the keyboard still works while the query is live. */
        for ( int i = 0; i < 50; ++i ) {
            rig.press( "x" );
        }
        UT_CHECK_EQ( rig.transcript().input.size(), (std::size_t) 50 );

        /*
         * The wall-clock bound. Generous, because a sanitizer build is ten
         * times slower than this one -- the failure it is for is not "slow",
         * it is "rendered or folded ten thousand rows", which costs minutes.
         */
        const auto elapsed = std::chrono::steady_clock::now() - started;
        UT_CHECK_MSG( elapsed < std::chrono::seconds( 30 ),
                      "took "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 elapsed ).count()
                          << " ms" );
    } );

    registry.add( "G2.4 query.more asks for exactly one more batch", []() {
        Rig rig;
        defineTenThousand( rig );
        rig.submit( kTenThousandGoal );
        const us::QueryId query = rig.lastQuery();

        rig.model().setMessage( "" );
        const Command* more = rig.model().commands().find( "query.more" );
        UT_CHECK( more != NULL );
        UT_CHECK( more->enabled( rig.model() ) );
        const std::vector<CommandRequest> requests = more->run( rig.model() );
        UT_CHECK_EQ( requests.size(), (std::size_t) 1 );
        UT_CHECK_EQ( requests[0].kind, std::string( "demand" ) );
        UT_CHECK_EQ( requests[0].handle, (std::uint64_t) query );
    } );

    // -- G2.6: cancel always reaches Aborted, and the UI detaches -----------

    registry.add( "G2.6 C-c on a live query reaches Aborted and detaches", []() {
        Rig rig;
        defineTenThousand( rig );
        rig.submit( kTenThousandGoal );
        const us::QueryId query = rig.lastQuery();
        UT_CHECK( !rig.transcript().finished );

        rig.press( "C-c" );

        UT_CHECK_EQ( rig.issued().size(), (std::size_t) 1 );
        UT_CHECK_EQ( rig.issued()[0].kind, std::string( "cancel" ) );
        UT_CHECK_EQ( rig.issued()[0].handle, (std::uint64_t) query );

        /* Detached: nothing is live any more ... */
        UT_CHECK( rig.transcript().finished );
        UT_CHECK_EQ( rig.transcript().liveQuery, us::kNoQuery );

        /* ... and the core's terminal answer is in the transcript. */
        bool aborted = false;
        for ( const TranscriptEntry& entry : rig.transcript().entries ) {
            if ( entry.kind == TranscriptEntry::Kind::Status
                 && entry.query == query
                 && entry.text.find( "aborted" ) != std::string::npos ) {
                aborted = true;
            }
        }
        UT_CHECK_MSG( aborted, "no Aborted status for the query" << rig.dump() );

        /*
         * The v1 limitation, stated: this core cannot stop a running goal,
         * so the status line must not imply that it did (plan section 6).
         */
        UT_CHECK( !rig.model().capabilities().realCancel );
        UT_CHECK_MSG( rig.model().sessionStatus().find( "cancel detaches only" )
                          != std::string::npos,
                      "status line: " << rig.model().sessionStatus() );
        UT_CHECK_MSG( rig.model().message().find( "detached" )
                          != std::string::npos,
                      "message: " << rig.model().message() );
    } );

    registry.add( "G2.6 C-g aborts the live query too", []() {
        Rig rig;
        defineTenThousand( rig );
        rig.submit( kTenThousandGoal );
        rig.press( "C-g" );
        UT_CHECK( rig.transcript().finished );
        UT_CHECK_EQ( rig.issued().size(), (std::size_t) 1 );
        UT_CHECK_EQ( rig.issued()[0].kind, std::string( "cancel" ) );
    } );

    registry.add( "G2.6 a detached query adds no rows afterwards", []() {
        Rig rig;
        defineTenThousand( rig );
        rig.submit( kTenThousandGoal );
        const us::QueryId query = rig.lastQuery();
        const std::size_t before =
            rig.count( TranscriptEntry::Kind::Solution, query );

        /*
         * Cancel and ask for more in the same breath: the demand is queued
         * behind the cancel, so the core must refuse it -- and even if a
         * core raced and sent rows anyway, the front end has detached and
         * must not show them under a query the user stopped.
         */
        rig.press( "C-c" );
        CommandRequest more;
        more.kind = "demand";
        more.handle = (std::uint64_t) query;
        rig.issue( more );

        UT_CHECK_EQ( rig.count( TranscriptEntry::Kind::Solution, query ), before );
    } );

    registry.add( "G2.6 with nothing running, cancel says so and asks nothing",
                  []() {
        Rig rig;
        rig.press( "C-c" );
        UT_CHECK( rig.issued().empty() );
        UT_CHECK_MSG( !rig.model().message().empty(),
                      "C-c with nothing running said nothing" );
    } );

    registry.add( "G2.6 a finished query does not hold the status line", []() {
        Rig rig;
        rig.submit( "c( 1 );" );
        rig.submit( "c( $x )" );
        UT_CHECK( rig.transcript().finished );
        UT_CHECK_MSG( rig.model().sessionStatus().find( "idle" )
                          != std::string::npos,
                      "status line: " << rig.model().sessionStatus() );
    } );

    // -- H: help that cannot fall behind the engine --------------------------

    registry.add( "G2 H every builtin the core reports has a help topic", []() {
        /*
         * UI.md section 5.3's test: the extractor proves PRESENCE, not
         * correctness, and presence is checked against the one list that
         * cannot be wrong about what exists -- the engine's own describe().
         * A builtin added to the engine and not to SPEC.md section 9 fails
         * here, which is the point.
         */
        Rig rig;
        const std::vector<us::PredicateKey>& builtins =
            rig.model().capabilities().builtins;
        UT_CHECK_MSG( !builtins.empty(), "the core reports no builtins at all" );
        for ( const us::PredicateKey& key : builtins ) {
            UT_CHECK_MSG( rig.model().helpBook()->topic( "builtin." + key.name )
                              != NULL,
                          "builtin '" << key.name << "/" << key.arity
                              << "' has no help topic -- document it in "
                                 "unify/SPEC.md section 9" );
        }
    } );

    return registry.run( "lens session: the transcript against a real engine (G2)" ) == 0
               ? 0
               : 1;
}
