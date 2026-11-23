#if !defined( _LENS_LAYOUT_TREE_HPP )
#define _LENS_LAYOUT_TREE_HPP

/**
 * @file layout-tree.hpp
 *
 * The tiling tree -- UI.md section 2.
 *
 * A binary tree of splits with fractional weights, in the Oberon spirit:
 * non-overlapping, resizable, and always covering the tile area exactly.
 * No overlapping windows, no floating palettes; dialogs are tiles too, which
 * is what makes "a dialog cannot land off-screen or clip at 80x24" true by
 * construction rather than by care.
 *
 * THE TREE IS NEVER TOUCHED BY THE SOLVER. That is the single most important
 * property in this file, and it is what makes ACCEPTANCE.md G1.3 -- resize
 * 120x40 down to 80x24 and back, and get the same layout tree -- true by
 * construction instead of by a best effort to reconstruct what was thrown
 * away. Degradation at a small geometry is a property of one call to
 * `solve()`, not a mutation of the layout; the tree a user built is the tree
 * they still have. The criterion is written as tree EQUALITY precisely
 * because the naive reading ("the model is identical") is false for any
 * solver that edits the model to make things fit.
 */

#include "geometry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lens {

/** Stable identity of a tile (a leaf). Never reused within a session. */
using TileId = std::uint32_t;

/** What a tile is currently showing. Tiles and buffers are decoupled. */
using BufferId = std::uint32_t;

constexpr TileId   kNoTile = 0;
constexpr BufferId kNoBuffer = 0;

/**
 * How a split arranges its two children.
 *
 * Named for what you SEE rather than for the direction of the divider,
 * because "a horizontal split" is ambiguous in every codebase that has ever
 * used the phrase -- half of them mean a horizontal divider, the other half
 * mean side-by-side children.
 *
 *   Rows    children stacked one above the other  (Emacs C-x 2)
 *   Columns children side by side                 (Emacs C-x 3)
 */
enum class Split {
    Rows,
    Columns
};

/**
 * One node. Leaves carry a tile; splits carry a ratio and two children.
 *
 * Stored in an arena and referred to by index rather than by pointer, so a
 * tree copies with its default copy constructor and compares without any
 * pointer chasing -- both of which the golden tests do constantly.
 */
struct LayoutNode {
    bool     isLeaf = true;

    /* Leaf. */
    TileId   tile = kNoTile;
    BufferId buffer = kNoBuffer;

    /* Split. */
    Split  split = Split::Rows;
    double ratio = 0.5;    //!< share of the parent given to `first`
    int    first = -1;
    int    second = -1;
};

class LayoutTree {
public:
    /** A tree of exactly one tile showing `buffer`. */
    explicit LayoutTree( BufferId buffer = kNoBuffer );

    // -- structure ---------------------------------------------------------

    /**
     * Split the focused tile, putting `buffer` in the new one, and focus it.
     *
     * @param ratio share of the space given to the EXISTING tile.
     * @return the new tile's id.
     */
    TileId splitFocused( Split split, BufferId buffer, double ratio = 0.5 );

    /**
     * Close the focused tile; its space goes to its sibling. A no-op when
     * only one tile is left -- an environment with no tiles has nothing to
     * show and no way back, so the last one is not closable.
     *
     * @return true if a tile was actually closed.
     */
    bool closeFocused();

    /**
     * Discard every other tile, leaving the focused one alone -- Emacs
     * `C-x 1`, and destructive in the same way: the arrangement is gone, not
     * hidden. A non-destructive zoom would be a different command with a
     * different binding, and pretending one is the other is how users lose
     * layouts they spent time on.
     *
     * @return true if anything was discarded.
     */
    bool maximiseFocused();

    // -- focus -------------------------------------------------------------

    TileId focused() const { return m_focused; }

    /** Focus a tile by id. False if there is no such tile. */
    bool focus( TileId tile );

    /** Move focus to the next / previous tile in left-to-right, top-to-bottom
     *  order, wrapping. */
    void focusNext();
    void focusPrev();

    /**
     * Tiles in most-recently-focused-first order.
     *
     * The solver needs this, and only this, to decide what to demote when
     * the geometry cannot hold everything: the least recently focused tile
     * is the one the user is least likely to be looking at.
     */
    const std::vector<TileId>& focusOrder() const { return m_focusOrder; }

    // -- inspection --------------------------------------------------------

    /** Tiles in layout order (left to right, top to bottom). */
    std::vector<TileId> tiles() const;

    std::size_t tileCount() const;

    /** The buffer a tile shows, or kNoBuffer. */
    BufferId bufferOf( TileId tile ) const;

    /** Show a different buffer in a tile (UI.md's `C-x b`). */
    bool setBuffer( TileId tile, BufferId buffer );

    int rootIndex() const { return m_root; }
    const LayoutNode& node( int index ) const { return m_nodes[ (std::size_t) index ]; }

    /**
     * Structural equality: same shape, same tiles, same buffers, same
     * ratios, same focus and same focus order.
     *
     * Deliberately NOT index equality. Arena indices are an implementation
     * detail that closing a tile can renumber, and a comparison that
     * noticed would make G1.3 fail for a reason that has nothing to do with
     * what the user sees.
     */
    bool operator == ( const LayoutTree& other ) const;
    bool operator != ( const LayoutTree& other ) const { return !( *this == other ); }

    /** A one-line shape description, for test failure messages. */
    std::string describe() const;

private:
    int  allocate( const LayoutNode& node );
    int  findLeaf( TileId tile ) const;
    int  parentOf( int index ) const;
    void collectLeaves( int index, std::vector<TileId>& out ) const;
    void touchFocus( TileId tile );
    void forgetTile( TileId tile );
    bool sameSubtree( int a, const LayoutTree& other, int b ) const;
    void describeInto( int index, std::string& out ) const;

    /**
     * Rebuild the arena so it holds exactly the reachable nodes.
     *
     * Called after a close or a maximise. Without it the arena grows
     * forever across a long session of splits and closes, and -- more to the
     * point -- two trees that are structurally identical could hold
     * different amounts of garbage, which is invisible to operator== but
     * shows up as unbounded memory.
     */
    void compact();
    int  copySubtree( int index, const LayoutTree& from,
                      std::vector<LayoutNode>& into ) const;

    std::vector<LayoutNode> m_nodes;
    int      m_root = -1;
    TileId   m_focused = kNoTile;
    TileId   m_nextTile = 1;

    /// Most-recently-focused first. Every live tile appears exactly once.
    std::vector<TileId> m_focusOrder;
};

} // namespace lens

#endif // _LENS_LAYOUT_TREE_HPP
