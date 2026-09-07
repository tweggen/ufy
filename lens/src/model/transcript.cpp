/**
 * @file transcript.cpp
 */

#include "transcript.hpp"

#include <sstream>

namespace lens {

const char* const kTranscriptPrompt = "?- ";

std::string renderValue( const us::Value& value )
{
    std::ostringstream os;

    switch( value.kind ) {
    case us::Value::Kind::Atom:
    case us::Value::Kind::Var:
        os << value.name;
        break;
    case us::Value::Kind::Int:
        os << value.i;
        break;
    case us::Value::Kind::Float:
        os << value.f;
        break;
    case us::Value::Kind::Str:
        /*
         * No quotes. Engine item E7 is open, so every binding arrives as a
         * Str whatever it really is -- quoting them all would tell the user
         * that `1` is the string "1", which is worse than saying nothing
         * about the type. When E7 lands, Str becomes rare and quoting it
         * becomes right.
         */
        os << value.name;
        break;
    case us::Value::Kind::Cons:
        os << value.name;
        if( !value.args.empty() ) {
            os << "( ";
            for( std::size_t i = 0; i < value.args.size(); ++i ) {
                if( i ) { os << ", "; }
                os << renderValue( value.args[i] );
            }
            os << " )";
        }
        break;
    case us::Value::Kind::Array:
        os << "[ ";
        for( std::size_t i = 0; i < value.args.size(); ++i ) {
            if( i ) { os << ", "; }
            os << renderValue( value.args[i] );
        }
        os << " ]";
        break;
    case us::Value::Kind::Map:
        os << "{ ";
        for( std::size_t i = 0; i < value.pairs.size(); ++i ) {
            if( i ) { os << ", "; }
            os << value.pairs[i].first << ": "
               << renderValue( value.pairs[i].second );
        }
        os << " }";
        break;
    }

    /*
     * A truncated node must SAY so. A front end that dropped the marker
     * would show the user a wrong term rather than a shortened one, which is
     * the failure SESSION-API section 3 calls load-bearing.
     */
    if( value.truncated ) {
        os << " …";
    }

    return os.str();
}


std::string renderBindings(
    const std::vector<std::pair<std::string, us::Value>>& bindings )
{
    std::string line;
    for( std::size_t i = 0; i < bindings.size(); ++i ) {
        if( !line.empty() ) {
            line += ", ";
        }
        line += bindings[i].first;
        line += " = ";
        line += renderValue( bindings[i].second );
    }
    return line;
}


std::vector<TranscriptEntry> renderDiagnostic( const us::Diagnostic& diagnostic )
{
    std::vector<TranscriptEntry> entries;

    std::ostringstream header;
    header << ( diagnostic.file.empty() ? "<input>" : diagnostic.file );
    header << ":" << diagnostic.line;
    if( diagnostic.column > 0 ) {
        header << ":" << diagnostic.column;
    }
    header << ": ";
    if( diagnostic.sev == us::Severity::Warning ) {
        header << "warning: ";
    } else if( diagnostic.sev == us::Severity::Note ) {
        header << "note: ";
    }
    header << diagnostic.message;

    TranscriptEntry first;
    first.kind = TranscriptEntry::Kind::Diagnostic;
    first.text = header.str();
    first.file = diagnostic.file;
    first.line = diagnostic.line;
    first.column = diagnostic.column;
    first.sourceLine = diagnostic.sourceLine;
    entries.push_back( first );

    /*
     * The offending line and a caret, exactly as unify-run prints them
     * (G2.2). Shown only when there is a line to show: a runtime error has
     * none, and an empty line plus a lone caret would be noise pretending
     * to be information.
     */
    if( !diagnostic.sourceLine.empty() ) {
        TranscriptEntry source;
        source.kind = TranscriptEntry::Kind::Diagnostic;
        source.text = diagnostic.sourceLine;
        entries.push_back( source );

        TranscriptEntry caret;
        caret.kind = TranscriptEntry::Kind::Diagnostic;
        caret.text = std::string(
            diagnostic.column > 1 ? (std::size_t)( diagnostic.column - 1 ) : 0,
            ' ' ) + "^";
        entries.push_back( caret );
    }

    return entries;
}

} // namespace lens
