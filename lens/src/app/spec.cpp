/**
 * @file spec.cpp
 */

#include "spec.hpp"

#include "driver.hpp"

#include "vault-unify-local-session.hpp"
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


/** One solution, as a variable-name to text map. */
typedef std::map<std::string, std::string> Row;


/**
 * Run one goal and return every solution.
 *
 * Bindings arrive as text -- see the note in spec.hpp about why this file
 * asks the engine only for atomic values.
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
            row[ solution->bindings[b].first ] = solution->bindings[b].second.name;
        }
        rows.push_back( row );
    }

    return rows;
}


std::string field( const Row& row, const char* name )
{
    const Row::const_iterator it = row.find( name );
    return it == row.end() ? std::string() : it->second;
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
};

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


void readCases( us::LocalSession& session, Collector& collector,
                std::vector<Case>& out_cases )
{
    std::map<std::string, std::size_t> byId;

    const std::vector<Row> cases =
        query( session, collector, "case( $id, $name )" );
    for( std::size_t i = 0; i < cases.size(); ++i ) {
        Case entry;
        entry.id = field( cases[i], "$id" );
        entry.name = field( cases[i], "$name" );
        if( entry.name.empty() ) {
            entry.name = entry.id;
        }
        byId[ entry.id ] = out_cases.size();
        out_cases.push_back( entry );
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
        const std::string id = field( layouts[i], "$id" );
        if( byId.count( id ) == 0 ) {
            orphans.push_back( Orphan{ "layout", id } );
            continue;
        }
        out_cases[ byId[id] ].layout = field( layouts[i], "$layout" );
    }

    const std::vector<Row> geometries =
        query( session, collector, "geometry( $id, $w, $h )" );
    for( std::size_t i = 0; i < geometries.size(); ++i ) {
        const std::string id = field( geometries[i], "$id" );
        if( byId.count( id ) == 0 ) {
            orphans.push_back( Orphan{ "geometry", id } );
            continue;
        }
        long long w = 0;
        long long h = 0;
        Case& entry = out_cases[ byId[id] ];
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
            const std::string id = field( presses[i], "$id" );
            if( byId.count( id ) == 0 ) {
                orphans.push_back( Orphan{ "press", id } );
                continue;
            }
            Case& entry = out_cases[ byId[id] ];

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
            const std::string id = field( rows[i], "$id" );
            if( byId.count( id ) == 0 ) {
                orphans.push_back( Orphan{ "expect", id } );
                continue;
            }
            Case& entry = out_cases[ byId[id] ];

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
