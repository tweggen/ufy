#if !defined( _LENS_LAYOUT_SOLVER_HPP )
#define _LENS_LAYOUT_SOLVER_HPP

/**
 * @file solver.hpp
 *
 * Turns a LayoutTree plus an area into rectangles -- UI.md section 2,
 * ACCEPTANCE.md G1.2 and G1.3.
 *
 * The solver is a PURE FUNCTION of (tree, area, limits). It never mutates
 * the tree. Everything G1.3 asks for follows from that one property: a
 * resize down to 80x24 and back cannot lose the arrangement, because the
 * arrangement was never where the degradation happened.
 *
 * Two guarantees the tests assert structurally rather than by eye:
 *
 *   1. The placements COVER the area exactly -- every cell of it belongs to
 *      exactly one tile or stub. No gaps, no overlaps, nothing outside.
 *   2. The result is a deterministic function of its inputs, so a golden
 *      screen is reproducible and a bug is reproducible from a key script.
 */

#include "geometry.hpp"
#include "layout-tree.hpp"

#include <string>
#include <vector>

namespace lens {

/** Where one tile ended up. */
struct Placement {
    TileId tile = kNoTile;
    Rect   rect;

    /**
     * True when this tile was demoted to a one-line title stub because the
     * geometry could not hold it at full size.
     *
     * The panel is not rendered; the row shows the tile's title so the user
     * can still see it exists and Tab to it. UI.md section 2: "the solver
     * drops the least-recently-focused tile to a one-line title stub rather
     * than rendering a tile too small to be read."
     */
    bool   stub = false;
};

struct Solution {
    /** In layout order, stubs last. Every rect is non-empty. */
    std::vector<Placement> placements;

    /** How many tiles were demoted to stubs. 0 when everything fits. */
    std::size_t stubCount = 0;

    /**
     * Tiles that could not be shown at all -- not even as a one-row stub.
     *
     * Reached only when the area has fewer rows than the layout has tiles,
     * which is below the geometry lens agrees to run at (G1.4). It exists
     * because the alternative is worse: a placement with zero height would
     * satisfy "every tile has a placement" while being invisible, and a
     * renderer would draw nothing and say nothing. A tile that cannot be
     * shown is a fact the status line can report.
     */
    std::vector<TileId> hidden;

    const Placement* find( TileId tile ) const;
};

/** The smallest a tile may be before the solver demotes it (UI.md: 20x5). */
struct SolverLimits {
    int minWidth = 20;
    int minHeight = 5;
};

/**
 * Lay `tree` out inside `area`.
 *
 * When everything cannot fit at `limits`, tiles are demoted to stubs in
 * LEAST-RECENTLY-FOCUSED order -- the tile the user is least likely to be
 * looking at goes first -- and the focused tile is demoted last of all. A
 * stub takes one row at the bottom of the area.
 *
 * If even a single tile cannot fit, the area is too small for lens to draw
 * anything honest, and the caller is expected to have refused to start
 * (ACCEPTANCE.md G1.4). This function still returns a covering solution in
 * that case rather than an empty one, because a renderer that has to
 * special-case "no solution" gets that case wrong.
 */
Solution solve( const LayoutTree& tree, const Rect& area,
                const SolverLimits& limits = SolverLimits() );

/**
 * Check the coverage guarantee: every cell of `area` belongs to exactly one
 * placement, and no placement leaves it.
 *
 * Exposed rather than kept in the test file because it is cheap, total, and
 * exactly the invariant a future solver change is most likely to break --
 * so the golden-screen harness asserts it on every frame it renders, not
 * only in the layout tests.
 *
 * @return an empty string when the solution is sound, or a description of
 *     the first problem found.
 */
std::string checkCoverage( const Solution& solution, const Rect& area );

} // namespace lens

#endif // _LENS_LAYOUT_SOLVER_HPP
