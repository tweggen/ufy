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
#include <map>
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
        settle();

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
                ++m_asked[ request.kind ];
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

    /** As main.cpp's script loop: until no event asks for anything more. */
    void settle()
    {
        for ( int round = 0; round < 64; ++round ) {
            m_bridge.settle();
            const std::vector<us::Event> events = m_bridge.drain();
            for ( const us::Event& event : events ) {
                m_model.foldSession( event );
            }
            const std::vector<CommandRequest> asked = m_model.takeRequests();
            if ( asked.empty() && events.empty() ) {
                break;
            }
            for ( const CommandRequest& request : asked ) {
                ++m_asked[ request.kind ];
                m_bridge.issue( m_model, request );
            }
        }
    }

    /** Issue a request directly, as a command handler's result would be. */
    void issue( const CommandRequest& request )
    {
        m_bridge.issue( m_model, request );
        settle();
    }

    /** How many requests of `kind` keys have asked for, in all. */
    std::size_t asked( const std::string& kind ) const
    {
        const auto it = m_asked.find( kind );
        return it == m_asked.end() ? 0 : it->second;
    }

    SessionBridge& bridge() { return m_bridge; }

    /** Tab until a `kind` panel has focus. */
    void focus( PanelKind kind )
    {
        for ( int i = 0; i < 24; ++i ) {
            const Buffer* focused = m_model.focusedBuffer();
            if ( focused && focused->kind == kind ) {
                return;
            }
            press( "Tab" );
        }
        UT_FAIL( "could not focus the panel" );
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
    std::map<std::string, std::size_t> m_asked;
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

/** Put the catalogue cursor on `label` and press Enter. */
void openDefinition( Rig& rig, const std::string& label )
{
    rig.focus( PanelKind::Catalogue );
    const std::vector<CatalogueRow> rows = rig.model().catalogueRows();
    int at = -1;
    for ( std::size_t i = 0; i < rows.size(); ++i ) {
        if ( rows[i].label == label ) { at = (int) i; }
    }
    UT_CHECK_MSG( at >= 0, "no catalogue row " << label );
    while ( rig.model().catalogue().selected < at ) { rig.press( "Down" ); }
    while ( rig.model().catalogue().selected > at ) { rig.press( "Up" ); }
    rig.press( "Enter" );
    UT_CHECK_MSG( rig.model().sourceDefinition().set, "the source never arrived" );
}

/** Live clauses of name/arity, over every module, as the catalogue has it. */
std::uint32_t clausesNamed( Rig& rig, const std::string& name, std::uint32_t arity )
{
    std::uint32_t n = 0;
    for ( const us::CatalogueEntry& e : rig.model().catalogue().entries ) {
        if ( e.key.name == name && e.key.arity == arity ) { n += e.clauseCount; }
    }
    return n;
}

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

    // -- G5: the results panels against the real engine ----------------------

    registry.add( "G5.1 Down past the end of the table fetches one batch", []() {
        /*
         * End to end: the key reaches the engine and the batch comes back
         * into the table. That holding Down asks only ONCE is a property of
         * the gap between asking and being answered, and this rig settles
         * the engine after every key that asked for anything -- so it
         * cannot see that gap, and the model test (lens-inspect, "exactly
         * one demand") is where it is pinned.
         */
        Rig rig( "full", 160, 50 );
        defineTenThousand( rig );
        rig.submit( kTenThousandGoal );
        UT_CHECK_EQ( rig.model().results().solutions.size(), kInitialDemand );

        rig.focus( PanelKind::Solutions );
        rig.press( "End" );
        rig.press( "Down Down Down Down Down" );
        UT_CHECK_EQ( rig.asked( "demand" ), std::size_t( 1 ) );
        UT_CHECK_EQ( rig.model().results().solutions.size(), 2 * kInitialDemand );
        UT_CHECK( rig.model().results().more );
    } );

    registry.add( "G5.2 a term deeper than the budget is cut by the core and "
                  "opened with inspect, not by solving again", []() {
        Rig rig( "full", 160, 50 );
        /* Eleven levels: three past the core's default maxDepth of 8. */
        rig.submit( "deep( f( f( f( f( f( f( f( f( f( f( f( bottom ) ) ) ) ) ) ) ) ) ) ) );" );
        rig.submit( "deep( $t )" );
        UT_CHECK_EQ( rig.asked( "solve" ), std::size_t( 1 ) );
        UT_CHECK_EQ( rig.model().results().solutions.size(), std::size_t( 1 ) );
        UT_CHECK_MSG( rig.model().results().solutions[0].bindings[0].second.args.size() == 1,
                      "the core did not send the term as a tree" );

        /* Open every node, top to bottom, the way a user would. */
        rig.focus( PanelKind::Inspector );
        bool sawCut = false;
        for ( int step = 0; step < 80; ++step ) {
            const std::vector<InspectorRow> rows = rig.model().inspectorRows();
            const int at = rig.model().inspector().cursor;
            if ( at < 0 || at >= (int) rows.size() ) {
                break;
            }
            const InspectorRow& row = rows[ (std::size_t) at ];
            sawCut = sawCut || row.truncated;
            if ( row.expandable && !row.open ) {
                rig.press( "Right" );
            } else if ( at + 1 < (int) rows.size() ) {
                rig.press( "Down" );
            } else {
                break;
            }
        }

        UT_CHECK_MSG( sawCut, "the core's budget never cut the term -- the case "
                              "proves nothing" );
        UT_CHECK_MSG( rig.asked( "inspect" ) >= 1, "nothing asked inspect" );
        UT_CHECK_EQ( rig.asked( "solve" ), std::size_t( 1 ) );

        bool bottom = false;
        for ( const InspectorRow& row : rig.model().inspectorRows() ) {
            UT_CHECK_MSG( !row.truncated, "a cut node is left: " << row.text );
            bottom = bottom || row.text == "bottom";
        }
        UT_CHECK_MSG( bottom, "the innermost term never arrived" );
    } );

    registry.add( "G0.10 lens keeps exactly the query it shows retained", []() {
        Rig rig;
        rig.submit( "colour( red ); colour( green );" );
        for ( int i = 0; i < 6; ++i ) {
            rig.submit( "colour( $c )" );
            UT_CHECK_MSG( rig.bridge().describeNow().retainedQueries <= 1,
                          "after query " << ( i + 1 ) << " the core holds "
                              << rig.bridge().describeNow().retainedQueries
                              << " retained queries" );
        }
        UT_CHECK_EQ( rig.bridge().describeNow().retainedQueries, std::uint64_t( 1 ) );
    } );

    // -- G3: the browser against the real engine ------------------------------

    registry.add( "G3.4 each definition refreshes the catalogue; idle keys ask "
                  "nothing", []() {
        Rig rig( "browse", 120, 40 );
        UT_CHECK_MSG( rig.model().catalogue().loaded,
                      "the first listing never arrived" );
        const std::size_t before = rig.asked( "listing" );

        rig.submit( "colour( red );" );
        rig.submit( "colour( green );" );
        rig.submit( "warm( $c ) { colour( $c ); $c = red; }" );
        const std::size_t afterDefines = rig.asked( "listing" );
        UT_CHECK_MSG( afterDefines - before >= 1 && afterDefines - before <= 3,
                      "three definitions asked for " << ( afterDefines - before )
                          << " listings" );

        bool colour = false;
        for ( const us::CatalogueEntry& e : rig.model().catalogue().entries ) {
            if ( e.key.name == "colour" && e.key.arity == 1 ) {
                colour = true;
                UT_CHECK_EQ( e.clauseCount, std::uint32_t( 2 ) );
            }
        }
        UT_CHECK_MSG( colour, "colour/1 is not in the catalogue" );

        /* Walking the catalogue is not a reason to ask the engine anything. */
        rig.focus( PanelKind::Catalogue );
        for ( int i = 0; i < 20; ++i ) {
            rig.press( "Down" );
            rig.press( "Up" );
        }
        UT_CHECK_EQ( rig.asked( "listing" ), afterDefines );
    } );

    registry.add( "G3.2 Enter on a predicate shows the engine's own source", []() {
        Rig rig( "browse", 120, 40 );
        rig.submit( "colour( red );" );
        rig.submit( "colour( green );" );
        rig.focus( PanelKind::Catalogue );

        const std::vector<CatalogueRow> rows = rig.model().catalogueRows();
        int at = -1;
        for ( std::size_t i = 0; i < rows.size(); ++i ) {
            if ( rows[i].label == "colour/1" ) { at = (int) i; }
        }
        UT_CHECK( at >= 0 );
        while ( rig.model().catalogue().selected < at ) { rig.press( "Down" ); }
        rig.press( "Enter" );

        UT_CHECK_EQ( rig.asked( "source" ), std::size_t( 1 ) );
        const SourceDefinition& d = rig.model().sourceDefinition();
        UT_CHECK_MSG( d.set, "the source never arrived" );
        UT_CHECK_EQ( d.lines.size(), std::size_t( 2 ) );
        UT_CHECK_MSG( !d.lines.empty() && d.lines[0].find( "red" ) != std::string::npos,
                      "first line: " << ( d.lines.empty() ? "" : d.lines[0] ) );
        UT_CHECK_EQ( d.clauses, std::uint32_t( 2 ) );
    } );

    registry.add( "G3.3 edit a definition and F3: replaced, and nothing else "
                  "touched", []() {
        Rig rig( "browse", 120, 40 );
        rig.submit( "colour( red );" );
        rig.submit( "colour( green );" );
        rig.submit( "warm( $c ) { colour( $c ); $c = red; }" );
        openDefinition( rig, "colour/1" );

        /*
         * Named, not temporaries in the comparison: GCC 15 at -O2 reports
         * a bogus -Wfree-nonheap-object for a braced vector<string>
         * temporary destroyed inside this lambda.
         */
        const std::vector<std::string> asTyped = { "colour( red );",
                                                   "colour( green );" };
        const std::vector<std::string> edited = { "colour( red );" };

        const SourceDefinition& d = rig.model().sourceDefinition();
        UT_CHECK_MSG( d.lines == asTyped,
                      "the source is not the text as typed (E5)" );

        /* Delete the second clause: end of it, sixteen characters, join. */
        rig.focus( PanelKind::Source );
        rig.press( "Down End" );
        for ( int i = 0; i < 17; ++i ) { rig.press( "Backspace" ); }
        UT_CHECK( d.lines == edited );
        rig.press( "F3" );

        UT_CHECK_EQ( clausesNamed( rig, "colour", 1 ), std::uint32_t( 1 ) );
        UT_CHECK_EQ( clausesNamed( rig, "warm", 1 ), std::uint32_t( 1 ) );
        UT_CHECK( !d.dirty );

        bool reported = false;
        for ( const TranscriptEntry& e : rig.transcript().entries ) {
            reported = reported || e.text == "-- colour/1 replaced (1 clause)";
        }
        UT_CHECK_MSG( reported, "the transcript did not report the replacement"
                                    << rig.dump() );

        /* And the engine answers accordingly. */
        rig.focus( PanelKind::Transcript );
        rig.submit( "colour( $x )" );
        UT_CHECK_EQ( rig.count( TranscriptEntry::Kind::Solution, rig.lastQuery() ),
                     std::size_t( 1 ) );
        rig.submit( "warm( $c )" );
        UT_CHECK_EQ( rig.count( TranscriptEntry::Kind::Solution, rig.lastQuery() ),
                     std::size_t( 1 ) );
    } );

    registry.add( "G3.3 an edit that does not parse leaves the old definition", []() {
        Rig rig( "browse", 120, 40 );
        rig.submit( "colour( red );" );
        rig.submit( "colour( green );" );
        openDefinition( rig, "colour/1" );
        rig.focus( PanelKind::Source );
        rig.press( "End" );
        rig.press( "(" );
        rig.press( "F3" );

        UT_CHECK_EQ( clausesNamed( rig, "colour", 1 ), std::uint32_t( 2 ) );
        UT_CHECK( rig.model().sourceDefinition().dirty );

        UT_CHECK_MSG( rig.model().message().find( "old definition stands" )
                          != std::string::npos,
                      "message: " << rig.model().message() );

        /*
         * Asked of the ENGINE, not only of lens's catalogue: the parser
         * appended `colour( red );` before it reached the error, and a
         * catalogue that never refreshed would still say 2 whatever the
         * engine now holds.
         */
        rig.focus( PanelKind::Transcript );
        rig.submit( "colour( $x )" );
        UT_CHECK_EQ( rig.count( TranscriptEntry::Kind::Solution, rig.lastQuery() ),
                     std::size_t( 2 ) );
    } );

    registry.add( "G3.4 a define that fails halfway still refreshes the catalogue",
                  []() {
        /*
         * The parser appends as it parses: `colour( red );` is in the world
         * before the parser reaches the broken second clause. The core used
         * to announce a change only for a clean define, which left the
         * catalogue saying colour/1 did not exist while the engine answered
         * for it.
         */
        Rig rig( "browse", 120, 40 );
        rig.submit( "colour( red ); colour( ;" );
        UT_CHECK_EQ( clausesNamed( rig, "colour", 1 ), std::uint32_t( 1 ) );
    } );

    return registry.run( "lens session: the transcript and results against a "
                         "real engine (G2, G3, G5)" ) == 0
               ? 0
               : 1;
}
