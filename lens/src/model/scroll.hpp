#if !defined( _LENS_MODEL_SCROLL_HPP )
#define _LENS_MODEL_SCROLL_HPP

/**
 * @file scroll.hpp
 *
 * Where a scrolling panel's viewport sits, and the one function that decides
 * it.
 *
 * Every panel that shows a list longer than its tile -- help, the menu, the
 * palette, the transcript, and every panel still to come -- has the same
 * problem and had, until this file existed, three copies of the same wrong
 * answer. The wrong answer is to compute the scroll offset from the cursor
 * alone:
 *
 *     top = ( cursor >= rows ) ? cursor - rows + 1 : 0;
 *
 * That is self-consistent in any single frame, which is why a golden screen
 * cannot see it is wrong, and why it survived. It is wrong ACROSS frames: it
 * pins the cursor to the last visible row for as long as the list is
 * scrolled at all, so moving the cursor up scrolls the text and never moves
 * the highlight. To a user that reads as "the key did nothing".
 *
 * The right answer needs the PREVIOUS viewport: scroll only when the cursor
 * would otherwise leave it, and then by the least amount. That is one line
 * of arithmetic and one piece of remembered state, and it is the whole of
 * this file.
 */

#include <algorithm>

namespace lens {

/**
 * Where a scrolling panel is, as four numbers.
 *
 * The projection interaction tests assert on. Deliberately not the grid:
 * these are what the user perceives -- which line is selected, where it
 * appears on screen, how much of the list is showing -- with none of the
 * detail that makes a screen golden re-record on every cosmetic change.
 */
struct ScrollView {
    int cursorLine = 0;      //!< the selected item, indexed into the content
    int topLine = 0;         //!< the first line rendered
    int viewportRows = 0;    //!< how many rows the tile has for content
    int totalLines = 0;

    /** Where the eye goes: which screen row the cursor is drawn on. */
    int cursorScreenRow() const { return cursorLine - topLine; }

    /** The invariant every panel owes: the cursor is on screen. */
    bool cursorVisible() const
    {
        return cursorScreenRow() >= 0 && cursorScreenRow() < viewportRows;
    }

    bool operator == ( const ScrollView& o ) const
    {
        return cursorLine == o.cursorLine && topLine == o.topLine
            && viewportRows == o.viewportRows && totalLines == o.totalLines;
    }
};

/**
 * The least scrolling that keeps `cursor` visible.
 *
 * @param currentTop
 *     Where the viewport is NOW. This parameter is the entire fix: without
 *     it there is no way to answer "does the view need to move at all?", and
 *     the only self-consistent answer left is "always put the cursor at the
 *     edge" -- which is the bug.
 *
 * @return the new top line.
 */
inline int ensureVisible( int cursor, int total, int rows, int currentTop )
{
    if( rows <= 0 ) {
        return 0;
    }

    int top = currentTop;

    /* Never leave blank rows below when there is content above to show. */
    const int maxTop = std::max( 0, total - rows );
    if( top > maxTop ) { top = maxTop; }
    if( top < 0 ) { top = 0; }

    /* Scroll only far enough to bring the cursor back inside. */
    if( cursor < top ) {
        top = cursor;
    } else if( cursor >= top + rows ) {
        top = cursor - rows + 1;
    }

    if( top < 0 ) { top = 0; }
    return top;
}

} // namespace lens

#endif // _LENS_MODEL_SCROLL_HPP
