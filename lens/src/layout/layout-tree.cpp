/**
 * @file layout-tree.cpp
 *
 * See the header for why the solver never touches this structure.
 */

#include "layout-tree.hpp"

#include <algorithm>
#include <sstream>

namespace lens {

LayoutTree::LayoutTree( BufferId buffer )
{
    LayoutNode leaf;
    leaf.isLeaf = true;
    leaf.tile = m_nextTile++;
    leaf.buffer = buffer;

    m_root = allocate( leaf );
    m_focused = leaf.tile;
    m_focusOrder.push_back( leaf.tile );
}


int LayoutTree::allocate( const LayoutNode& node )
{
    m_nodes.push_back( node );
    return (int) m_nodes.size() - 1;
}


int LayoutTree::findLeaf( TileId tile ) const
{
    for( std::size_t i = 0; i < m_nodes.size(); ++i ) {
        if( m_nodes[i].isLeaf && m_nodes[i].tile == tile ) {
            return (int) i;
        }
    }
    return -1;
}


int LayoutTree::parentOf( int index ) const
{
    for( std::size_t i = 0; i < m_nodes.size(); ++i ) {
        if( !m_nodes[i].isLeaf
            && ( m_nodes[i].first == index || m_nodes[i].second == index ) ) {
            return (int) i;
        }
    }
    return -1;
}


void LayoutTree::collectLeaves( int index, std::vector<TileId>& out ) const
{
    if( index < 0 ) {
        return;
    }
    const LayoutNode& n = m_nodes[ (std::size_t) index ];
    if( n.isLeaf ) {
        out.push_back( n.tile );
        return;
    }
    collectLeaves( n.first, out );
    collectLeaves( n.second, out );
}


std::vector<TileId> LayoutTree::tiles() const
{
    std::vector<TileId> out;
    collectLeaves( m_root, out );
    return out;
}


std::size_t LayoutTree::tileCount() const
{
    return tiles().size();
}


void LayoutTree::touchFocus( TileId tile )
{
    m_focusOrder.erase(
        std::remove( m_focusOrder.begin(), m_focusOrder.end(), tile ),
        m_focusOrder.end() );
    m_focusOrder.insert( m_focusOrder.begin(), tile );
}


void LayoutTree::forgetTile( TileId tile )
{
    m_focusOrder.erase(
        std::remove( m_focusOrder.begin(), m_focusOrder.end(), tile ),
        m_focusOrder.end() );
}


TileId LayoutTree::splitFocused( Split split, BufferId buffer, double ratio )
{
    const int leafIndex = findLeaf( m_focused );
    if( leafIndex < 0 ) {
        return kNoTile;
    }

    /*
     * Clamped rather than trusted. A ratio outside (0,1) produces a child
     * with zero or negative extent, which the solver would then have to
     * defend against on every recursion; clamping once here means the
     * invariant "both children are non-degenerate" holds everywhere else.
     */
    if( ratio < 0.05 ) { ratio = 0.05; }
    if( ratio > 0.95 ) { ratio = 0.95; }

    const LayoutNode existing = m_nodes[ (std::size_t) leafIndex ];

    LayoutNode fresh;
    fresh.isLeaf = true;
    fresh.tile = m_nextTile++;
    fresh.buffer = buffer;

    const int firstIndex = allocate( existing );
    const int secondIndex = allocate( fresh );

    /* Reuse the old leaf's slot for the split, so no parent needs updating. */
    LayoutNode& slot = m_nodes[ (std::size_t) leafIndex ];
    slot = LayoutNode();
    slot.isLeaf = false;
    slot.split = split;
    slot.ratio = ratio;
    slot.first = firstIndex;
    slot.second = secondIndex;

    m_focused = fresh.tile;
    touchFocus( fresh.tile );
    return fresh.tile;
}


bool LayoutTree::closeFocused()
{
    const int leafIndex = findLeaf( m_focused );
    if( leafIndex < 0 ) {
        return false;
    }

    const int parent = parentOf( leafIndex );
    if( parent < 0 ) {
        /* The root is the only tile. Not closable: see the header. */
        return false;
    }

    const LayoutNode& p = m_nodes[ (std::size_t) parent ];
    const int siblingIndex = ( p.first == leafIndex ) ? p.second : p.first;

    /* The sibling subtree takes the parent's place. */
    const LayoutNode sibling = m_nodes[ (std::size_t) siblingIndex ];

    forgetTile( m_focused );

    /* Splice: overwrite the parent slot with the sibling subtree. */
    m_nodes[ (std::size_t) parent ] = sibling;

    compact();

    /*
     * Focus the most recently focused tile that still exists. Falling back
     * to "the first tile" would move focus somewhere arbitrary after a
     * close, which is precisely the moment a user is least able to guess
     * where it went.
     */
    const std::vector<TileId> live = tiles();
    m_focused = live.empty() ? kNoTile : live.front();
    for( std::size_t i = 0; i < m_focusOrder.size(); ++i ) {
        if( std::find( live.begin(), live.end(), m_focusOrder[i] ) != live.end() ) {
            m_focused = m_focusOrder[i];
            break;
        }
    }
    if( kNoTile != m_focused ) {
        touchFocus( m_focused );
    }
    return true;
}


bool LayoutTree::maximiseFocused()
{
    const int leafIndex = findLeaf( m_focused );
    if( leafIndex < 0 || leafIndex == m_root ) {
        return false;
    }

    const LayoutNode keep = m_nodes[ (std::size_t) leafIndex ];

    m_nodes.clear();
    m_root = allocate( keep );

    m_focusOrder.clear();
    m_focusOrder.push_back( keep.tile );
    m_focused = keep.tile;
    return true;
}


bool LayoutTree::focus( TileId tile )
{
    if( findLeaf( tile ) < 0 ) {
        return false;
    }
    m_focused = tile;
    touchFocus( tile );
    return true;
}


void LayoutTree::focusNext()
{
    const std::vector<TileId> order = tiles();
    if( order.size() < 2 ) {
        return;
    }
    for( std::size_t i = 0; i < order.size(); ++i ) {
        if( order[i] == m_focused ) {
            focus( order[ ( i + 1 ) % order.size() ] );
            return;
        }
    }
    focus( order.front() );
}


void LayoutTree::focusPrev()
{
    const std::vector<TileId> order = tiles();
    if( order.size() < 2 ) {
        return;
    }
    for( std::size_t i = 0; i < order.size(); ++i ) {
        if( order[i] == m_focused ) {
            focus( order[ ( i + order.size() - 1 ) % order.size() ] );
            return;
        }
    }
    focus( order.front() );
}


BufferId LayoutTree::bufferOf( TileId tile ) const
{
    const int index = findLeaf( tile );
    return index < 0 ? kNoBuffer : m_nodes[ (std::size_t) index ].buffer;
}


bool LayoutTree::setBuffer( TileId tile, BufferId buffer )
{
    const int index = findLeaf( tile );
    if( index < 0 ) {
        return false;
    }
    m_nodes[ (std::size_t) index ].buffer = buffer;
    return true;
}

// ---------------------------------------------------------------------------
// Compaction
// ---------------------------------------------------------------------------

int LayoutTree::copySubtree( int index, const LayoutTree& from,
                             std::vector<LayoutNode>& into ) const
{
    const LayoutNode& src = from.m_nodes[ (std::size_t) index ];
    if( src.isLeaf ) {
        into.push_back( src );
        return (int) into.size() - 1;
    }

    /*
     * Children first, then the parent: the parent's indices must be known
     * before it is appended, and appending it first would need patching
     * afterwards.
     */
    const int first = copySubtree( src.first, from, into );
    const int second = copySubtree( src.second, from, into );

    LayoutNode copy = src;
    copy.first = first;
    copy.second = second;
    into.push_back( copy );
    return (int) into.size() - 1;
}


void LayoutTree::compact()
{
    if( m_root < 0 ) {
        return;
    }
    std::vector<LayoutNode> rebuilt;
    const int newRoot = copySubtree( m_root, *this, rebuilt );
    m_nodes.swap( rebuilt );
    m_root = newRoot;
}

// ---------------------------------------------------------------------------
// Comparison and description
// ---------------------------------------------------------------------------

bool LayoutTree::sameSubtree( int a, const LayoutTree& other, int b ) const
{
    if( a < 0 || b < 0 ) {
        return a == b;
    }
    const LayoutNode& na = m_nodes[ (std::size_t) a ];
    const LayoutNode& nb = other.m_nodes[ (std::size_t) b ];

    if( na.isLeaf != nb.isLeaf ) {
        return false;
    }
    if( na.isLeaf ) {
        return na.tile == nb.tile && na.buffer == nb.buffer;
    }
    if( na.split != nb.split ) {
        return false;
    }
    /*
     * Ratios are doubles that survive a round trip through the solver
     * untouched, so exact comparison would be defensible -- but a tolerance
     * costs nothing and means a future solver that normalises a ratio does
     * not silently fail an equality test that is really about SHAPE.
     */
    if( na.ratio < nb.ratio - 1e-9 || na.ratio > nb.ratio + 1e-9 ) {
        return false;
    }
    return sameSubtree( na.first, other, nb.first )
        && sameSubtree( na.second, other, nb.second );
}


bool LayoutTree::operator == ( const LayoutTree& other ) const
{
    return m_focused == other.m_focused
        && m_focusOrder == other.m_focusOrder
        && sameSubtree( m_root, other, other.m_root );
}


void LayoutTree::describeInto( int index, std::string& out ) const
{
    if( index < 0 ) {
        out += "-";
        return;
    }
    const LayoutNode& n = m_nodes[ (std::size_t) index ];
    if( n.isLeaf ) {
        std::ostringstream os;
        os << "t" << n.tile << ":b" << n.buffer;
        if( n.tile == m_focused ) {
            os << "*";
        }
        out += os.str();
        return;
    }

    out += ( n.split == Split::Rows ) ? "rows(" : "cols(";
    describeInto( n.first, out );
    out += " ";
    describeInto( n.second, out );
    out += ")";
}


std::string LayoutTree::describe() const
{
    std::string out;
    describeInto( m_root, out );
    return out;
}

} // namespace lens
