/**
 * @file solver.cpp
 *
 * See the header for the two guarantees. The implementation is in three
 * steps, and keeping them separate is what makes each one testable:
 *
 *   1. decide which tiles are stubs (a pure function of the focus order),
 *   2. give the stubs one row each at the bottom,
 *   3. divide what is left over the tree, skipping stubbed leaves.
 *
 * The temptation is to fuse them -- to demote a tile at the moment the
 * recursion notices it does not fit. That fails, and it fails subtly: the
 * recursion has only local knowledge, so it demotes whichever tile it
 * happens to reach first rather than the one the user cares least about,
 * and the answer stops being a function of the focus order.
 */

#include "solver.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace lens {

const Placement* Solution::find( TileId tile ) const
{
    for( std::size_t i = 0; i < placements.size(); ++i ) {
        if( placements[i].tile == tile ) {
            return &placements[i];
        }
    }
    return NULL;
}

namespace {

/** Leaves of a subtree that are not stubbed, in layout order. */
void activeLeaves( const LayoutTree& tree, int index,
                   const std::set<TileId>& stubs,
                   std::vector<TileId>& out )
{
    if( index < 0 ) {
        return;
    }
    const LayoutNode& n = tree.node( index );
    if( n.isLeaf ) {
        if( stubs.find( n.tile ) == stubs.end() ) {
            out.push_back( n.tile );
        }
        return;
    }
    activeLeaves( tree, n.first, stubs, out );
    activeLeaves( tree, n.second, stubs, out );
}


std::size_t activeCount( const LayoutTree& tree, int index,
                         const std::set<TileId>& stubs )
{
    std::vector<TileId> leaves;
    activeLeaves( tree, index, stubs, leaves );
    return leaves.size();
}


/**
 * The smallest area a subtree needs, counting only its active leaves.
 *
 * Rows stack, so heights add and widths take the maximum; Columns sit side
 * by side, so widths add and heights take the maximum. A subtree with no
 * active leaves needs nothing, which is what lets its sibling take the whole
 * space rather than half of it.
 */
Size minimumSize( const LayoutTree& tree, int index,
                  const std::set<TileId>& stubs, const SolverLimits& limits )
{
    if( index < 0 ) {
        return Size( 0, 0 );
    }
    const LayoutNode& n = tree.node( index );

    if( n.isLeaf ) {
        if( stubs.find( n.tile ) != stubs.end() ) {
            return Size( 0, 0 );
        }
        return Size( limits.minWidth, limits.minHeight );
    }

    const Size a = minimumSize( tree, n.first, stubs, limits );
    const Size b = minimumSize( tree, n.second, stubs, limits );

    if( 0 == a.w && 0 == a.h ) { return b; }
    if( 0 == b.w && 0 == b.h ) { return a; }

    if( Split::Rows == n.split ) {
        return Size( std::max( a.w, b.w ), a.h + b.h );
    }
    return Size( a.w + b.w, std::max( a.h, b.h ) );
}


/**
 * Split one extent in two, honouring both minimums and losing no cells.
 *
 * The rounding matters more than it looks: `total - firstSize` rather than a
 * second rounded computation is what guarantees the two halves add back up
 * to exactly the parent, which is most of the coverage guarantee.
 */
void divide( int total, double ratio, int minFirst, int minSecond,
             int& out_first, int& out_second )
{
    int first = (int) std::llround( (double) total * ratio );

    if( first < minFirst ) { first = minFirst; }
    if( total - first < minSecond ) { first = total - minSecond; }

    /* A degenerate parent cannot satisfy both; split it as evenly as it can. */
    if( first < 0 ) { first = 0; }
    if( first > total ) { first = total; }

    out_first = first;
    out_second = total - first;
}


void place( const LayoutTree& tree, int index, const Rect& rect,
            const std::set<TileId>& stubs, const SolverLimits& limits,
            std::vector<Placement>& out )
{
    if( index < 0 ) {
        return;
    }
    const LayoutNode& n = tree.node( index );

    if( n.isLeaf ) {
        if( stubs.find( n.tile ) == stubs.end() ) {
            Placement p;
            p.tile = n.tile;
            p.rect = rect;
            p.stub = false;
            out.push_back( p );
        }
        return;
    }

    const std::size_t activeFirst = activeCount( tree, n.first, stubs );
    const std::size_t activeSecond = activeCount( tree, n.second, stubs );

    /* A side with nothing to show yields all of its space to the other. */
    if( 0 == activeFirst ) {
        place( tree, n.second, rect, stubs, limits, out );
        return;
    }
    if( 0 == activeSecond ) {
        place( tree, n.first, rect, stubs, limits, out );
        return;
    }

    const Size minFirst = minimumSize( tree, n.first, stubs, limits );
    const Size minSecond = minimumSize( tree, n.second, stubs, limits );

    if( Split::Rows == n.split ) {
        int h1 = 0;
        int h2 = 0;
        divide( rect.h, n.ratio, minFirst.h, minSecond.h, h1, h2 );
        place( tree, n.first, Rect( rect.x, rect.y, rect.w, h1 ),
               stubs, limits, out );
        place( tree, n.second, Rect( rect.x, rect.y + h1, rect.w, h2 ),
               stubs, limits, out );
    } else {
        int w1 = 0;
        int w2 = 0;
        divide( rect.w, n.ratio, minFirst.w, minSecond.w, w1, w2 );
        place( tree, n.first, Rect( rect.x, rect.y, w1, rect.h ),
               stubs, limits, out );
        place( tree, n.second, Rect( rect.x + w1, rect.y, w2, rect.h ),
               stubs, limits, out );
    }
}

} // namespace

Solution solve( const LayoutTree& tree, const Rect& area,
                const SolverLimits& limits )
{
    Solution solution;

    const std::vector<TileId> all = tree.tiles();
    if( all.empty() || area.empty() ) {
        return solution;
    }

    /*
     * Step 1: choose the stubs.
     *
     * Demote the least recently focused tile until what remains fits, never
     * demoting the last one -- something has to be on screen -- and, because
     * the focus order has the current tile at its front, never demoting the
     * focused tile until it is the only candidate left.
     */
    std::set<TileId> stubs;
    std::vector<TileId> demotionOrder = tree.focusOrder();
    std::reverse( demotionOrder.begin(), demotionOrder.end() );

    /*
     * Every stub costs a row of the same area the remaining tiles have to
     * fit into, so there is a hard ceiling: one row must be left for the
     * tile that survives. Past that ceiling further tiles cannot be shown
     * at all, and saying so beats emitting a zero-height placement.
     */
    const std::size_t maxStubs =
        ( area.h > 1 ) ? (std::size_t)( area.h - 1 ) : 0;

    for( std::size_t i = 0; i < demotionOrder.size(); ++i ) {
        Rect usable = area;
        usable.h = area.h - (int) stubs.size();

        const Size need = minimumSize( tree, tree.rootIndex(), stubs, limits );
        if( need.fitsIn( usable ) ) {
            break;
        }
        if( stubs.size() + 1 >= all.size() ) {
            break;   /* keep at least one real tile */
        }
        if( stubs.size() >= maxStubs ) {
            break;   /* no room even for another stub row */
        }
        stubs.insert( demotionOrder[i] );
    }

    solution.stubCount = stubs.size();

    /* Step 2: the stub strip, one row each, at the bottom, in layout order. */
    Rect body = area;
    body.h = area.h - (int) stubs.size();
    if( body.h < 0 ) {
        body.h = 0;
    }

    /*
     * If the body still cannot hold every remaining tile at one row each,
     * the extra ones are hidden -- least recently focused first, the same
     * order demotion uses, so what disappears is predictable.
     */
    std::set<TileId> hidden;
    {
        std::vector<TileId> active;
        activeLeaves( tree, tree.rootIndex(), stubs, active );
        if( (int) active.size() > body.h ) {
            std::size_t excess = active.size() - (std::size_t) body.h;
            for( std::size_t i = 0; i < demotionOrder.size() && excess > 0; ++i ) {
                const TileId candidate = demotionOrder[i];
                if( stubs.find( candidate ) != stubs.end() ) { continue; }
                if( std::find( active.begin(), active.end(), candidate )
                        == active.end() ) { continue; }
                if( active.size() - hidden.size() <= 1 ) { break; }
                hidden.insert( candidate );
                --excess;
            }
        }
    }
    for( std::set<TileId>::const_iterator it = hidden.begin();
         it != hidden.end(); ++it ) {
        stubs.insert( *it );          /* excluded from the body's division */
        solution.hidden.push_back( *it );
    }

    /* Step 3: divide the body over the tree. */
    place( tree, tree.rootIndex(), body, stubs, limits, solution.placements );

    int stubRow = body.bottom();
    for( std::size_t i = 0; i < all.size(); ++i ) {
        if( stubs.find( all[i] ) == stubs.end() ) {
            continue;
        }
        if( hidden.find( all[i] ) != hidden.end() ) {
            continue;   /* no room for even a stub row */
        }
        Placement p;
        p.tile = all[i];
        p.rect = Rect( area.x, stubRow, area.w, 1 );
        p.stub = true;
        solution.placements.push_back( p );
        ++stubRow;
    }

    return solution;
}


std::string checkCoverage( const Solution& solution, const Rect& area )
{
    std::ostringstream problem;

    if( area.empty() ) {
        return solution.placements.empty()
            ? std::string()
            : std::string( "placements exist for an empty area" );
    }

    /* Nothing may leave the area, and nothing may be degenerate. */
    for( std::size_t i = 0; i < solution.placements.size(); ++i ) {
        const Rect& r = solution.placements[i].rect;
        if( r.w <= 0 || r.h <= 0 ) {
            problem << "tile " << solution.placements[i].tile
                    << " has empty geometry " << r.w << "x" << r.h;
            return problem.str();
        }
        if( r.x < area.x || r.y < area.y
            || r.right() > area.right() || r.bottom() > area.bottom() ) {
            problem << "tile " << solution.placements[i].tile
                    << " at (" << r.x << "," << r.y << " " << r.w << "x" << r.h
                    << ") leaves the area";
            return problem.str();
        }
    }

    /* No two may overlap. Quadratic, and the counts here are single digits. */
    for( std::size_t i = 0; i < solution.placements.size(); ++i ) {
        for( std::size_t j = i + 1; j < solution.placements.size(); ++j ) {
            if( solution.placements[i].rect.intersects(
                    solution.placements[j].rect ) ) {
                problem << "tiles " << solution.placements[i].tile << " and "
                        << solution.placements[j].tile << " overlap";
                return problem.str();
            }
        }
    }

    /*
     * And together they must cover every cell. Counting area would be
     * cheaper and would miss the case that matters -- a gap of exactly the
     * size of an overlap elsewhere -- so this walks the cells. The tile area
     * is a few thousand cells; correctness is worth the loop.
     */
    for( int y = area.y; y < area.bottom(); ++y ) {
        for( int x = area.x; x < area.right(); ++x ) {
            bool covered = false;
            for( std::size_t i = 0; i < solution.placements.size(); ++i ) {
                if( solution.placements[i].rect.contains( x, y ) ) {
                    covered = true;
                    break;
                }
            }
            if( !covered ) {
                problem << "cell (" << x << "," << y << ") is covered by no tile";
                return problem.str();
            }
        }
    }

    return std::string();
}

} // namespace lens
