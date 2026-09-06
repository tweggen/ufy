/**
 * @file layout-test.cpp
 *
 * The tiling tree and the geometry solver -- ACCEPTANCE.md G1.2 and G1.3.
 *
 * These are model tests, not screen tests, which is rule 6 of the method:
 * "assert on the model; use grids for layout". A character grid embeds
 * content, so making it the oracle for tiling would turn every cosmetic
 * change into a dozen re-recordings. What actually has to be true of a
 * tiling solver -- it covers the area, it never overlaps, it is a function
 * of its inputs, and it does not damage the layout when the terminal gets
 * small -- is all assertable here, without a terminal.
 */

#include "../../unify/test/session/test-harness.hpp"

#include "../src/layout/layout-tree.hpp"
#include "../src/layout/solver.hpp"

#include <random>
#include <set>

namespace {

using namespace unify_test;
using namespace lens;

/** The tile area of a 120x40 terminal: menu bar, status line, hint line. */
Rect areaFor( int cols, int rows )
{
    return Rect( 0, 1, cols, rows - 3 );
}

/** Assert the solution covers `area` exactly, quoting the tree if not. */
void requireCoverage( const LayoutTree& tree, const Solution& solution,
                      const Rect& area, const char* what )
{
    const std::string problem = checkCoverage( solution, area );
    if ( !problem.empty() ) {
        UT_FAIL( what << ": " << problem
                 << "\n        tree: " << tree.describe()
                 << "\n        area: " << area.w << "x" << area.h
                 << " at (" << area.x << "," << area.y << ")" );
    }
}

} // namespace

int main()
{
    Registry registry;

    // -- the tree ----------------------------------------------------------

    registry.add( "a new tree is one focused tile", []() {
        LayoutTree tree( 7 );
        UT_CHECK_EQ( tree.tileCount(), std::size_t( 1 ) );
        UT_CHECK( tree.focused() != kNoTile );
        UT_CHECK_EQ( tree.bufferOf( tree.focused() ), BufferId( 7 ) );
        UT_CHECK_EQ( tree.focusOrder().size(), std::size_t( 1 ) );
    } );

    registry.add( "splitting focuses the new tile and keeps the old one", []() {
        LayoutTree tree( 1 );
        const TileId first = tree.focused();
        const TileId second = tree.splitFocused( Split::Columns, 2 );

        UT_CHECK( second != kNoTile );
        UT_CHECK( second != first );
        UT_CHECK_EQ( tree.tileCount(), std::size_t( 2 ) );
        UT_CHECK_EQ( tree.focused(), second );
        UT_CHECK_EQ( tree.bufferOf( first ), BufferId( 1 ) );
        UT_CHECK_EQ( tree.bufferOf( second ), BufferId( 2 ) );

        /* Most-recently-focused first, and every live tile appears once. */
        UT_CHECK_EQ( tree.focusOrder().size(), std::size_t( 2 ) );
        UT_CHECK_EQ( tree.focusOrder().front(), second );
    } );

    registry.add( "the last tile cannot be closed", []() {
        LayoutTree tree( 1 );
        UT_CHECK( !tree.closeFocused() );
        UT_CHECK_EQ( tree.tileCount(), std::size_t( 1 ) );
    } );

    registry.add( "closing gives the space back and focuses a live tile", []() {
        LayoutTree tree( 1 );
        const TileId first = tree.focused();
        tree.splitFocused( Split::Rows, 2 );
        UT_CHECK( tree.closeFocused() );

        UT_CHECK_EQ( tree.tileCount(), std::size_t( 1 ) );
        UT_CHECK_EQ( tree.focused(), first );
        UT_CHECK_EQ( tree.focusOrder().size(), std::size_t( 1 ) );

        /* And the surviving tile now owns the whole area. */
        const Rect area = areaFor( 120, 40 );
        const Solution s = solve( tree, area );
        UT_CHECK_EQ( s.placements.size(), std::size_t( 1 ) );
        UT_CHECK( s.placements[0].rect == area );
    } );

    registry.add( "maximise leaves exactly the focused tile", []() {
        LayoutTree tree( 1 );
        tree.splitFocused( Split::Columns, 2 );
        tree.splitFocused( Split::Rows, 3 );
        const TileId keep = tree.focused();

        UT_CHECK( tree.maximiseFocused() );
        UT_CHECK_EQ( tree.tileCount(), std::size_t( 1 ) );
        UT_CHECK_EQ( tree.focused(), keep );
        UT_CHECK_EQ( tree.focusOrder().size(), std::size_t( 1 ) );
        UT_CHECK( !tree.maximiseFocused() );   /* already maximal */
    } );

    registry.add( "focus cycles through every tile and wraps", []() {
        LayoutTree tree( 1 );
        tree.splitFocused( Split::Columns, 2 );
        tree.splitFocused( Split::Rows, 3 );

        const std::vector<TileId> order = tree.tiles();
        UT_CHECK_EQ( order.size(), std::size_t( 3 ) );

        tree.focus( order[0] );
        std::vector<TileId> visited;
        for ( std::size_t i = 0; i < order.size(); ++i ) {
            visited.push_back( tree.focused() );
            tree.focusNext();
        }
        UT_CHECK( visited == order );
        UT_CHECK_EQ( tree.focused(), order[0] );   /* wrapped */

        tree.focusPrev();
        UT_CHECK_EQ( tree.focused(), order.back() );
    } );

    // -- G1.2: coverage ----------------------------------------------------

    registry.add( "G1.2 one tile covers the area exactly", []() {
        LayoutTree tree( 1 );
        const Rect area = areaFor( 120, 40 );
        const Solution s = solve( tree, area );
        requireCoverage( tree, s, area, "single tile" );
        UT_CHECK_EQ( s.stubCount, std::size_t( 0 ) );
    } );

    registry.add( "G1.2 a row split stacks, a column split sits side by side",
                  []() {
        const Rect area = areaFor( 120, 40 );

        LayoutTree rows( 1 );
        const TileId a = rows.focused();
        const TileId b = rows.splitFocused( Split::Rows, 2 );
        const Solution sr = solve( rows, area );
        requireCoverage( rows, sr, area, "row split" );

        const Placement* pa = sr.find( a );
        const Placement* pb = sr.find( b );
        UT_CHECK( pa != NULL && pb != NULL );
        UT_CHECK_MSG( pa->rect.y < pb->rect.y,
                      "Rows must stack the first child above the second" );
        UT_CHECK_EQ( pa->rect.w, area.w );
        UT_CHECK_EQ( pb->rect.w, area.w );

        LayoutTree cols( 1 );
        const TileId c = cols.focused();
        const TileId d = cols.splitFocused( Split::Columns, 2 );
        const Solution sc = solve( cols, area );
        requireCoverage( cols, sc, area, "column split" );

        const Placement* pc = sc.find( c );
        const Placement* pd = sc.find( d );
        UT_CHECK( pc != NULL && pd != NULL );
        UT_CHECK_MSG( pc->rect.x < pd->rect.x,
                      "Columns must put the first child left of the second" );
        UT_CHECK_EQ( pc->rect.h, area.h );
        UT_CHECK_EQ( pd->rect.h, area.h );
    } );

    registry.add( "G1.2 coverage holds for random layouts at many geometries",
                  []() {
        /*
         * The property that actually matters, exercised over shapes nobody
         * would think to write by hand. Seeded, so a failure is reproducible
         * from the seed printed in the message rather than being a story
         * about a flaky test.
         */
        const int geometries[][2] = {
            { 80, 24 }, { 100, 30 }, { 120, 40 }, { 200, 60 }, { 81, 25 }
        };

        for ( unsigned seed = 1; seed <= 40; ++seed ) {
            std::mt19937 rng( seed );
            LayoutTree tree( 1 );

            const int steps = 3 + (int) ( rng() % 8 );
            for ( int step = 0; step < steps; ++step ) {
                switch ( rng() % 5 ) {
                case 0:
                case 1:
                    tree.splitFocused( Split::Rows,
                                       (BufferId) ( rng() % 8 ) + 1 );
                    break;
                case 2:
                case 3:
                    tree.splitFocused( Split::Columns,
                                       (BufferId) ( rng() % 8 ) + 1 );
                    break;
                default:
                    tree.focusNext();
                    break;
                }
            }

            for ( const auto& g : geometries ) {
                const Rect area = areaFor( g[0], g[1] );
                const Solution s = solve( tree, area );

                const std::string problem = checkCoverage( s, area );
                if ( !problem.empty() ) {
                    UT_FAIL( "seed " << seed << " at " << g[0] << "x" << g[1]
                             << ": " << problem
                             << "\n        tree: " << tree.describe() );
                }

                /* Every tile is accounted for: placed, stubbed or hidden. */
                UT_CHECK_MSG(
                    s.placements.size() + s.hidden.size() == tree.tileCount(),
                    "seed " << seed << ": " << s.placements.size()
                        << " placements + " << s.hidden.size() << " hidden for "
                        << tree.tileCount() << " tiles" );
                UT_CHECK_MSG( s.hidden.empty(),
                              "seed " << seed << " at " << g[0] << "x" << g[1]
                                  << ": tiles were hidden at a geometry lens "
                                     "agrees to run at" );
            }
        }
    } );

    // -- G1.3: degradation must not damage the layout ----------------------

    registry.add( "G1.3 resizing down and back restores the layout tree", []() {
        LayoutTree tree( 1 );
        tree.splitFocused( Split::Columns, 2 );
        tree.splitFocused( Split::Rows, 3 );
        tree.splitFocused( Split::Rows, 4 );
        tree.splitFocused( Split::Columns, 5 );

        const LayoutTree before = tree;

        const Rect big = areaFor( 120, 40 );
        const Rect small = areaFor( 80, 24 );

        const Solution atBig = solve( tree, big );
        requireCoverage( tree, atBig, big, "120x40" );

        /*
         * The assertion the criterion is written around. It is tree
         * EQUALITY, not "the model is still valid": a solver that made room
         * by editing the tree would pass the weaker reading and would have
         * silently thrown away the arrangement the user built.
         */
        UT_CHECK_MSG( tree == before,
                      "solving at 120x40 changed the tree\n        before: "
                          << before.describe() << "\n        after:  "
                          << tree.describe() );

        const Solution atSmall = solve( tree, small );
        requireCoverage( tree, atSmall, small, "80x24" );
        UT_CHECK_MSG( tree == before,
                      "solving at 80x24 changed the tree\n        before: "
                          << before.describe() << "\n        after:  "
                          << tree.describe() );

        const Solution backAtBig = solve( tree, big );
        requireCoverage( tree, backAtBig, big, "120x40 again" );
        UT_CHECK_MSG( tree == before, "solving back at 120x40 changed the tree" );

        /* And the restored geometry is identical, not merely valid. */
        UT_CHECK_EQ( backAtBig.placements.size(), atBig.placements.size() );
        for ( std::size_t i = 0; i < atBig.placements.size(); ++i ) {
            UT_CHECK_MSG( backAtBig.placements[i].tile == atBig.placements[i].tile
                          && backAtBig.placements[i].rect
                                 == atBig.placements[i].rect
                          && backAtBig.placements[i].stub
                                 == atBig.placements[i].stub,
                          "placement " << i << " differs after a resize round "
                          "trip -- the solver is not a pure function of its "
                          "inputs" );
        }
    } );

    registry.add( "G1.3 a small geometry demotes tiles rather than cramming",
                  []() {
        LayoutTree tree( 1 );
        for ( int i = 0; i < 5; ++i ) {
            tree.splitFocused( Split::Rows, (BufferId) ( i + 2 ) );
        }
        UT_CHECK_EQ( tree.tileCount(), std::size_t( 6 ) );

        const Rect small = areaFor( 80, 24 );   /* 21 rows for 6 tiles */
        const Solution s = solve( tree, small );
        requireCoverage( tree, s, small, "80x24 with six tiles" );

        UT_CHECK_MSG( s.stubCount > 0,
                      "six tiles in 21 rows must produce stubs; a 5-row "
                      "minimum cannot hold them all" );

        /* Stubs are exactly one row, and real tiles meet the minimum. */
        for ( std::size_t i = 0; i < s.placements.size(); ++i ) {
            const Placement& p = s.placements[i];
            if ( p.stub ) {
                UT_CHECK_EQ( p.rect.h, 1 );
            } else {
                UT_CHECK_MSG( p.rect.h >= 5 && p.rect.w >= 20,
                              "tile " << p.tile << " is " << p.rect.w << "x"
                                      << p.rect.h << ", below the 20x5 minimum" );
            }
        }
    } );

    registry.add( "G1.3 the focused tile is the last one demoted", []() {
        LayoutTree tree( 1 );
        for ( int i = 0; i < 5; ++i ) {
            tree.splitFocused( Split::Rows, (BufferId) ( i + 2 ) );
        }

        /* Focus the very first tile, making it the most recent. */
        const std::vector<TileId> order = tree.tiles();
        tree.focus( order.front() );

        const Solution s = solve( tree, areaFor( 80, 24 ) );
        const Placement* p = s.find( tree.focused() );
        UT_CHECK( p != NULL );
        UT_CHECK_MSG( !p->stub,
                      "the focused tile was demoted to a stub while other "
                      "tiles kept their space" );
    } );

    registry.add( "G1.3 demotion follows least-recently-focused order", []() {
        LayoutTree tree( 1 );
        for ( int i = 0; i < 5; ++i ) {
            tree.splitFocused( Split::Rows, (BufferId) ( i + 2 ) );
        }

        /*
         * Touch every tile in a known order, so the least recently focused
         * is unambiguous rather than an accident of how the tree was built.
         */
        const std::vector<TileId> order = tree.tiles();
        for ( std::size_t i = 0; i < order.size(); ++i ) {
            tree.focus( order[i] );
        }
        /* order[0] is now the least recently focused; order.back() the most. */

        const Solution s = solve( tree, areaFor( 80, 24 ) );
        UT_CHECK( s.stubCount > 0 );

        const Placement* oldest = s.find( order.front() );
        const Placement* newest = s.find( order.back() );
        UT_CHECK( oldest != NULL && newest != NULL );
        UT_CHECK_MSG( oldest->stub,
                      "the least recently focused tile should be demoted first" );
        UT_CHECK_MSG( !newest->stub,
                      "the most recently focused tile should be demoted last" );
    } );

    registry.add( "G1.3 at least one tile always survives", []() {
        LayoutTree tree( 1 );
        for ( int i = 0; i < 9; ++i ) {
            tree.splitFocused( Split::Rows, (BufferId) ( i + 2 ) );
        }

        /* Absurdly small, but the solver must still answer something sane. */
        const Rect tiny( 0, 1, 80, 8 );
        const Solution s = solve( tree, tiny );
        requireCoverage( tree, s, tiny, "8 rows with ten tiles" );

        UT_CHECK_MSG( s.stubCount < tree.tileCount(),
                      "every tile was demoted; something must remain visible" );
        UT_CHECK_MSG( !s.hidden.empty(),
                      "ten tiles cannot fit in eight rows, so some must be "
                      "reported hidden rather than silently given no space" );
        UT_CHECK_MSG( s.placements.size() + s.hidden.size() == tree.tileCount(),
                      "tiles went missing: " << s.placements.size()
                          << " placed + " << s.hidden.size() << " hidden != "
                          << tree.tileCount() );
    } );

    return registry.run( "lens layout (G1.2, G1.3)" ) == 0 ? 0 : 1;
}
