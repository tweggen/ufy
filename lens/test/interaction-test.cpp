/**
 * @file interaction-test.cpp
 *
 * The third test category: what happens BETWEEN frames.
 *
 * We had two, and neither could see the bug this file was written for. The
 * model tests assert one state against one expectation. The golden screens
 * assert one final frame, and rule 6 of the method deliberately keeps them
 * to layout so a cosmetic change does not re-record a dozen files. Both are
 * right, and both are blind to time.
 *
 * Every defect a real user has reported in lens so far has lived in the
 * sequence rather than in any single state:
 *
 *   - help opened too small to read
 *   - no way to discover the basic keys
 *   - "pressing Up after Down does nothing the first time"
 *
 * The last one is instructive. The screen after `Down Down Up` is perfectly
 * self-consistent -- the right line is highlighted, the right text is shown,
 * a golden of it would look correct and would pass. What is wrong is the
 * TRANSITION: the highlight did not move when the key was pressed. No
 * assertion about a single state can catch that, which is why it reached a
 * user.
 *
 * So this suite watches a sequence of observations and asserts on the
 * differences. It still asserts on the model rather than on pixels, so rule
 * 6 stands; it just adds the dimension the other two categories do not have.
 */

#include "../../unify/test/session/test-harness.hpp"

#include "../src/app/layouts.hpp"
#include "../src/model/model.hpp"
#include "../src/model/view.hpp"

#include <random>
#include <sstream>

namespace {

using namespace unify_test;
using namespace lens;

/**
 * Drives a model with keys and records what the user would perceive after
 * each one.
 *
 * Deliberately not a subprocess: the point is to run thousands of key
 * sequences, and an interaction bug that needs a pty to reproduce is an
 * interaction bug nobody will write a test for.
 */
class Driver {
public:
    struct Step {
        std::string key;
        ScrollView  scroll;
        bool        scrollValid = false;
        std::size_t tiles = 0;
        std::string focusedPanel;
        std::string message;
    };

    Driver( const char* layout = "browse", int w = 120, int h = 40 )
    {
        m_model.setGeometry( w, h );
        registerShellCommands( m_model );
        m_model.keymap() = defaultKeymap();
        applyStockLayout( m_model, layout );
        m_model.rebuildHelp();
        record( "<start>" );
    }

    Model& model() { return m_model; }

    /** Feed one key sequence, recording an observation per KEY. */
    void press( const std::string& keys )
    {
        KeySeq seq;
        if ( !parseKeySeq( keys, seq ) ) {
            UT_FAIL( "the test has a bad key sequence: '" << keys << "'" );
        }
        for ( const Key& key : seq ) {
            Event event;
            event.kind = Event::Kind::Key;
            event.key = key;
            ( void ) fold( m_model, event );
            record( keys );
        }
    }

    void pressChar( char32_t ch )
    {
        Event event;
        event.kind = Event::Kind::Key;
        event.key = Key::character( ch );
        ( void ) fold( m_model, event );
        record( encodeUtf8( ch ) );
    }

    const std::vector<Step>& steps() const { return m_steps; }
    const Step& last() const { return m_steps.back(); }
    const Step& previous() const { return m_steps[ m_steps.size() - 2 ]; }

    /** A readable film of the run, for a failure message. */
    std::string trace( std::size_t tail = 8 ) const
    {
        std::ostringstream os;
        const std::size_t from =
            m_steps.size() > tail ? m_steps.size() - tail : 0;
        for ( std::size_t i = from; i < m_steps.size(); ++i ) {
            const Step& step = m_steps[i];
            os << "\n          " << i << "  " << step.key
               << "  panel=" << step.focusedPanel
               << " tiles=" << step.tiles;
            if ( step.scrollValid ) {
                os << " line=" << step.scroll.cursorLine
                   << " top=" << step.scroll.topLine
                   << " row=" << step.scroll.cursorScreenRow()
                   << " rows=" << step.scroll.viewportRows
                   << " of=" << step.scroll.totalLines;
            }
        }
        return os.str();
    }

private:
    void record( const std::string& key )
    {
        Step step;
        step.key = key;
        step.scroll = m_model.observeFocusedScroll( step.scrollValid );
        step.tiles = m_model.layout().tileCount();
        step.message = m_model.message();

        const Buffer* focused = m_model.focusedBuffer();
        if ( focused ) {
            switch ( focused->kind ) {
            case PanelKind::Help:        step.focusedPanel = "Help"; break;
            case PanelKind::Menu:        step.focusedPanel = "Menu"; break;
            case PanelKind::Palette:     step.focusedPanel = "Palette"; break;
            case PanelKind::Transcript:  step.focusedPanel = "Transcript"; break;
            case PanelKind::Placeholder: step.focusedPanel = focused->title; break;
            }
        }
        m_steps.push_back( step );
    }

    Model m_model;
    std::vector<Driver::Step> m_steps;
};


/**
 * The invariants every scrolling panel owes, checked after one keystroke.
 *
 * Written once and applied to help, the menu and the palette -- and to the
 * six panels that do not exist yet, which is the point. Three copies of the
 * same scroll arithmetic is how this bug happened; one set of invariants
 * over one shared rule is how it stops happening.
 */
void checkScrollInvariants( const Driver& driver, const char* what )
{
    const Driver::Step& now = driver.last();
    const Driver::Step& before = driver.previous();

    if ( !now.scrollValid ) {
        return;
    }

    /* I1 -- the cursor is on screen. */
    UT_CHECK_MSG( now.scroll.cursorVisible(),
                  what << ": the cursor is off screen (row "
                       << now.scroll.cursorScreenRow() << " of "
                       << now.scroll.viewportRows << ")" << driver.trace() );

    /* The view never shows blank rows below content it could have shown. */
    if ( now.scroll.totalLines > now.scroll.viewportRows ) {
        UT_CHECK_MSG(
            now.scroll.topLine
                <= now.scroll.totalLines - now.scroll.viewportRows,
            what << ": scrolled past the end, leaving blank rows"
                 << driver.trace() );
    }
    UT_CHECK_MSG( now.scroll.topLine >= 0,
                  what << ": negative scroll" << driver.trace() );

    if ( !before.scrollValid
         || before.scroll.viewportRows != now.scroll.viewportRows ) {
        return;   /* the panel or its size changed; nothing to compare */
    }

    /*
     * I2 -- scrolling is MINIMAL. The view moves only when the cursor would
     * otherwise leave it. This is the invariant the old code broke: it
     * recomputed the offset from the cursor every frame, so the view moved
     * on every keystroke and the cursor never moved at all.
     */
    const int wouldStay = before.scroll.topLine;
    const bool cursorFitsWithoutMoving =
        now.scroll.cursorLine >= wouldStay
        && now.scroll.cursorLine < wouldStay + now.scroll.viewportRows
        && wouldStay <= std::max( 0, now.scroll.totalLines
                                         - now.scroll.viewportRows );

    if ( cursorFitsWithoutMoving ) {
        UT_CHECK_MSG( now.scroll.topLine == wouldStay,
                      what << ": the view scrolled from " << wouldStay
                           << " to " << now.scroll.topLine
                           << " although the cursor was already visible -- "
                              "so the highlight stayed put and the text moved"
                           << driver.trace() );
    }

    /*
     * I3 -- a cursor move is always visible. If the selected line changed,
     * either the highlight moved or the text did. Both staying put is the
     * user-visible symptom of "the key did nothing".
     */
    if ( before.scroll.cursorLine != now.scroll.cursorLine ) {
        const bool highlightMoved =
            before.scroll.cursorScreenRow() != now.scroll.cursorScreenRow();
        const bool textMoved = before.scroll.topLine != now.scroll.topLine;
        UT_CHECK_MSG( highlightMoved || textMoved,
                      what << ": the selection moved but nothing on screen did"
                           << driver.trace() );
    }
}

} // namespace

int main()
{
    Registry registry;

    // -- the reported bug --------------------------------------------------

    registry.add( "reversing direction takes effect on the first press", []() {
        Driver driver;
        driver.press( "F1" );

        /* Far enough down that the list is definitely scrolled. */
        for ( int i = 0; i < 25; ++i ) {
            driver.press( "Down" );
        }
        const Driver::Step scrolled = driver.last();
        UT_CHECK_MSG( scrolled.scrollValid, "help is not reporting a viewport" );
        UT_CHECK_MSG( scrolled.scroll.topLine > 0,
                      "the case needs a scrolled list; top is still 0" );

        driver.press( "Up" );

        const Driver::Step& after = driver.last();
        UT_CHECK_MSG( after.scroll.cursorLine == scrolled.scroll.cursorLine - 1,
                      "one Up should move the selection by exactly one line" );
        UT_CHECK_MSG(
            after.scroll.cursorScreenRow()
                == scrolled.scroll.cursorScreenRow() - 1,
            "ONE Up after scrolling down must move the highlight up one row. "
            "It did not, so the key looks dead and the user presses it again."
                << driver.trace() );
    } );

    registry.add( "every keystroke in a long walk keeps the invariants", []() {
        Driver driver;
        driver.press( "F1" );

        for ( int i = 0; i < 40; ++i ) {
            driver.press( "Down" );
            checkScrollInvariants( driver, "walking down" );
        }
        for ( int i = 0; i < 40; ++i ) {
            driver.press( "Up" );
            checkScrollInvariants( driver, "walking back up" );
        }
    } );

    registry.add( "down then up returns to exactly where it started", []() {
        Driver driver;
        driver.press( "F1" );

        bool valid = false;
        const ScrollView start = driver.model().observeFocusedScroll( valid );
        UT_CHECK( valid );

        for ( int i = 0; i < 12; ++i ) { driver.press( "Down" ); }
        for ( int i = 0; i < 12; ++i ) { driver.press( "Up" ); }

        const ScrollView end = driver.model().observeFocusedScroll( valid );
        UT_CHECK_MSG( end == start,
                      "twelve down and twelve up did not return to the "
                      "starting view" << driver.trace() );
    } );

    // -- the same invariants, on every scrolling panel ----------------------

    registry.add( "the menu obeys the scrolling invariants", []() {
        Driver driver( "browse", 80, 24 );   /* small, so it must scroll */
        driver.press( "F10" );

        for ( int i = 0; i < 20; ++i ) {
            driver.press( "Down" );
            checkScrollInvariants( driver, "menu down" );
        }
        for ( int i = 0; i < 20; ++i ) {
            driver.press( "Up" );
            checkScrollInvariants( driver, "menu up" );
        }
    } );

    registry.add( "the palette obeys the scrolling invariants", []() {
        Driver driver( "browse", 80, 24 );
        driver.press( "M-x" );

        for ( int i = 0; i < 20; ++i ) {
            driver.press( "Down" );
            checkScrollInvariants( driver, "palette down" );
        }
        for ( int i = 0; i < 20; ++i ) {
            driver.press( "Up" );
            checkScrollInvariants( driver, "palette up" );
        }
    } );

    // -- a random walk, which is where the NEXT bug gets caught -------------

    registry.add( "a seeded random walk never breaks an invariant", []() {
        /*
         * The layout suite's property test found a real bug no hand-written
         * case would have (ten tiles in eight rows). This is the same
         * technique pointed at interaction: press plausible keys, in
         * plausible orders, and assert the invariants after every one.
         *
         * Seeded, so a failure is replayable from the seed printed in the
         * message rather than being a story about a flaky test.
         */
        const char* keys[] = {
            "Down", "Up", "Enter", "Backspace", "Tab", "S-Tab",
            "F1", "F10", "M-x", "Esc", "C-x 2", "C-x 3", "C-x 0",
            "Left", "Right", "Home", "End"
        };
        const std::size_t keyCount = sizeof( keys ) / sizeof( keys[0] );

        for ( unsigned seed = 1; seed <= 30; ++seed ) {
            std::mt19937 rng( seed );
            Driver driver( ( seed % 2 ) ? "browse" : "run",
                           ( seed % 3 ) ? 120 : 80,
                           ( seed % 3 ) ? 40 : 24 );

            for ( int step = 0; step < 60; ++step ) {
                const std::size_t which = rng() % keyCount;
                driver.press( keys[which] );

                std::ostringstream what;
                what << "seed " << seed << " step " << step
                     << " key " << keys[which];
                checkScrollInvariants( driver, what.str().c_str() );

                /* Structural invariants that must hold whatever was pressed. */
                UT_CHECK_MSG( driver.model().layout().tileCount() >= 1,
                              what.str() << ": every tile was closed" );

                const Solution solution =
                    solve( driver.model().layout(),
                           driver.model().tileArea() );
                const std::string problem =
                    checkCoverage( solution, driver.model().tileArea() );
                UT_CHECK_MSG( problem.empty(),
                              what.str() << ": " << problem );
            }
        }
    } );

    registry.add( "a random walk never loses focus or the transcript", []() {
        const char* keys[] = {
            "Tab", "S-Tab", "C-x 2", "C-x 3", "C-x 0", "C-x 1",
            "F1", "F10", "Esc", "Down", "Up", "Enter"
        };
        const std::size_t keyCount = sizeof( keys ) / sizeof( keys[0] );

        for ( unsigned seed = 1; seed <= 20; ++seed ) {
            std::mt19937 rng( seed );
            Driver driver;

            for ( int step = 0; step < 50; ++step ) {
                driver.press( keys[ rng() % keyCount ] );

                std::ostringstream what;
                what << "seed " << seed << " step " << step;

                /*
                 * Focus must always name a tile that exists. A focus that
                 * points at a closed tile is invisible in any single frame
                 * and makes the next keystroke go nowhere.
                 */
                const std::vector<TileId> tiles =
                    driver.model().layout().tiles();
                const TileId focused = driver.model().layout().focused();
                bool found = false;
                for ( TileId tile : tiles ) {
                    if ( tile == focused ) { found = true; }
                }
                UT_CHECK_MSG( found,
                              what.str() << ": focus points at tile "
                                  << focused << ", which no longer exists"
                                  << driver.trace() );
            }
        }
    } );

    registry.add( "a modal panel always gives focus back", []() {
        /*
         * Open the menu, cancel it, and the layout must be exactly as it
         * was -- same tiles, same focus. A dialog that leaked a tile or
         * moved focus would accumulate damage over a session, which is
         * precisely the kind of thing a single-frame test cannot see.
         */
        for ( const char* opener : { "F10", "M-x" } ) {
            Driver driver;
            const std::size_t tiles = driver.model().layout().tileCount();
            const TileId focused = driver.model().layout().focused();

            for ( int repeat = 0; repeat < 5; ++repeat ) {
                driver.press( opener );
                driver.press( "Esc" );

                UT_CHECK_MSG( driver.model().layout().tileCount() == tiles,
                              opener << " leaked a tile after " << repeat + 1
                                     << " open/close cycles"
                                     << driver.trace() );
                UT_CHECK_MSG( driver.model().layout().focused() == focused,
                              opener << " did not give focus back"
                                     << driver.trace() );
            }
        }
    } );

    return registry.run( "lens interaction (what happens between frames)" ) == 0
               ? 0
               : 1;
}
