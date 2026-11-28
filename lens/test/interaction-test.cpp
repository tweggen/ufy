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

#include "../src/app/driver.hpp"
#include "../src/app/layouts.hpp"
#include "../src/model/model.hpp"
#include "../src/model/view.hpp"

#include <random>
#include <sstream>

namespace {

using namespace unify_test;
using namespace lens;

/*
 * The Driver used to live here. It moved to src/app/driver.hpp when the
 * `--spec` runner needed the same apparatus: two callers with two ideas of
 * what "the cursor is on line 3" means would let a spec case and a C++ case
 * with the same words in them disagree, and nobody would know which was
 * right. So there is one Driver, and `press` reports a bad key sequence by
 * returning false rather than by knowing about this harness.
 */
void pressOrFail( Driver& driver, const std::string& keys )
{
    if ( !driver.press( keys ) ) {
        UT_FAIL( "the test has a bad key sequence: '" << keys << "'" );
    }
}


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

/** Give the results panels something to scroll: a query with `n` rows. */
void populateResults( Driver& driver, std::uint64_t n )
{
    Model& model = driver.model();
    model.beginQuery( 41 );
    for ( std::uint64_t i = 0; i < n; ++i ) {
        us::Value wide;
        wide.kind = us::Value::Kind::Array;
        for ( int k = 0; k < 30; ++k ) {
            us::Value element;
            element.kind = us::Value::Kind::Int;
            element.i = (std::int64_t)( i * 100 + (std::uint64_t) k );
            wide.args.push_back( element );
        }
        us::Solution solution;
        solution.index = i;
        solution.bindings.push_back( std::make_pair( std::string( "$row" ), wide ) );
        us::Event event;
        event.header.query = 41;
        event.body = solution;
        model.foldSession( event );
    }
    us::QueryStatus status;
    status.state = us::QueryStatus::State::Running;
    status.produced = n;
    us::Event done;
    done.header.query = 41;
    done.body = status;
    model.foldSession( done );

    /* A catalogue too, and a long definition in Source. */
    us::Listing listing;
    for ( std::uint64_t i = 0; i < n; ++i ) {
        us::CatalogueEntry entry;
        std::ostringstream name;
        name << "p" << i;
        entry.key.name = name.str();
        entry.key.arity = 1;
        entry.key.module = 1;
        entry.origin.kind = ( i % 5 == 0 ) ? us::Origin::Kind::Builtin
                                           : us::Origin::Kind::Transcript;
        entry.clauseCount = 1;
        listing.entries.push_back( entry );
    }
    us::Event listed;
    listed.body = listing;
    model.foldSession( listed );

    SourceDefinition& definition = model.sourceDefinition();
    definition.set = true;
    definition.key.name = "p1";
    definition.key.arity = 1;
    definition.lines.clear();
    for ( std::uint64_t i = 0; i < n; ++i ) {
        definition.lines.push_back( "p1( x ) { line; }" );
    }
    model.setSourceShows( Model::SourceShows::Definition );

    for ( std::uint64_t i = 0; i < n; ++i ) {
        us::Diagnostic diagnostic;
        diagnostic.file = "<session>";
        diagnostic.line = (std::uint32_t)( i + 1 );
        diagnostic.column = 1;
        diagnostic.message = "parse error";
        us::Event event;
        event.body = diagnostic;
        model.foldSession( event );
    }
}

void focusTitled( Driver& driver, const std::string& title )
{
    for ( int i = 0; i < 24 && driver.last().focusedPanel != title; ++i ) {
        pressOrFail( driver, "Tab" );
    }
    UT_CHECK_EQ( driver.last().focusedPanel, title );
}

} // namespace

int main()
{
    Registry registry;

    // -- the reported bug --------------------------------------------------

    registry.add( "reversing direction takes effect on the first press", []() {
        Driver driver;
        pressOrFail( driver, "F1" );

        /* Far enough down that the list is definitely scrolled. */
        for ( int i = 0; i < 25; ++i ) {
            pressOrFail( driver, "Down" );
        }
        const Driver::Step scrolled = driver.last();
        UT_CHECK_MSG( scrolled.scrollValid, "help is not reporting a viewport" );
        UT_CHECK_MSG( scrolled.scroll.topLine > 0,
                      "the case needs a scrolled list; top is still 0" );

        pressOrFail( driver, "Up" );

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
        pressOrFail( driver, "F1" );

        for ( int i = 0; i < 40; ++i ) {
            pressOrFail( driver, "Down" );
            checkScrollInvariants( driver, "walking down" );
        }
        for ( int i = 0; i < 40; ++i ) {
            pressOrFail( driver, "Up" );
            checkScrollInvariants( driver, "walking back up" );
        }
    } );

    registry.add( "down then up returns to exactly where it started", []() {
        Driver driver;
        pressOrFail( driver, "F1" );

        bool valid = false;
        const ScrollView start = driver.model().observeFocusedScroll( valid );
        UT_CHECK( valid );

        for ( int i = 0; i < 12; ++i ) { pressOrFail( driver, "Down" ); }
        for ( int i = 0; i < 12; ++i ) { pressOrFail( driver, "Up" ); }

        const ScrollView end = driver.model().observeFocusedScroll( valid );
        UT_CHECK_MSG( end == start,
                      "twelve down and twelve up did not return to the "
                      "starting view" << driver.trace() );
    } );

    // -- the same invariants, on every scrolling panel ----------------------

    registry.add( "the menu obeys the scrolling invariants", []() {
        /* The longest drop-down, at the smallest geometry. It wraps, so
         * twenty presses go round it more than once. */
        Driver driver( "browse", 80, 24 );
        pressOrFail( driver, "M-n" );

        for ( int i = 0; i < 20; ++i ) {
            pressOrFail( driver, "Down" );
            checkScrollInvariants( driver, "menu down" );
        }
        for ( int i = 0; i < 20; ++i ) {
            pressOrFail( driver, "Up" );
            checkScrollInvariants( driver, "menu up" );
        }
    } );

    registry.add( "the palette obeys the scrolling invariants", []() {
        Driver driver( "browse", 80, 24 );
        pressOrFail( driver, "M-x" );

        for ( int i = 0; i < 20; ++i ) {
            pressOrFail( driver, "Down" );
            checkScrollInvariants( driver, "palette down" );
        }
        for ( int i = 0; i < 20; ++i ) {
            pressOrFail( driver, "Up" );
            checkScrollInvariants( driver, "palette up" );
        }
    } );

    // -- a random walk, which is where the NEXT bug gets caught -------------

    registry.add( "the transcript cursor obeys the scrolling invariants", []() {
        /*
         * G2.5 gave the transcript a cursor over past input, and with it the
         * same problem every list panel has. Forty lines in an 80x24 run
         * layout is several screens of them, which is the case where a
         * cursor-only offset would have pinned the highlight to an edge.
         */
        Driver driver( "run", 80, 24 );
        for ( int i = 0; i < 16 && driver.last().focusedPanel != "Transcript"; ++i ) {
            pressOrFail( driver, "Tab" );
        }
        UT_CHECK_EQ( driver.last().focusedPanel, std::string( "Transcript" ) );

        for ( int i = 0; i < 40; ++i ) {
            std::ostringstream line;
            line << "line( " << i << " );";
            for ( char c : line.str() ) {
                driver.pressChar( (char32_t) (unsigned char) c );
            }
            pressOrFail( driver, "Enter" );
        }

        for ( int i = 0; i < 45; ++i ) {
            pressOrFail( driver, "PageUp" );
            UT_CHECK_MSG( driver.last().scrollValid,
                          "the transcript is not reporting a viewport" );
            checkScrollInvariants( driver, "transcript up" );
        }
        for ( int i = 0; i < 39; ++i ) {
            pressOrFail( driver, "PageDown" );
            checkScrollInvariants( driver, "transcript down" );
        }
        /* And one more leaves the past for the prompt. */
        pressOrFail( driver, "PageDown" );
        UT_CHECK_MSG( !driver.last().scrollValid,
                      "at the prompt the transcript has no line cursor" );
    } );

    registry.add( "the list panels (G3, G5) obey the scrolling invariants", []() {
        /*
         * Solutions, the Inspector and Diagnostics are three more lists, and
         * they get the same invariants rather than a copy of them -- which
         * is the point of having written them once. 80x24, so each is a few
         * rows tall and every key near an edge scrolls.
         */
        struct Case { const char* layout; const char* panel; const char* open; };
        const Case cases[] = {
            { "run",    "Solutions",   "" },
            { "debug",  "Inspector",   "Right" },
            { "browse", "Diagnostics", "" },
            { "browse", "Catalogue",   "" },
            { "browse", "Source",      "" },
        };
        for ( const Case& c : cases ) {
            Driver driver( c.layout, 80, 24 );
            populateResults( driver, 60 );
            focusTitled( driver, c.panel );
            if ( *c.open ) {
                pressOrFail( driver, c.open );
            }
            const std::string what = std::string( c.panel );
            for ( const char* key : { "Down", "Up", "PageDown", "PageUp" } ) {
                for ( int i = 0; i < 70; ++i ) {
                    pressOrFail( driver, key );
                    UT_CHECK_MSG( driver.last().scrollValid,
                                  what << " is not reporting a viewport" );
                    checkScrollInvariants( driver, ( what + " " + key ).c_str() );
                }
            }
            pressOrFail( driver, "End" );
            checkScrollInvariants( driver, ( what + " End" ).c_str() );
            pressOrFail( driver, "Home" );
            checkScrollInvariants( driver, ( what + " Home" ).c_str() );
        }
    } );

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
            "Left", "Right", "Home", "End", "M-f", "M-n", "M-h",
            "PageUp", "PageDown", "F5", "C-c"
        };
        const std::size_t keyCount = sizeof( keys ) / sizeof( keys[0] );

        for ( unsigned seed = 1; seed <= 30; ++seed ) {
            std::mt19937 rng( seed );
            const char* layouts[] = { "browse", "run", "full", "debug" };
            Driver driver( layouts[ seed % 4 ],
                           ( seed % 3 ) ? 120 : 80,
                           ( seed % 3 ) ? 40 : 24 );
            /* Something in every results panel, so its keys have work. */
            populateResults( driver, 25 );

            for ( int step = 0; step < 60; ++step ) {
                const std::size_t which = rng() % keyCount;
                pressOrFail( driver, keys[which] );

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
                pressOrFail( driver, keys[ rng() % keyCount ] );

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
                pressOrFail( driver, opener );
                pressOrFail( driver, "Esc" );

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
