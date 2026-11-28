/**
 * @file help-extract.cpp
 *
 * The third source of help -- UI.md section 5.3: builtin and keyword topics
 * EXTRACTED at build time from the language's own documents, so that `F1`
 * on `findall` shows what the documents say and cannot fall behind them. A
 * hand-copied help text is wrong within a month.
 *
 * Usage:
 *     lens-help-extract <SPEC.md> <LANGUAGE.md> <output.inc>
 *
 * Two sources, each read from one place:
 *
 *   SPEC.md, section 9 "Builtins" -- one topic per builtin, `builtin.<name>`,
 *     from the paragraph that opens with its bold signature:
 *         **`print($x)`** -- PrintBuiltinClause ...
 *     A paragraph may name several (`contains` / `startswith` / `endswith`);
 *     each gets the same text.
 *
 *   LANGUAGE.md, "Reserved words and shapes" -- one topic per reserved form,
 *     `keyword.<name>`, from its table row: what you type, how it is
 *     reserved, and what it means.
 *
 * plus one index page, `language`, linking every topic it made.
 *
 * THE HONEST LIMIT, stated in UI.md and repeated here because it is the
 * reason this program is strict: an extractor is itself a parser and can
 * drift from the documents' formatting. So it does not guess. A section it
 * cannot find, a bold line in section 9 that is not a signature, a table row
 * with the wrong number of cells, or a source that yields nothing at all is
 * an ERROR, with the file and line, and the build stops. Silently emitting
 * fewer topics is the one outcome it must never have: the build would stay
 * green and `F1` would quietly stop answering.
 *
 * The output is a C++ initializer list of HelpTopic, included by
 * src/model/help-content.cpp. Every byte outside printable ASCII is written
 * as an octal escape, so the generated file means the same thing whatever
 * the compiler believes its encoding is -- and octal rather than hex,
 * because a hex escape swallows every hex digit after it and `\xe2\x80\x94a`
 * would not be an em dash followed by "a".
 */

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Topic {
    std::string id;
    std::string title;
    std::vector<std::string> lines;
};

/** Thrown with a `file:line: message` for the build log. */
struct ExtractError {
    std::string message;
};

[[noreturn]] void fail( const std::string& file, std::size_t line,
                        const std::string& message )
{
    std::ostringstream os;
    os << file;
    if( line > 0 ) {
        os << ":" << line;
    }
    os << ": " << message;
    throw ExtractError{ os.str() };
}


std::vector<std::string> readLines( const std::string& path )
{
    std::ifstream in( path.c_str(), std::ios::binary );
    if( !in ) {
        fail( path, 0, "cannot open" );
    }
    std::vector<std::string> lines;
    std::string line;
    while( std::getline( in, line ) ) {
        if( !line.empty() && line[ line.size() - 1 ] == '\r' ) {
            line.erase( line.size() - 1 );
        }
        lines.push_back( line );
    }
    return lines;
}


bool startsWith( const std::string& s, const std::string& prefix )
{
    return s.compare( 0, prefix.size(), prefix ) == 0;
}


std::string trim( const std::string& s )
{
    const std::string::size_type a = s.find_first_not_of( " \t" );
    if( a == std::string::npos ) {
        return std::string();
    }
    const std::string::size_type b = s.find_last_not_of( " \t" );
    return s.substr( a, b - a + 1 );
}


bool isIdentStart( char c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_';
}


bool isIdent( char c )
{
    return isIdentStart( c ) || ( c >= '0' && c <= '9' );
}


/** Markdown emphasis and code marks go; the words stay. */
std::string plain( const std::string& markdown )
{
    std::string out;
    for( std::size_t i = 0; i < markdown.size(); ++i ) {
        if( markdown[i] == '`' ) {
            continue;
        }
        if( markdown[i] == '*' && i + 1 < markdown.size()
            && markdown[ i + 1 ] == '*' ) {
            ++i;
            continue;
        }
        out += markdown[i];
    }
    return out;
}


/** Every `code span` on a line, in order. */
std::vector<std::string> codeSpans( const std::string& text )
{
    std::vector<std::string> spans;
    std::string::size_type at = 0;
    for( ;; ) {
        const std::string::size_type open = text.find( '`', at );
        if( open == std::string::npos ) {
            break;
        }
        const std::string::size_type close = text.find( '`', open + 1 );
        if( close == std::string::npos ) {
            break;
        }
        spans.push_back( text.substr( open + 1, close - open - 1 ) );
        at = close + 1;
    }
    return spans;
}


/**
 * Paragraphs become single lines, so the help panel reflows them to the
 * tile's width; a code block keeps its lines, indented, because reflowing
 * code would change what it says.
 */
std::vector<std::string> reflow( const std::vector<std::string>& source )
{
    std::vector<std::string> out;
    std::string paragraph;
    bool inCode = false;

    const auto flush = [ & ]() {
        if( !paragraph.empty() ) {
            out.push_back( plain( paragraph ) );
            paragraph.clear();
        }
    };

    for( std::size_t i = 0; i < source.size(); ++i ) {
        const std::string& line = source[i];
        if( startsWith( trim( line ), "```" ) ) {
            flush();
            inCode = !inCode;
            continue;
        }
        if( inCode ) {
            out.push_back( "    " + line );
            continue;
        }
        if( trim( line ).empty() ) {
            flush();
            if( !out.empty() && !out.back().empty() ) {
                out.push_back( std::string() );
            }
            continue;
        }
        if( !paragraph.empty() ) {
            paragraph += ' ';
        }
        paragraph += trim( line );
    }
    flush();
    while( !out.empty() && out.back().empty() ) {
        out.pop_back();
    }
    return out;
}


/** Index of the line that is exactly `heading`, or fail. */
std::size_t findHeading( const std::vector<std::string>& lines,
                         const std::string& heading, const std::string& file )
{
    for( std::size_t i = 0; i < lines.size(); ++i ) {
        if( trim( lines[i] ) == heading ) {
            return i;
        }
    }
    fail( file, 0, "no '" + heading + "' heading -- the document was "
                   "restructured, so this extractor must be too" );
}


// ---------------------------------------------------------------------------
// SPEC.md section 9
// ---------------------------------------------------------------------------

/**
 * The names in a bold signature line, or fail.
 *
 *     **`print($x)`** -- ...
 *     **`contains($s, $sub)` / `startswith($s, $prefix)` / ...**
 */
std::vector<std::string> signatureNames( const std::string& line,
                                         const std::string& file,
                                         std::size_t lineNumber )
{
    const std::string::size_type close = line.find( "**", 2 );
    if( close == std::string::npos ) {
        fail( file, lineNumber, "a bold line in section 9 that never closes "
                                "its bold -- expected **`name(args)`**" );
    }
    const std::string head = line.substr( 2, close - 2 );
    const std::vector<std::string> spans = codeSpans( head );
    if( spans.empty() ) {
        fail( file, lineNumber, "a bold line in section 9 with no `signature` "
                                "in it: '" + head + "'" );
    }

    std::vector<std::string> names;
    for( std::size_t i = 0; i < spans.size(); ++i ) {
        const std::string& span = spans[i];
        std::size_t end = 0;
        while( end < span.size() && isIdent( span[end] ) ) {
            ++end;
        }
        if( end == 0 || !isIdentStart( span[0] ) || end >= span.size()
            || span[end] != '(' ) {
            fail( file, lineNumber, "'" + span + "' is not a builtin "
                                    "signature of the form name(args)" );
        }
        names.push_back( span.substr( 0, end ) );
    }
    return names;
}


void extractBuiltins( const std::string& file, std::vector<Topic>& out )
{
    const std::vector<std::string> lines = readLines( file );
    const std::size_t start = findHeading( lines, "## 9. Builtins", file );

    std::size_t end = start + 1;
    while( end < lines.size() && !startsWith( lines[end], "## " ) ) {
        ++end;
    }

    struct Entry {
        std::vector<std::string> names;
        std::vector<std::string> body;
    };
    std::vector<Entry> entries;

    for( std::size_t i = start + 1; i < end; ++i ) {
        const std::string& line = lines[i];
        if( trim( line ) == "---" ) {
            continue;
        }
        /*
         * A signature OPENS a paragraph. A bold word at the start of a
         * wrapped continuation line -- SPEC.md has "**byte** length" -- is
         * text, and treating it as a heading is exactly the formatting
         * drift this program exists to refuse rather than guess about.
         */
        const bool opensParagraph =
            i == start + 1 || trim( lines[ i - 1 ] ).empty();
        if( opensParagraph && startsWith( line, "**" ) ) {
            Entry entry;
            entry.names = signatureNames( line, file, i + 1 );
            entries.push_back( entry );
        }
        if( !entries.empty() ) {
            entries.back().body.push_back( line );
        }
    }

    if( entries.empty() ) {
        fail( file, start + 1, "section 9 yielded no builtins -- expected "
                               "paragraphs opening **`name(args)`**" );
    }

    for( std::size_t e = 0; e < entries.size(); ++e ) {
        const std::vector<std::string> body = reflow( entries[e].body );
        for( std::size_t n = 0; n < entries[e].names.size(); ++n ) {
            Topic topic;
            topic.id = "builtin." + entries[e].names[n];
            topic.title = entries[e].names[n] + " \xe2\x80\x94 builtin";
            topic.lines = body;
            topic.lines.push_back( std::string() );
            topic.lines.push_back( "From unify/SPEC.md, section 9 (Builtins). "
                                   "See [[language]]." );
            out.push_back( topic );
        }
    }
}


// ---------------------------------------------------------------------------
// LANGUAGE.md, "Reserved words and shapes"
// ---------------------------------------------------------------------------

/** The cells of a `| a | b | c |` row. */
std::vector<std::string> cells( const std::string& row )
{
    std::vector<std::string> out;
    std::string cell;
    bool inCode = false;
    for( std::size_t i = 1; i < row.size(); ++i ) {
        const char c = row[i];
        if( c == '`' ) {
            inCode = !inCode;
        }
        if( c == '|' && !inCode ) {
            out.push_back( trim( cell ) );
            cell.clear();
            continue;
        }
        cell += c;
    }
    return out;
}


/** The reserved words a form cell names: the first plain identifier per span. */
std::vector<std::string> formNames( const std::string& form,
                                    const std::string& file,
                                    std::size_t lineNumber )
{
    std::vector<std::string> names;
    const std::vector<std::string> spans = codeSpans( form );
    for( std::size_t s = 0; s < spans.size(); ++s ) {
        const std::string& span = spans[s];
        for( std::size_t i = 0; i < span.size(); ++i ) {
            if( span[i] == '$' ) {
                /* A variable, not a word: skip it whole. */
                ++i;
                while( i < span.size() && isIdent( span[i] ) ) { ++i; }
                continue;
            }
            if( isIdentStart( span[i] ) ) {
                std::size_t end = i;
                while( end < span.size() && isIdent( span[end] ) ) { ++end; }
                names.push_back( span.substr( i, end - i ) );
                break;
            }
        }
    }
    if( names.empty() ) {
        fail( file, lineNumber, "a reserved-words row whose form names no "
                                "word: '" + form + "'" );
    }
    return names;
}


void extractKeywords( const std::string& file, std::vector<Topic>& out )
{
    const std::vector<std::string> lines = readLines( file );
    const std::size_t heading =
        findHeading( lines, "### Reserved words and shapes", file );

    std::size_t i = heading + 1;
    while( i < lines.size() && !startsWith( lines[i], "|" ) ) {
        if( startsWith( lines[i], "#" ) ) {
            fail( file, i + 1, "no table under 'Reserved words and shapes'" );
        }
        ++i;
    }
    if( i + 2 >= lines.size() ) {
        fail( file, heading + 1, "the reserved-words table is missing" );
    }

    const std::vector<std::string> header = cells( lines[i] );
    if( header.size() != 3 ) {
        fail( file, i + 1, "the reserved-words table should have three "
                           "columns (form, reservation, meaning)" );
    }
    i += 2;   /* the header and its |---| separator */

    std::size_t rows = 0;
    for( ; i < lines.size() && startsWith( lines[i], "|" ); ++i ) {
        const std::vector<std::string> row = cells( lines[i] );
        if( row.size() != 3 ) {
            std::ostringstream os;
            os << "a reserved-words row with " << row.size()
               << " cells, expected 3";
            fail( file, i + 1, os.str() );
        }
        ++rows;

        const std::vector<std::string> names = formNames( row[0], file, i + 1 );
        for( std::size_t n = 0; n < names.size(); ++n ) {
            Topic topic;
            topic.id = "keyword." + names[n];
            topic.title = names[n] + " \xe2\x80\x94 reserved";
            topic.lines.push_back( "  " + plain( row[0] ) );
            topic.lines.push_back( std::string() );
            topic.lines.push_back( plain( row[2] ) + "." );
            topic.lines.push_back( std::string() );
            topic.lines.push_back( "Reserved: " + plain( row[1] ) + "." );
            topic.lines.push_back( std::string() );
            topic.lines.push_back( "From unify/LANGUAGE.md, Reference appendix. "
                                   "See [[language]]." );
            out.push_back( topic );
        }
    }

    if( rows == 0 ) {
        fail( file, heading + 1, "the reserved-words table has no rows" );
    }
}


// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

std::string literal( const std::string& text )
{
    std::string out = "\"";
    for( std::size_t i = 0; i < text.size(); ++i ) {
        const unsigned char c = (unsigned char) text[i];
        if( c == '"' || c == '\\' ) {
            out += '\\';
            out += (char) c;
        } else if( c >= 0x20 && c < 0x7F ) {
            out += (char) c;
        } else {
            char escape[ 8 ];
            std::snprintf( escape, sizeof( escape ), "\\%03o", c );
            out += escape;
        }
    }
    out += "\"";
    return out;
}


void addIndex( std::vector<Topic>& topics )
{
    Topic index;
    index.id = "language";
    index.title = "The language";
    index.lines.push_back( "Every builtin and reserved word, as the language's "
                           "own documents describe them -- extracted from "
                           "unify/SPEC.md and unify/LANGUAGE.md when lens was "
                           "built, so this cannot fall behind them." );
    index.lines.push_back( std::string() );
    index.lines.push_back( "F1 with the cursor on a name in the transcript "
                           "opens its page directly." );
    index.lines.push_back( std::string() );
    index.lines.push_back( "Reserved words:" );
    for( std::size_t i = 0; i < topics.size(); ++i ) {
        if( startsWith( topics[i].id, "keyword." ) ) {
            index.lines.push_back( "  [[" + topics[i].id + "]]" );
        }
    }
    index.lines.push_back( std::string() );
    index.lines.push_back( "Builtins:" );
    for( std::size_t i = 0; i < topics.size(); ++i ) {
        if( startsWith( topics[i].id, "builtin." ) ) {
            index.lines.push_back( "  [[" + topics[i].id + "]]" );
        }
    }
    index.lines.push_back( std::string() );
    index.lines.push_back( "See [[transcript]]." );
    topics.push_back( index );
}

} // namespace


int main( int argc, char** argv )
{
    if( argc != 4 ) {
        std::cerr << "usage: " << argv[0]
                  << " <SPEC.md> <LANGUAGE.md> <output.inc>\n";
        return 2;
    }

    std::vector<Topic> topics;
    try {
        extractBuiltins( argv[1], topics );
        extractKeywords( argv[2], topics );
    }
    catch( const ExtractError& error ) {
        std::cerr << "lens-help-extract: " << error.message << "\n";
        return 1;
    }

    /* A name documented twice would make one page silently win. */
    std::map<std::string, int> seen;
    for( std::size_t i = 0; i < topics.size(); ++i ) {
        if( ++seen[ topics[i].id ] > 1 ) {
            std::cerr << "lens-help-extract: '" << topics[i].id
                      << "' is documented twice\n";
            return 1;
        }
    }
    addIndex( topics );

    std::ostringstream os;
    os << "// Generated by lens-help-extract from " << argv[1] << " and "
       << argv[2] << ".\n// Do not edit: edit the documents.\n";
    for( std::size_t i = 0; i < topics.size(); ++i ) {
        os << "{ " << literal( topics[i].id ) << ", "
           << literal( topics[i].title ) << ", {\n";
        for( std::size_t k = 0; k < topics[i].lines.size(); ++k ) {
            os << "    " << literal( topics[i].lines[k] ) << ",\n";
        }
        os << "} },\n";
    }

    /* Written only if it changed, so an unrelated doc edit does not rebuild
     * every model test. */
    const std::string text = os.str();
    {
        std::ifstream existing( argv[3], std::ios::binary );
        if( existing ) {
            std::ostringstream current;
            current << existing.rdbuf();
            if( current.str() == text ) {
                return 0;
            }
        }
    }
    std::ofstream out( argv[3], std::ios::binary );
    if( !out ) {
        std::cerr << "lens-help-extract: cannot write " << argv[3] << "\n";
        return 1;
    }
    out << text;
    return out ? 0 : 1;
}
