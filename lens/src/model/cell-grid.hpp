#if !defined( _LENS_MODEL_CELL_GRID_HPP )
#define _LENS_MODEL_CELL_GRID_HPP

/**
 * @file cell-grid.hpp
 *
 * The character grid -- the output of `view`, and the only thing `term/`
 * ever draws.
 *
 * ARCHITECTURE.md section 4: `view : (Model, Geometry) -> CellGrid` is a
 * pure function, so the same model and geometry always render the same grid.
 * That is what makes every screen testable with no terminal, no pty and no
 * timing, and it is the reason the grid is a value type here rather than a
 * sequence of calls into a drawing library.
 *
 * It also defines the seam FTXUI sits behind. `ITerminal::draw(CellGrid)`
 * takes this and nothing else, so replacing the terminal library changes one
 * file and no caller (gate G1.5).
 *
 * Colour lives on a cell but goldens record only the characters. That is
 * deliberate and it is ARCHITECTURE.md section 6.2's rule made structural:
 * every panel must be legible in the monochrome tier, so no meaning may be
 * carried by colour alone -- and a golden that cannot see colour cannot
 * accidentally start depending on it.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace lens {

/**
 * Colour, as a palette index rather than an RGB triple.
 *
 * Three tiers are auto-detected at startup (truecolour, 256-colour,
 * monochrome plus attributes); the model names a ROLE and the terminal layer
 * resolves it. A model that named an RGB value would render differently in
 * each tier and could not be tested once.
 */
enum class Colour : std::uint8_t {
    Default = 0,
    Dim,           //!< de-emphasised text: stubs, inactive borders
    Accent,        //!< the focused tile's border and title
    Warning,
    Error,
    Selection
};

struct Attr {
    Colour colour = Colour::Default;
    bool   bold = false;
    bool   reverse = false;
    bool   underline = false;

    constexpr bool operator == ( const Attr& o ) const
    {
        return colour == o.colour && bold == o.bold
            && reverse == o.reverse && underline == o.underline;
    }
};

struct Cell {
    char32_t ch = U' ';
    Attr     attr;

    bool operator == ( const Cell& o ) const
    {
        return ch == o.ch && attr == o.attr;
    }
};

/**
 * How many columns a character occupies.
 *
 * Zero for combining marks, two for East Asian wide characters, one
 * otherwise. Without this a single CJK character in a clause head misaligns
 * every column to its right, in every panel and in every golden -- UI.md
 * section 3.1 names it explicitly for that reason.
 *
 * The honest limit: this is per-CODEPOINT, so it handles wide characters and
 * combining marks but not full grapheme clusters (an emoji with a skin-tone
 * modifier, a regional-indicator flag). Correct clustering arrives with the
 * editor in G3, where it is gated by a test over a CJK and combining-mark
 * corpus. Until then this is right for the cases lens actually renders and
 * wrong only for cases it cannot yet produce.
 */
int displayWidth( char32_t ch );

/** Columns occupied by a UTF-8 string, summing displayWidth(). */
int displayWidth( const std::string& utf8 );

/** Decode UTF-8 into codepoints. Invalid bytes become U+FFFD. */
std::vector<char32_t> decodeUtf8( const std::string& utf8 );

/** Encode one codepoint as UTF-8. */
std::string encodeUtf8( char32_t ch );

class CellGrid {
public:
    CellGrid() = default;
    CellGrid( int width, int height );

    int width()  const { return m_width; }
    int height() const { return m_height; }

    void resize( int width, int height );

    /** Out-of-range reads return a blank cell rather than crashing. */
    const Cell& at( int x, int y ) const;

    /** Out-of-range writes are ignored, so no caller needs to clip. */
    void put( int x, int y, char32_t ch, const Attr& attr = Attr() );

    void fill( int x, int y, int w, int h, char32_t ch,
               const Attr& attr = Attr() );

    /**
     * Draw UTF-8 text at (x, y), clipped to `maxWidth` columns.
     *
     * @return the number of columns actually drawn. A wide character that
     *     would straddle the clip boundary is dropped rather than
     *     half-drawn, because half of a wide character is a different
     *     character on most terminals.
     */
    int drawText( int x, int y, const std::string& utf8, int maxWidth,
                  const Attr& attr = Attr() );

    /**
     * A single-line box with an optional title in its top border.
     *
     * The title is truncated with an ellipsis rather than wrapped; a border
     * that grew a second row would break the tiling arithmetic the solver
     * just guaranteed.
     */
    void drawBox( int x, int y, int w, int h, const std::string& title,
                  const Attr& attr = Attr() );

    /**
     * The grid as text, one line per row, trailing blanks stripped.
     *
     * Trailing blanks are stripped because a golden file full of invisible
     * padding is a golden file that editors and diff tools quietly corrupt.
     * Nothing is lost: the grid's width is a property of the model and is
     * asserted there, not read out of a text file.
     */
    std::string toText() const;

    bool operator == ( const CellGrid& o ) const;

private:
    int m_width = 0;
    int m_height = 0;
    std::vector<Cell> m_cells;
};

} // namespace lens

#endif // _LENS_MODEL_CELL_GRID_HPP
