/**
 * @file spec.cpp
 */

#include "spec.hpp"

#include "driver.hpp"

#include "vault-unify-local-session.hpp"
#include "vault-unify-session-value.hpp"
#include "vault-unify-session.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <ostream>
#include <sstream>
#include <vector>

namespace lens {

namespace {

namespace us = vault::unify::session;

// ---------------------------------------------------------------------------
// Talking to the engine.
// ---------------------------------------------------------------------------

/**
 * Collects every event the session delivers.
 *
 * Called on the session thread; the runner reads it only after
 * `waitUntilQuiet()`. That is why this can be a plain vector behind a mutex
 * rather than the queue the interactive bridge needs: a spec run is a batch
 * job, and a batch job that interleaved with the engine would be harder to
 * read for no benefit.
 */
class Collector : public us::EventSink {
public:
    void onEvent( const us::Event& event ) override
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        m_events.push_back( event );
    }

    std::vector<us::Event> take()
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        std::vector<us::Event> out;
        out.swap( m_events );
        return out;
    }

private:
    std::mutex m_mutex;
    std::vector<us::Event> m_events;
};


/**
 * One solution: each goal variable, and the one word or number it bound to.
 *
 * WHY THIS IS STILL TEXT, now that a binding crosses the boundary as a TREE
 * (engine item E7.3 -- Atom, Int, Cons, Array, Map, Var; the table is in
 * unify/src/vault-unify-term-value.hpp). The alternative was to keep
 * `us::Value` in the row and check the kind at each of the dozen reads. It
 * was not taken, for two reasons:
 *
 *  - The check is the SAME check at every one of them. The flat-fact
 *    vocabulary this file reads has no argument that may be a compound, so
 *    "is this a leaf" is a property of the CROSSING rather than of the
 *    field, and the crossing is the one place that can state it once and be
 *    sure it was not forgotten at the thirteenth read.
 *  - The vocabulary dispatches on text. checkExpectation() tells
 *    `cursor( 99 )` from `panel( Help )` by trying asInt() on the argument
 *    (see "the value-equality family" below). Typed values would have moved
 *    that decision in the same phase that changed what arrives, and then a
 *    red case would mean either of two things.
 *
 * So a row is text, plus the one reason it may not be readable at all.
 * `complaint` is non-empty when some binding was not a leaf, and it belongs
 * to the ROW rather than to the field: a fact with a compound in it is not a
 * fact this file can read, whichever argument the compound was in.
 *
 * AND THEN THE NESTED FORM ARRIVED (E7.6), whose whole point is that one
 * argument IS a tree. So the row also keeps every binding untouched in
 * `terms`. The two are not two views of the same thing and must not be read
 * as if they were:
 *
 *  - `values` / `complaint` are the FLAT reader's verdict. For a flat fact
 *    they are the whole truth.
 *  - For a `case/3` or `case/4` row `complaint` is ALWAYS set, because
 *    `$steps` is a list and a list is not a leaf. It is not a diagnosis
 *    there, it is the flat reader saying "not mine". The nested reader must
 *    therefore never consult it, and instead checks leaf-ness itself at the
 *    two places it wants a word -- `$id` and `$name`.
 */
struct Row {
    std::map<std::string, std::string> values;
    std::map<std::string, us::Value>   terms;
    std::string complaint;
};


/**
 * A binding, flattened to the word or number a flat fact may carry.
 *
 * @return true with `out_text` set, or false with `out_why` naming the trap.
 *
 * NOT `us::toDisplayString()` and not lens's own `renderValue()`
 * (src/model/transcript.cpp), though both are linked in and both flatten a
 * `Value`. Both render for a READER -- the first quotes `Str` so a
 * debugger's eye can see the kind, the second deliberately does not so a
 * user reads their own answer back. This produces a KEY: a case id, a panel
 * name, a key sequence, a step number. `"Help"` with its quotes is not the
 * panel `Help`, and a compound rendered to text would be a compound
 * smuggled in as a word -- exactly the "a typo must not be green" rule this
 * runner exists to keep. toDisplayString IS the right tool for the
 * complaint, and is used there: showing the kind is what a diagnostic wants.
 *
 * Every kind is spelled out, with no `default`, so a kind added to the wire
 * format surfaces as a -Wswitch warning here rather than as a binding that
 * silently reads as the empty string.
 */
bool flatten( const us::Value& value, std::string& out_text,
              std::string& out_why )
{
    if( value.truncated ) {
        /*
         * A cut value is a DIFFERENT value, not a shorter one, and this file
         * compares for equality and looks ids up in a map. Losing the case
         * loudly beats matching a prefix.
         */
        out_why = "was cut short by the value budget";
        return false;
    }

    std::ostringstream text;

    switch( value.kind ) {
    case us::Value::Kind::Atom:
    case us::Value::Kind::Str:
        /*
         * This engine produces only Atom -- quoting is lost in the parser,
         * so `red` and `"red"` arrive byte-identical. Str is a remote core's
         * or the fake session's, and is taken unquoted for the reason above:
         * the text is about to become a key.
         */
        text << value.name;
        break;

    case us::Value::Kind::Int:
        text << value.i;
        break;

    case us::Value::Kind::Float:
        text << value.f;
        break;

    case us::Value::Kind::Var:
        out_why = "is an unbound variable (" + us::toDisplayString( value )
                + ")";
        return false;

    case us::Value::Kind::Cons:
    case us::Value::Kind::Array:
    case us::Value::Kind::Map:
        out_why = "is a structured term (" + us::toDisplayString( value )
                + "), and every argument of a flat fact must be a word or a "
                  "number";
        return false;
    }

    out_text = text.str();
    return true;
}


/**
 * Run one goal and return every solution.
 *
 * Bindings arrive as term trees and are flattened HERE, at the crossing --
 * see Row above for why here rather than at each read.
 */
std::vector<Row> query( us::LocalSession& session, Collector& collector,
                        const std::string& goal )
{
    us::QueryOptions options;
    /*
     * Everything at once. The demand-driven path exists so a UI stays
     * responsive; here it would only add a way to silently run half a suite.
     */
    options.initialDemand = 1000000;
    options.retain = false;

    /*
     * The default budget (depth 8, 512 nodes) is sized for a value SHOWN on a
     * 40-row screen. A nested case is not shown, it is READ, and a case of a
     * dozen steps with expectations on each is a bigger tree than a
     * transcript line ever is; at the display default it would arrive cut
     * short. Raising the ceiling is not the same as tolerating a cut: both
     * flatten() and the nested reader refuse a `truncated` node outright, so
     * a case too large even for this fails loudly rather than running as the
     * prefix of itself.
     */
    options.budget.maxDepth = 64;
    options.budget.maxNodes = 100000;

    ( void ) session.solve( goal, options );
    session.waitUntilQuiet();

    std::vector<Row> rows;
    const std::vector<us::Event> events = collector.take();

    for( std::size_t i = 0; i < events.size(); ++i ) {
        const us::Solution* solution =
            std::get_if<us::Solution>( &events[i].body );
        if( !solution ) {
            continue;
        }

        Row row;
        for( std::size_t b = 0; b < solution->bindings.size(); ++b ) {
            const std::string& name = solution->bindings[b].first;
            std::string text;
            std::string why;

            row.terms[ name ] = solution->bindings[b].second;

            if( flatten( solution->bindings[b].second, text, why ) ) {
                row.values[ name ] = text;
                continue;
            }

            /*
             * The first complaint only. A row is already unusable after one,
             * and a reader chasing a compound argument wants the name of the
             * argument, not a list.
             */
            if( row.complaint.empty() ) {
                row.complaint = name + " " + why;
            }
        }
        rows.push_back( row );
    }

    return rows;
}


std::string field( const Row& row, const char* name )
{
    const std::map<std::string, std::string>::const_iterator it =
        row.values.find( name );
    return it == row.values.end() ? std::string() : it->second;
}


/** The binding as it arrived, tree and all, or NULL if the goal had no such
 *  variable. Only the nested reader wants this; see Row. */
const us::Value* binding( const Row& row, const char* name )
{
    const std::map<std::string, us::Value>::const_iterator it =
        row.terms.find( name );
    return it == row.terms.end() ? NULL : &it->second;
}


bool asInt( const std::string& text, long long& out )
{
    if( text.empty() ) {
        return false;
    }
    char* end = NULL;
    const long long value = std::strtoll( text.c_str(), &end, 10 );
    if( !end || *end != '\0' ) {
        return false;
    }
    out = value;
    return true;
}

// ---------------------------------------------------------------------------
// The cases, as read out of the engine.
// ---------------------------------------------------------------------------

struct Expectation {
    std::string kind;                 //!< visible, panel, cursor, moved, ...
    std::vector<std::string> args;
};

struct Press {
    int         index = 0;
    std::string keys;
    int         times = 1;
};

struct Case {
    std::string id;
    std::string name;
    std::string layout = "browse";
    int         width = 120;
    int         height = 40;

    std::vector<Press> presses;
    std::map<int, std::vector<Expectation> > expectations;

    std::string malformed;   //!< non-empty if the facts do not add up

    /**
     * Written as one nested term rather than as a pile of flat facts.
     *
     * Load-bearing rather than informational: it is what lets a flat
     * `press( Id, ... )` aimed at a nested case be REFUSED instead of
     * appended. A nested case already carries its steps in order, so a flat
     * press joining it would land at some step number the term never
     * mentions -- a fact that appears to do something and does something
     * else, which is the one outcome this runner is built against.
     */
    bool nested = false;
};

// ---------------------------------------------------------------------------
// The nested form: one term per case.
//
// Everything below produces exactly the Press and Expectation the flat
// reader produces, and then stops. There is ONE checker (checkExpectation)
// and one replayer (runCase) for both forms, on purpose: two readers of one
// vocabulary is a documentation problem, two checkers would be a correctness
// one -- the shapes would drift and a case would mean different things
// depending on how it was spelled.
// ---------------------------------------------------------------------------

/**
 * One expectation term -> the same Expectation a flat `expect` fact makes.
 *
 * `visible` arrives as a 0-arity ConsTerm, which the walker types as Atom;
 * everything else is a Cons whose functor is the kind and whose arguments
 * must each be a leaf. Nothing is validated against the vocabulary here --
 * that is checkExpectation()'s job, and it already fails on a kind it does
 * not know rather than passing it. Splitting the check would give a typo two
 * chances to be forgiven.
 */
bool readExpectation( const us::Value& value, Expectation& out_expectation,
                      std::string& out_why )
{
    if( value.truncated ) {
        out_why = "'" + us::toDisplayString( value )
                + "' was cut short by the value budget";
        return false;
    }

    if( value.kind == us::Value::Kind::Atom
        || value.kind == us::Value::Kind::Str ) {
        out_expectation.kind = value.name;
        return true;
    }

    if( value.kind != us::Value::Kind::Cons ) {
        out_why = "'" + us::toDisplayString( value )
                + "' is not an expectation; an expectation is a name, or a "
                  "name( ... ) term";
        return false;
    }

    out_expectation.kind = value.name;
    for( std::size_t i = 0; i < value.args.size(); ++i ) {
        std::string text;
        std::string why;
        if( !flatten( value.args[i], text, why ) ) {
            std::ostringstream os;
            os << value.name << " argument " << ( i + 1 ) << " " << why;
            out_why = os.str();
            return false;
        }
        out_expectation.args.push_back( text );
    }
    return true;
}


bool readExpectations( const us::Value& list,
                       std::vector<Expectation>& out_expectations,
                       std::string& out_why )
{
    if( list.kind != us::Value::Kind::Array || list.truncated ) {
        out_why = "the expectations of a step must be a list, not '"
                + us::toDisplayString( list ) + "'";
        return false;
    }

    for( std::size_t i = 0; i < list.args.size(); ++i ) {
        Expectation expectation;
        if( !readExpectation( list.args[i], expectation, out_why ) ) {
            return false;
        }
        out_expectations.push_back( expectation );
    }
    return true;
}


/**
 * The steps list -> presses and expectations, numbered by position.
 *
 * A step's number is where it sits in the list, so the nested form cannot
 * have the two incoherences the flat form can -- a gap in the numbering, or
 * an expectation of a step nobody presses. That is the point of it.
 *
 * `step( Keys )` and `repeat( N, Keys )` exist because an empty list does
 * not parse (unify/ROADMAP.md): `step( "F10", [] )` is a parse error, so a
 * step with nothing to check needs a shape that omits the list rather than
 * one that writes it empty.
 */
bool readSteps( const us::Value& list, Case& out_case, std::string& out_why )
{
    if( list.kind != us::Value::Kind::Array || list.truncated ) {
        out_why = "the steps of a case must be a list, not '"
                + us::toDisplayString( list ) + "'";
        return false;
    }

    for( std::size_t i = 0; i < list.args.size(); ++i ) {
        const us::Value& value = list.args[i];

        std::ostringstream where;
        where << "step " << ( i + 1 ) << ": ";

        const bool repeating = ( value.kind == us::Value::Kind::Cons
                                 && value.name == "repeat" );
        if( !repeating
            && !( value.kind == us::Value::Kind::Cons && value.name == "step" ) ) {
            out_why = where.str() + "'" + us::toDisplayString( value )
                    + "' is not step( Keys ), step( Keys, Expectations ), "
                      "repeat( N, Keys ) or repeat( N, Keys, Expectations )";
            return false;
        }

        /* repeat takes a count before the keys; both may end with a list. */
        const std::size_t least = repeating ? 2u : 1u;
        if( value.args.size() != least && value.args.size() != least + 1 ) {
            std::ostringstream os;
            os << where.str() << value.name << " takes " << least << " or "
               << ( least + 1 ) << " arguments, not " << value.args.size();
            out_why = os.str();
            return false;
        }

        Press press;
        press.index = (int) ( i + 1 );
        press.times = 1;

        if( repeating ) {
            std::string text;
            std::string why;
            long long times = 0;
            if( !flatten( value.args[0], text, why )
                || !asInt( text, times ) || times < 0 ) {
                out_why = where.str()
                        + "a repeat count must be a whole number, not '"
                        + us::toDisplayString( value.args[0] ) + "'";
                return false;
            }
            press.times = (int) times;
        }

        {
            std::string why;
            if( !flatten( value.args[ repeating ? 1u : 0u ], press.keys, why ) ) {
                out_why = where.str() + "the keys " + why;
                return false;
            }
        }

        if( value.args.size() == least + 1 ) {
            std::vector<Expectation> expectations;
            std::string why;
            if( !readExpectations( value.args[ least ], expectations, why ) ) {
                out_why = where.str() + why;
                return false;
            }
            out_case.expectations[ press.index ] = expectations;
        }

        out_case.presses.push_back( press );
    }
    return true;
}


/**
 * The optional options list -- what `layout/2` and `geometry/3` say flatly.
 *
 * Closed the same way the expectations are: an option nobody defined is an
 * error, because an ignored option is a case that ran with a geometry the
 * author did not ask for and could not see.
 */
bool readOptions( const us::Value& list, Case& out_case, std::string& out_why )
{
    if( list.kind != us::Value::Kind::Array || list.truncated ) {
        out_why = "the options of a case must be a list, not '"
                + us::toDisplayString( list ) + "'";
        return false;
    }

    for( std::size_t i = 0; i < list.args.size(); ++i ) {
        const us::Value& value = list.args[i];
        std::string why;

        if( value.kind == us::Value::Kind::Cons && value.name == "layout"
            && value.args.size() == 1 ) {
            if( !flatten( value.args[0], out_case.layout, why ) ) {
                out_why = "the layout " + why;
                return false;
            }
            continue;
        }

        if( value.kind == us::Value::Kind::Cons && value.name == "geometry"
            && value.args.size() == 2 ) {
            std::string w;
            std::string h;
            long long columns = 0;
            long long rows = 0;
            if( !flatten( value.args[0], w, why ) || !asInt( w, columns )
                || !flatten( value.args[1], h, why ) || !asInt( h, rows ) ) {
                out_why = "geometry wants two whole numbers, not '"
                        + us::toDisplayString( value ) + "'";
                return false;
            }
            out_case.width = (int) columns;
            out_case.height = (int) rows;
            continue;
        }

        out_why = "'" + us::toDisplayString( value )
                + "' is not an option; the options are layout( Name ) and "
                  "geometry( Columns, Rows )";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Checking one expectation against one observation.
// ---------------------------------------------------------------------------

/** The quantities an expectation can talk about. */
bool quantity( const Driver::Step& step, const std::string& what,
               long long& out, std::string& out_error )
{
    if( what == "tiles" ) {
        out = (long long) step.tiles;
        return true;
    }

    if( what != "cursor" && what != "top" && what != "row"
        && what != "rows" && what != "lines" ) {
        out_error = "no such quantity '" + what + "'";
        return false;
    }

    if( !step.scrollValid ) {
        out_error = "the focused panel does not report a viewport, so '"
                    + what + "' has no value here";
        return false;
    }

    if( what == "cursor" )     { out = step.scroll.cursorLine; }
    else if( what == "top" )   { out = step.scroll.topLine; }
    else if( what == "row" )   { out = step.scroll.cursorScreenRow(); }
    else if( what == "rows" )  { out = step.scroll.viewportRows; }
    else                       { out = step.scroll.totalLines; }
    return true;
}


std::string describe( const Expectation& expectation )
{
    std::ostringstream os;
    os << expectation.kind;
    for( std::size_t i = 0; i < expectation.args.size(); ++i ) {
        os << ( i ? ", " : "( " ) << expectation.args[i];
    }
    if( !expectation.args.empty() ) {
        os << " )";
    }
    return os.str();
}


/**
 * @return true if the expectation holds; otherwise false with a reason.
 *
 * An expectation this does not recognise is a FAILURE, never a pass. A spec
 * language that ignored what it did not understand would turn every typo
 * into a green test, which is the one outcome a suite must never have.
 */
bool checkExpectation( const Expectation& expectation,
                       const Driver::Step& now,
                       const Driver::Step& before,
                       std::string& out_reason )
{
    const std::string& kind = expectation.kind;
    const std::vector<std::string>& args = expectation.args;
    std::ostringstream reason;

    if( kind == "visible" && args.empty() ) {
        if( !now.scrollValid ) {
            out_reason = "the focused panel does not report a viewport";
            return false;
        }
        if( !now.scroll.cursorVisible() ) {
            reason << "the cursor is off screen (row "
                   << now.scroll.cursorScreenRow() << " of "
                   << now.scroll.viewportRows << ")";
            out_reason = reason.str();
            return false;
        }
        return true;
    }

    if( kind == "panel" && args.size() == 1 ) {
        if( now.focusedPanel != args[0] ) {
            reason << "the focused panel is '" << now.focusedPanel
                   << "', not '" << args[0] << "'";
            out_reason = reason.str();
            return false;
        }
        return true;
    }

    if( kind == "message" && args.size() == 1 ) {
        if( now.message.find( args[0] ) == std::string::npos ) {
            reason << "the status line is '" << now.message
                   << "', which does not contain '" << args[0] << "'";
            out_reason = reason.str();
            return false;
        }
        return true;
    }

    if( kind == "still" && args.size() == 1 ) {
        long long nowValue = 0;
        long long beforeValue = 0;
        std::string error;
        if( !quantity( now, args[0], nowValue, error )
            || !quantity( before, args[0], beforeValue, error ) ) {
            out_reason = error;
            return false;
        }
        if( nowValue != beforeValue ) {
            reason << args[0] << " changed from " << beforeValue << " to "
                   << nowValue << ", and should not have";
            out_reason = reason.str();
            return false;
        }
        return true;
    }

    if( kind == "moved" && args.size() == 3 ) {
        const std::string& what = args[0];
        const std::string& direction = args[1];
        long long distance = 0;

        if( !asInt( args[2], distance ) ) {
            out_reason = "moved wants a whole number of lines, not '"
                         + args[2] + "'";
            return false;
        }
        if( direction != "up" && direction != "down" ) {
            out_reason = "moved wants 'up' or 'down', not '" + direction + "'";
            return false;
        }

        long long nowValue = 0;
        long long beforeValue = 0;
        std::string error;
        if( !quantity( now, what, nowValue, error )
            || !quantity( before, what, beforeValue, error ) ) {
            out_reason = error;
            return false;
        }

        const long long wanted = beforeValue
            + ( direction == "down" ? distance : -distance );
        if( nowValue != wanted ) {
            reason << what << " went from " << beforeValue << " to " << nowValue
                   << "; " << distance << " " << direction
                   << " would have made it " << wanted;
            out_reason = reason.str();
            return false;
        }
        return true;
    }

    /* The value-equality family, last because it is the largest. */
    if( args.size() == 1 ) {
        long long wanted = 0;
        if( asInt( args[0], wanted ) ) {
            long long actual = 0;
            std::string error;
            if( !quantity( now, kind, actual, error ) ) {
                out_reason = error;
                return false;
            }
            if( actual != wanted ) {
                reason << kind << " is " << actual << ", not " << wanted;
                out_reason = reason.str();
                return false;
            }
            return true;
        }
    }

    out_reason = "no such expectation: " + describe( expectation )
               + " (see src/app/spec.hpp for the vocabulary)";
    return false;
}

// ---------------------------------------------------------------------------
// Running one case.
// ---------------------------------------------------------------------------

/** @return empty on success, else the failure, ready to print. */
std::string runCase( const Case& entry )
{
    Driver driver( entry.layout, entry.width, entry.height );
    std::ostringstream failure;

    for( std::size_t i = 0; i < entry.presses.size(); ++i ) {
        const Press& press = entry.presses[i];

        const std::map<int, std::vector<Expectation> >::const_iterator found =
            entry.expectations.find( press.index );
        const std::vector<Expectation> none;
        const std::vector<Expectation>& expectations =
            found == entry.expectations.end() ? none : found->second;

        for( int repetition = 0; repetition < press.times; ++repetition ) {
            if( !driver.press( press.keys ) ) {
                failure << "step " << press.index << ": '" << press.keys
                        << "' is not a key sequence";
                return failure.str();
            }

            for( std::size_t e = 0; e < expectations.size(); ++e ) {
                std::string reason;
                if( checkExpectation( expectations[e], driver.last(),
                                      driver.previous(), reason ) ) {
                    continue;
                }

                failure << "step " << press.index << " (" << press.keys << ")";
                if( press.times > 1 ) {
                    failure << " repetition " << repetition + 1
                            << " of " << press.times;
                }
                failure << ": " << reason
                        << "\n        expected: " << describe( expectations[e] )
                        << driver.trace();
                return failure.str();
            }
        }
    }

    return std::string();
}

// ---------------------------------------------------------------------------
// Assembling the cases from the facts.
// ---------------------------------------------------------------------------

bool pressLess( const Press& a, const Press& b )
{
    return a.index < b.index;
}


/**
 * Report a row whose bindings this file cannot read.
 *
 * Against its own case when the id survived the crossing, and as a case of
 * its own when it did not -- never dropped. Dropping is the failure mode the
 * whole file is built against: a fact that silently does nothing is a test
 * that silently does not run, and that looks exactly like a green one.
 *
 * A row is reported here rather than at the read that would have used it,
 * because the compound may have been in ANY argument -- including `$id`
 * itself, which is why the id is looked up rather than assumed.
 */
void rejectRow( const Row& row, const char* what,
                const std::map<std::string, std::size_t>& byId,
                std::vector<Case>& out_cases )
{
    const std::string id = field( row, "$id" );
    const std::string complaint = std::string( what ) + ": " + row.complaint;

    const std::map<std::string, std::size_t>::const_iterator found =
        byId.find( id );
    if( found != byId.end() ) {
        out_cases[ found->second ].malformed = complaint;
        return;
    }

    Case entry;
    entry.id = id;
    entry.name = "<" + std::string( what )
               + ( id.empty() ? std::string( " whose case id is unreadable" )
                              : " for unknown case '" + id + "'" )
               + ">";
    entry.malformed = complaint;
    out_cases.push_back( entry );
}


/**
 * Add a case, and refuse to let a second one answer to the same id.
 *
 * The flat and the nested form are two ways of writing the same thing, so
 * nothing stops an author writing both -- and then one of them is dead code
 * that looks like a running test. `byId` keeps pointing at the FIRST, so the
 * facts that already found their case keep it, and the newcomer is the one
 * that is red.
 */
void addCase( Case& entry, std::map<std::string, std::size_t>& byId,
              std::vector<Case>& out_cases )
{
    if( byId.count( entry.id ) != 0 ) {
        if( entry.malformed.empty() ) {
            entry.malformed = "there is already a case( " + entry.id
                            + ", ... ); an id names exactly one case";
        }
    } else {
        byId[ entry.id ] = out_cases.size();
    }
    out_cases.push_back( entry );
}


/**
 * @return true, having spoiled the case, if a flat fact aimed at a nested
 *         one. See Case::nested for why this is refused and not merged.
 */
bool refuseFlatFact( Case& entry, const char* what )
{
    if( !entry.nested ) {
        return false;
    }
    if( entry.malformed.empty() ) {
        entry.malformed = "case '" + entry.id + "' is written as one nested "
                          "term, which already carries its steps in order, so "
                          "the flat " + std::string( what )
                        + "( " + entry.id + ", ... ) beside it cannot be part "
                          "of it";
    }
    return true;
}


void readCases( us::LocalSession& session, Collector& collector,
                std::vector<Case>& out_cases )
{
    std::map<std::string, std::size_t> byId;

    /*
     * The nested form first, so that every id it claims is already known when
     * the flat facts below look theirs up -- which is what turns a flat press
     * aimed at a nested case into a refusal rather than a silent append.
     */
    struct Nested { const char* goal; bool options; };
    static const Nested kNested[] = {
        { "case( $id, $name, $steps )",           false },
        { "case( $id, $name, $options, $steps )", true  },
    };

    for( std::size_t s = 0; s < sizeof( kNested ) / sizeof( kNested[0] ); ++s ) {
        const std::vector<Row> rows =
            query( session, collector, kNested[s].goal );

        for( std::size_t i = 0; i < rows.size(); ++i ) {
            Case entry;
            entry.nested = true;

            /*
             * NOT rows[i].complaint: for these goals it is always set, since
             * `$steps` is a list. The nested reader asks for a leaf only
             * where it wants a word, and this is one of the two places.
             */
            std::string why;
            const us::Value* idValue = binding( rows[i], "$id" );
            if( !idValue || !flatten( *idValue, entry.id, why ) ) {
                entry.name = "<a case whose id is unreadable>";
                entry.malformed = "the case id "
                                + ( idValue ? why : std::string( "is missing" ) );
                out_cases.push_back( entry );
                continue;
            }

            const us::Value* nameValue = binding( rows[i], "$name" );
            if( !nameValue || !flatten( *nameValue, entry.name, why )
                || entry.name.empty() ) {
                entry.name = entry.id;
            }

            if( kNested[s].options ) {
                const us::Value* optionsValue = binding( rows[i], "$options" );
                if( !optionsValue ) {
                    entry.malformed = "the case has no options list";
                } else if( !readOptions( *optionsValue, entry, why ) ) {
                    entry.malformed = why;
                }
            }

            const us::Value* stepsValue = binding( rows[i], "$steps" );
            if( entry.malformed.empty() ) {
                if( !stepsValue ) {
                    entry.malformed = "the case has no steps list";
                } else if( !readSteps( *stepsValue, entry, why ) ) {
                    entry.malformed = why;
                }
            }

            addCase( entry, byId, out_cases );
        }
    }

    const std::vector<Row> cases =
        query( session, collector, "case( $id, $name )" );
    for( std::size_t i = 0; i < cases.size(); ++i ) {
        Case entry;
        entry.id = field( cases[i], "$id" );
        entry.name = field( cases[i], "$name" );
        if( entry.name.empty() ) {
            entry.name = entry.id.empty() ? "<a case with no readable id>"
                                          : entry.id;
        }
        if( !cases[i].complaint.empty() ) {
            entry.malformed = "case: " + cases[i].complaint;
        }
        addCase( entry, byId, out_cases );
    }

    /*
     * Everything below refers to a case by id. A fact naming an id that has
     * no case/2 is a typo, and a typo that silently did nothing would be a
     * test that silently does not run -- so it is reported against the file
     * rather than dropped.
     */
    struct Orphan { std::string what; std::string id; };
    std::vector<Orphan> orphans;

    const std::vector<Row> layouts =
        query( session, collector, "layout( $id, $layout )" );
    for( std::size_t i = 0; i < layouts.size(); ++i ) {
        if( !layouts[i].complaint.empty() ) {
            rejectRow( layouts[i], "layout", byId, out_cases );
            continue;
        }
        const std::string id = field( layouts[i], "$id" );
        if( byId.count( id ) == 0 ) {
            orphans.push_back( Orphan{ "layout", id } );
            continue;
        }
        if( refuseFlatFact( out_cases[ byId[id] ], "layout" ) ) {
            continue;
        }
        out_cases[ byId[id] ].layout = field( layouts[i], "$layout" );
    }

    const std::vector<Row> geometries =
        query( session, collector, "geometry( $id, $w, $h )" );
    for( std::size_t i = 0; i < geometries.size(); ++i ) {
        if( !geometries[i].complaint.empty() ) {
            rejectRow( geometries[i], "geometry", byId, out_cases );
            continue;
        }
        const std::string id = field( geometries[i], "$id" );
        if( byId.count( id ) == 0 ) {
            orphans.push_back( Orphan{ "geometry", id } );
            continue;
        }
        long long w = 0;
        long long h = 0;
        Case& entry = out_cases[ byId[id] ];
        if( refuseFlatFact( entry, "geometry" ) ) {
            continue;
        }
        if( !asInt( field( geometries[i], "$w" ), w )
            || !asInt( field( geometries[i], "$h" ), h ) ) {
            entry.malformed = "geometry wants two whole numbers";
            continue;
        }
        entry.width = (int) w;
        entry.height = (int) h;
    }

    /* press/4 first, then the press/3 shorthand for "once". */
    for( int arity = 4; arity >= 3; --arity ) {
        const std::string goal = ( arity == 4 )
            ? "press( $id, $step, $keys, $times )"
            : "press( $id, $step, $keys )";

        const std::vector<Row> presses = query( session, collector, goal );
        for( std::size_t i = 0; i < presses.size(); ++i ) {
            if( !presses[i].complaint.empty() ) {
                rejectRow( presses[i], "press", byId, out_cases );
                continue;
            }
            const std::string id = field( presses[i], "$id" );
            if( byId.count( id ) == 0 ) {
                orphans.push_back( Orphan{ "press", id } );
                continue;
            }
            Case& entry = out_cases[ byId[id] ];
            if( refuseFlatFact( entry, "press" ) ) {
                continue;
            }

            long long index = 0;
            if( !asInt( field( presses[i], "$step" ), index ) ) {
                entry.malformed = "a press step number must be a whole number";
                continue;
            }

            Press press;
            press.index = (int) index;
            press.keys = field( presses[i], "$keys" );
            press.times = 1;

            if( arity == 4 ) {
                long long times = 0;
                if( !asInt( field( presses[i], "$times" ), times ) || times < 0 ) {
                    entry.malformed = "a press repetition count must be a "
                                      "whole number";
                    continue;
                }
                press.times = (int) times;
            }
            entry.presses.push_back( press );
        }
    }

    /* expect/3, /4 and /6 -- the three shapes the vocabulary needs. */
    struct Shape { const char* goal; int args; };
    static const Shape kShapes[] = {
        { "expect( $id, $step, $what )", 0 },
        { "expect( $id, $step, $what, $a )", 1 },
        { "expect( $id, $step, $what, $a, $b, $c )", 3 },
    };

    for( std::size_t s = 0; s < sizeof( kShapes ) / sizeof( kShapes[0] ); ++s ) {
        const std::vector<Row> rows =
            query( session, collector, kShapes[s].goal );

        for( std::size_t i = 0; i < rows.size(); ++i ) {
            if( !rows[i].complaint.empty() ) {
                rejectRow( rows[i], "expect", byId, out_cases );
                continue;
            }
            const std::string id = field( rows[i], "$id" );
            if( byId.count( id ) == 0 ) {
                orphans.push_back( Orphan{ "expect", id } );
                continue;
            }
            Case& entry = out_cases[ byId[id] ];
            if( refuseFlatFact( entry, "expect" ) ) {
                continue;
            }

            long long index = 0;
            if( !asInt( field( rows[i], "$step" ), index ) ) {
                entry.malformed = "an expect step number must be a whole number";
                continue;
            }

            Expectation expectation;
            expectation.kind = field( rows[i], "$what" );
            if( kShapes[s].args >= 1 ) {
                expectation.args.push_back( field( rows[i], "$a" ) );
            }
            if( kShapes[s].args >= 3 ) {
                expectation.args.push_back( field( rows[i], "$b" ) );
                expectation.args.push_back( field( rows[i], "$c" ) );
            }
            entry.expectations[ (int) index ].push_back( expectation );
        }
    }

    /* Order, and the two ways a case can be incoherent. */
    for( std::size_t i = 0; i < out_cases.size(); ++i ) {
        Case& entry = out_cases[i];
        std::stable_sort( entry.presses.begin(), entry.presses.end(), pressLess );

        if( !entry.malformed.empty() ) {
            continue;
        }
        if( entry.presses.empty() ) {
            entry.malformed = "the case presses no keys";
            continue;
        }

        std::map<int, std::vector<Expectation> >::const_iterator it;
        for( it = entry.expectations.begin();
             it != entry.expectations.end(); ++it ) {
            bool pressed = false;
            for( std::size_t p = 0; p < entry.presses.size(); ++p ) {
                if( entry.presses[p].index == it->first ) { pressed = true; }
            }
            if( !pressed ) {
                std::ostringstream os;
                os << "step " << it->first
                   << " is expected of but never pressed";
                entry.malformed = os.str();
                break;
            }
        }
    }

    for( std::size_t i = 0; i < orphans.size(); ++i ) {
        Case entry;
        entry.id = orphans[i].id;
        entry.name = "<" + orphans[i].what + " for unknown case '"
                   + orphans[i].id + "'>";
        entry.malformed = "there is no case( " + orphans[i].id + ", ... )";
        out_cases.push_back( entry );
    }
}


std::string readFile( const std::string& path, bool& out_ok )
{
    std::ifstream stream( path.c_str(), std::ios::binary );
    if( !stream ) {
        out_ok = false;
        return std::string();
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out_ok = true;
    return buffer.str();
}

} // namespace


int runSpec( const std::string& path, std::ostream& out )
{
    bool readable = false;
    const std::string text = readFile( path, readable );
    if( !readable ) {
        out << "unify-lens: cannot read spec '" << path << "'\n";
        return 2;
    }

    vault::unify::setDebugTraceEnabled( false );

    us::LocalSession session;
    if( 0 != session.start() ) {
        out << "unify-lens: cannot start the Unify engine\n";
        return 2;
    }

    Collector collector;
    session.subscribe( collector, 0 );

    us::Origin origin;
    origin.kind = us::Origin::Kind::Module;
    origin.file = path;
    session.define( text, origin, us::OverwritePolicy::Append );
    session.waitUntilQuiet();

    /*
     * A spec that does not parse is a setup error, not a red suite -- and it
     * is reported with the engine's own diagnostic, since that is the one
     * the author of the file can act on.
     */
    {
        const std::vector<us::Event> events = collector.take();
        int errors = 0;
        for( std::size_t i = 0; i < events.size(); ++i ) {
            const us::Diagnostic* diagnostic =
                std::get_if<us::Diagnostic>( &events[i].body );
            if( !diagnostic || diagnostic->sev != us::Severity::Error ) {
                continue;
            }
            out << "unify-lens: " << path << ":" << diagnostic->line << ":"
                << diagnostic->column << ": " << diagnostic->message << "\n";
            ++errors;
        }
        if( errors ) {
            session.close();
            return 2;
        }
    }

    std::vector<Case> cases;
    readCases( session, collector, cases );
    session.close();

    if( cases.empty() ) {
        out << "unify-lens: '" << path
            << "' states no case( Id, Name ) -- nothing to run.\n";
        return 2;
    }

    out << "== " << path << " (" << cases.size() << " cases)\n";

    int failed = 0;
    for( std::size_t i = 0; i < cases.size(); ++i ) {
        const Case& entry = cases[i];

        std::string failure = entry.malformed;
        if( failure.empty() ) {
            failure = runCase( entry );
        }

        if( failure.empty() ) {
            out << "   ok   " << entry.name << "\n";
            continue;
        }

        ++failed;
        out << "   FAIL " << entry.name << "\n        " << failure << "\n";
    }

    out << "== " << path << ": " << ( cases.size() - (std::size_t) failed )
        << " passed, " << failed << " failed\n";

    return failed ? 1 : 0;
}

} // namespace lens
