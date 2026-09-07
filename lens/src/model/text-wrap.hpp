#if !defined( _LENS_MODEL_TEXT_WRAP_HPP )
#define _LENS_MODEL_TEXT_WRAP_HPP

/**
 * @file text-wrap.hpp
 *
 * Wrapping, and the map from source lines to screen rows.
 *
 * Shared rather than private to the renderer because scrolling needs it too:
 * a help topic's cursor addresses SOURCE lines (so Enter can follow the link
 * on a line), but the viewport is measured in SCREEN rows, and a line that
 * wraps to three rows moves the cursor three rows down. Computing that in
 * one place is what keeps `fold`, `view` and the interaction tests agreeing
 * about where the cursor is -- and they must agree, or the tests assert
 * something the user never sees.
 */

#include <string>
#include <vector>

namespace lens {

/**
 * Wrap one line to `width` columns, preserving its indentation.
 *
 * A panel that truncates its own text is broken however big it is, and the
 * tile it lands in is the solver's decision rather than the text's. So the
 * text bends. Continuation lines keep the original indent, which keeps a
 * wrapped bullet looking like one bullet rather than two.
 */
std::vector<std::string> wrapLine( const std::string& text, int width );

/** Where each source line starts once wrapped, and how tall the whole is. */
struct WrapMap {
    /** Screen row at which source line `i` begins. */
    std::vector<int> rowOfLine;
    int totalRows = 0;

    /** The source line occupying screen `row`, clamped into range. */
    int lineOfRow( int row ) const;
};

WrapMap wrapMap( const std::vector<std::string>& lines, int width );

} // namespace lens

#endif // _LENS_MODEL_TEXT_WRAP_HPP
