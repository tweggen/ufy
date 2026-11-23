/**
 * @file transcript.cpp
 */

#include "transcript.hpp"

#include <ostream>
#include <sstream>

namespace lens {

const char* const kTranscriptPrompt = "?- ";

namespace {

/**
 * What a truncated node shows in place of what the budget cut.
 *
 * U+2026 and not "...", chosen rather than inherited: lens already spells
 * truncation this way everywhere it cuts something -- a tile title too wide
 * for its border (cell-grid.cpp), the elided-output line (model.cpp), a menu
 * item that opens more (the `Commands` entry, pinned by
 * menu-120x40.expected). A second, ASCII spelling here would make one event
 * -- "there is more than you are being shown" -- look like two, depending on
 * which layer happened to do the cutting. It is also one column on a fixed
 * grid where "..." is three, and the transcript is the panel most often
 * narrow.
 *
 * The engine's toDisplayString() spells it "..."; that function currently has
 * no caller anywhere in the tree, so nothing is being diverged FROM.
 *
 * Escaped rather than written as a glyph, for the reason cell-grid.cpp gives
 * at length: nothing in model/ should depend on the compiler agreeing that
 * this file is UTF-8.
 */
const char* const kEllipsis = "\u2026";

/**
 * The mark for children a budget cut, written as one more element.
 *
 * `[ 1, \u2026 ]` and `f( \u2026 )` -- the separator only when there is
 * something to separate it from, so an all-cut node does not render the empty
 * `[  ]` that a trailing mark used to leave behind.
 */
void appendCutMark( std::ostream& os, bool truncated, bool empty )
{
    if( !truncated ) {
        return;
    }
    if( !empty ) {
        os << ", ";
    }
    os << kEllipsis;
}

} // namespace

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
         * No quotes -- and NOT because a better day is coming. This engine
         * cannot produce a Str at all, ever: quoting is lost in the parser,
         * where `red` and `"red"` become byte-identical (unify/SPEC.md:70),
         * so every binding LocalSession sends arrives as Atom, Int, Cons,
         * Array, Map or Var. There are no floats either, for the same reason
         * (SPEC.md:50). Both kinds stay in the wire format for a remote core
         * or the fake session, which do have real types -- so this arm is
         * live code with no local producer, not a leftover.
         *
         * Which is exactly why it does not quote. A quote here would be lens
         * asserting that some other core's Str means "text, as opposed to a
         * number or an atom", and lens has no way to know that. Printing the
         * bytes plain is the one rendering that cannot be a lie. The engine's
         * toDisplayString() does quote; it renders for a debugger's eye,
         * where showing the kind is the point, and this renders for a user
         * reading their own program's answer back.
         */
        os << value.name;
        break;
    case us::Value::Kind::Cons:
        os << value.name;
        /*
         * `|| value.truncated`, and this is the whole trap. A compound whose
         * every argument the budget cut arrives with args EMPTY and truncated
         * set. Testing only args.empty() would print it as its bare functor
         * -- `point` where the term is `point( 1, 2 )` -- which is a wrong
         * term shown as a whole one, the precise failure SESSION-API
         * section 3 says the flag exists to prevent. A shortened term is
         * honest; a different term is not.
         */
        if( !value.args.empty() || value.truncated ) {
            os << "( ";
            for( std::size_t i = 0; i < value.args.size(); ++i ) {
                if( i ) { os << ", "; }
                os << renderValue( value.args[i] );
            }
            appendCutMark( os, value.truncated, value.args.empty() );
            os << " )";
        }
        break;
    case us::Value::Kind::Array:
        os << "[ ";
        for( std::size_t i = 0; i < value.args.size(); ++i ) {
            if( i ) { os << ", "; }
            os << renderValue( value.args[i] );
        }
        appendCutMark( os, value.truncated, value.args.empty() );
        os << " ]";
        break;
    case us::Value::Kind::Map:
        os << "{ ";
        for( std::size_t i = 0; i < value.pairs.size(); ++i ) {
            if( i ) { os << ", "; }
            os << value.pairs[i].first << ": "
               << renderValue( value.pairs[i].second );
        }
        appendCutMark( os, value.truncated, value.pairs.empty() );
        os << " }";
        break;
    }

    /*
     * A truncated LEAF still has to say so; the three compounds said it
     * above, from inside their own brackets, and must not say it twice.
     *
     * Inside, because the two truncations mean different things and should
     * not read alike: `[ 1, 2, … ]` says the list lost members, where
     * `[ 1, 2 ] …` reads as if something after the list had been cut. A
     * leaf really was cut at its own end, so its mark goes there, with no
     * separating space -- `verylongatom…` is a clipped word, `verylongatom
     * …` looks like a word followed by an omission.
     */
    if( value.truncated
        && value.kind != us::Value::Kind::Cons
        && value.kind != us::Value::Kind::Array
        && value.kind != us::Value::Kind::Map ) {
        os << kEllipsis;
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
