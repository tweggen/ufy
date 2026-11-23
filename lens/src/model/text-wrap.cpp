/**
 * @file text-wrap.cpp
 */

#include "text-wrap.hpp"
#include "cell-grid.hpp"

namespace lens {

std::vector<std::string> wrapLine( const std::string& text, int width )
{
    std::vector<std::string> out;
    if( width <= 0 ) {
        return out;
    }
    if( displayWidth( text ) <= width ) {
        out.push_back( text );
        return out;
    }

    const std::size_t indentSize = text.find_first_not_of( ' ' );
    const std::string indent(
        ( indentSize == std::string::npos ) ? 0 : indentSize, ' ' );

    std::string current;
    std::string word;

    const auto flush = [ & ]() {
        if( !current.empty() ) {
            out.push_back( current );
            current = indent;
        }
    };

    current = std::string();
    for( std::size_t i = 0; i <= text.size(); ++i ) {
        const bool end = ( i == text.size() );
        if( !end && text[i] != ' ' ) {
            word += text[i];
            continue;
        }

        if( !word.empty() ) {
            const std::string candidate =
                current.empty() ? word : current + " " + word;
            if( displayWidth( candidate ) > width && !current.empty() ) {
                flush();
                current += word;
            } else {
                current = candidate;
            }
            word.clear();
        } else if( !end && current.empty() ) {
            current += ' ';
        } else if( !end ) {
            current += ' ';
        }
    }
    if( !current.empty() ) {
        out.push_back( current );
    }
    return out;
}


int WrapMap::lineOfRow( int row ) const
{
    if( rowOfLine.empty() ) {
        return 0;
    }
    for( std::size_t i = rowOfLine.size(); i > 0; --i ) {
        if( rowOfLine[i - 1] <= row ) {
            return (int) ( i - 1 );
        }
    }
    return 0;
}


WrapMap wrapMap( const std::vector<std::string>& lines, int width )
{
    WrapMap map;
    map.rowOfLine.reserve( lines.size() );

    int row = 0;
    for( std::size_t i = 0; i < lines.size(); ++i ) {
        map.rowOfLine.push_back( row );
        const std::vector<std::string> wrapped = wrapLine( lines[i], width );
        /* An empty line still occupies one row, or blank lines would
         * collapse and the cursor would land somewhere unexpected. */
        row += wrapped.empty() ? 1 : (int) wrapped.size();
    }
    map.totalRows = row;
    return map;
}

} // namespace lens
