/**
 * @file cell-grid.cpp
 */

#include "cell-grid.hpp"

namespace lens {

namespace {

struct Range {
    char32_t first;
    char32_t last;
};

/**
 * Combining marks, which occupy no columns of their own.
 *
 * A subset, not the whole of Unicode's Mn/Me categories: the blocks a Unify
 * program or a diagnostic is realistically going to contain. Shipping a
 * generated 400-entry table would suggest a completeness this does not have
 * and the editor's own width work (G3) will supersede anyway.
 */
const Range kZeroWidth[] = {
    { 0x0300, 0x036F },   /* combining diacriticals */
    { 0x0483, 0x0489 },
    { 0x0591, 0x05BD },
    { 0x0610, 0x061A },
    { 0x064B, 0x065F },
    { 0x0670, 0x0670 },
    { 0x06D6, 0x06DC },
    { 0x0900, 0x0903 },
    { 0x093A, 0x093C },
    { 0x0941, 0x0948 },
    { 0x1AB0, 0x1AFF },
    { 0x1DC0, 0x1DFF },
    { 0x20D0, 0x20F0 },   /* combining marks for symbols */
    { 0xFE00, 0xFE0F },   /* variation selectors */
    { 0xFE20, 0xFE2F },
};

/** East Asian Wide and Fullwidth: two columns. */
const Range kWide[] = {
    { 0x1100, 0x115F },   /* Hangul Jamo initial consonants */
    { 0x2E80, 0x303E },   /* CJK radicals, Kangxi, CJK symbols */
    { 0x3041, 0x33FF },   /* Hiragana .. CJK compatibility */
    { 0x3400, 0x4DBF },   /* CJK extension A */
    { 0x4E00, 0x9FFF },   /* CJK unified ideographs */
    { 0xA000, 0xA4CF },   /* Yi */
    { 0xAC00, 0xD7A3 },   /* Hangul syllables */
    { 0xF900, 0xFAFF },   /* CJK compatibility ideographs */
    { 0xFE30, 0xFE6F },   /* CJK compatibility forms */
    { 0xFF00, 0xFF60 },   /* fullwidth forms */
    { 0xFFE0, 0xFFE6 },
    { 0x20000, 0x2FFFD }, /* CJK extensions B.. */
    { 0x30000, 0x3FFFD },
};

bool inRanges( char32_t ch, const Range* ranges, std::size_t count )
{
    for( std::size_t i = 0; i < count; ++i ) {
        if( ch >= ranges[i].first && ch <= ranges[i].last ) {
            return true;
        }
    }
    return false;
}

const Cell kBlankCell = Cell();

} // namespace

int displayWidth( char32_t ch )
{
    if( 0 == ch ) {
        return 0;
    }
    if( ch < 0x20 || ( ch >= 0x7F && ch < 0xA0 ) ) {
        /* Control characters are never drawn; the caller substitutes. */
        return 0;
    }
    if( inRanges( ch, kZeroWidth, sizeof( kZeroWidth ) / sizeof( kZeroWidth[0] ) ) ) {
        return 0;
    }
    if( inRanges( ch, kWide, sizeof( kWide ) / sizeof( kWide[0] ) ) ) {
        return 2;
    }
    return 1;
}


int displayWidth( const std::string& utf8 )
{
    int total = 0;
    const std::vector<char32_t> codepoints = decodeUtf8( utf8 );
    for( std::size_t i = 0; i < codepoints.size(); ++i ) {
        total += displayWidth( codepoints[i] );
    }
    return total;
}


std::vector<char32_t> decodeUtf8( const std::string& utf8 )
{
    std::vector<char32_t> out;
    std::size_t i = 0;

    while( i < utf8.size() ) {
        const unsigned char lead = (unsigned char) utf8[i];
        int extra = 0;
        char32_t ch = 0;

        if( lead < 0x80 ) {
            ch = lead; extra = 0;
        } else if( ( lead & 0xE0 ) == 0xC0 ) {
            ch = lead & 0x1F; extra = 1;
        } else if( ( lead & 0xF0 ) == 0xE0 ) {
            ch = lead & 0x0F; extra = 2;
        } else if( ( lead & 0xF8 ) == 0xF0 ) {
            ch = lead & 0x07; extra = 3;
        } else {
            out.push_back( 0xFFFD );
            ++i;
            continue;
        }

        if( i + (std::size_t) extra >= utf8.size() ) {
            out.push_back( 0xFFFD );
            break;
        }

        bool ok = true;
        for( int k = 1; k <= extra; ++k ) {
            const unsigned char cont = (unsigned char) utf8[ i + (std::size_t) k ];
            if( ( cont & 0xC0 ) != 0x80 ) { ok = false; break; }
            ch = ( ch << 6 ) | ( cont & 0x3F );
        }

        if( !ok ) {
            out.push_back( 0xFFFD );
            ++i;
            continue;
        }

        out.push_back( ch );
        i += (std::size_t) extra + 1;
    }

    return out;
}


std::string encodeUtf8( char32_t ch )
{
    std::string out;
    if( ch < 0x80 ) {
        out += (char) ch;
    } else if( ch < 0x800 ) {
        out += (char) ( 0xC0 | ( ch >> 6 ) );
        out += (char) ( 0x80 | ( ch & 0x3F ) );
    } else if( ch < 0x10000 ) {
        out += (char) ( 0xE0 | ( ch >> 12 ) );
        out += (char) ( 0x80 | ( ( ch >> 6 ) & 0x3F ) );
        out += (char) ( 0x80 | ( ch & 0x3F ) );
    } else {
        out += (char) ( 0xF0 | ( ch >> 18 ) );
        out += (char) ( 0x80 | ( ( ch >> 12 ) & 0x3F ) );
        out += (char) ( 0x80 | ( ( ch >> 6 ) & 0x3F ) );
        out += (char) ( 0x80 | ( ch & 0x3F ) );
    }
    return out;
}

// ---------------------------------------------------------------------------

CellGrid::CellGrid( int width, int height )
{
    resize( width, height );
}


void CellGrid::resize( int width, int height )
{
    m_width = width > 0 ? width : 0;
    m_height = height > 0 ? height : 0;
    m_cells.assign( (std::size_t) ( m_width * m_height ), Cell() );
}


const Cell& CellGrid::at( int x, int y ) const
{
    if( x < 0 || y < 0 || x >= m_width || y >= m_height ) {
        return kBlankCell;
    }
    return m_cells[ (std::size_t) ( y * m_width + x ) ];
}


void CellGrid::put( int x, int y, char32_t ch, const Attr& attr )
{
    if( x < 0 || y < 0 || x >= m_width || y >= m_height ) {
        return;
    }
    Cell& cell = m_cells[ (std::size_t) ( y * m_width + x ) ];
    cell.ch = ch;
    cell.attr = attr;
}


void CellGrid::fill( int x, int y, int w, int h, char32_t ch, const Attr& attr )
{
    for( int row = y; row < y + h; ++row ) {
        for( int col = x; col < x + w; ++col ) {
            put( col, row, ch, attr );
        }
    }
}


int CellGrid::drawText( int x, int y, const std::string& utf8, int maxWidth,
                        const Attr& attr )
{
    if( maxWidth <= 0 ) {
        return 0;
    }

    const std::vector<char32_t> codepoints = decodeUtf8( utf8 );
    int drawn = 0;

    for( std::size_t i = 0; i < codepoints.size(); ++i ) {
        const char32_t ch = codepoints[i];
        const int w = displayWidth( ch );

        if( 0 == w ) {
            /*
             * A combining mark belongs to the character before it. Nothing
             * in lens composes them yet, so it is dropped rather than drawn
             * over the previous cell -- visible as a missing accent, which
             * beats a corrupted column.
             */
            continue;
        }

        if( drawn + w > maxWidth ) {
            break;   /* a wide character that would straddle the edge */
        }

        put( x + drawn, y, ch, attr );
        if( 2 == w ) {
            /*
             * The second column of a wide character is blanked rather than
             * left as it was, so the cell behind it cannot show through.
             * U+0000 marks it as a continuation for the terminal layer.
             */
            put( x + drawn + 1, y, 0, attr );
        }
        drawn += w;
    }

    return drawn;
}


void CellGrid::drawBox( int x, int y, int w, int h, const std::string& title,
                        const Attr& attr )
{
    if( w < 2 || h < 2 ) {
        /* Too small for a border; fill it so nothing shows through. */
        fill( x, y, w, h, U' ', attr );
        return;
    }

    for( int col = x + 1; col < x + w - 1; ++col ) {
        put( col, y, U'─', attr );
        put( col, y + h - 1, U'─', attr );
    }
    for( int row = y + 1; row < y + h - 1; ++row ) {
        put( x, row, U'│', attr );
        put( x + w - 1, row, U'│', attr );
    }

    put( x, y, U'┌', attr );
    put( x + w - 1, y, U'┐', attr );
    put( x, y + h - 1, U'└', attr );
    put( x + w - 1, y + h - 1, U'┘', attr );

    if( title.empty() ) {
        return;
    }

    /* " title " inset one cell from the corner, truncated with an ellipsis. */
    const int available = w - 4;
    if( available <= 0 ) {
        return;
    }

    std::string shown = title;
    if( displayWidth( shown ) > available ) {
        const std::vector<char32_t> codepoints = decodeUtf8( shown );
        std::string truncated;
        int used = 0;
        for( std::size_t i = 0; i < codepoints.size(); ++i ) {
            const int cw = displayWidth( codepoints[i] );
            if( used + cw > available - 1 ) {
                break;
            }
            truncated += encodeUtf8( codepoints[i] );
            used += cw;
        }
        truncated += "…";
        shown = truncated;
    }

    put( x + 1, y, U' ', attr );
    const int drawn = drawText( x + 2, y, shown, available, attr );
    put( x + 2 + drawn, y, U' ', attr );
}


std::string CellGrid::toText() const
{
    std::string out;

    for( int y = 0; y < m_height; ++y ) {
        std::string line;
        for( int x = 0; x < m_width; ++x ) {
            const Cell& cell = at( x, y );
            if( 0 == cell.ch ) {
                continue;   /* the second column of a wide character */
            }
            line += encodeUtf8( cell.ch );
        }

        /* Strip trailing blanks: see the header for why. */
        std::size_t end = line.size();
        while( end > 0 && line[ end - 1 ] == ' ' ) {
            --end;
        }
        line.resize( end );

        out += line;
        out += "\n";
    }

    return out;
}


bool CellGrid::operator == ( const CellGrid& o ) const
{
    return m_width == o.m_width && m_height == o.m_height
        && m_cells == o.m_cells;
}

} // namespace lens
