/**
 * @file main.cpp
 *
 * The composition root -- ARCHITECTURE.md sections 1 and 7.
 *
 * Parses argv, builds a model, and either runs the terminal loop or replays
 * a key script headlessly. It is the only place that knows both the terminal
 * and the model exist; everything below it sees one or the other.
 *
 * `--script` is not only a test hook (ACCEPTANCE.md G1.7). It makes any lens
 * bug reproducible by a file, which matters more than usual for a program
 * whose bugs are otherwise reported as "the screen looked wrong".
 */

#include "layouts.hpp"
#include "session-bridge.hpp"

#include "../model/model.hpp"
#include "../model/view.hpp"

#if defined( LENS_HAVE_TERM )
#  include "../term/iterminal.hpp"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace lens;

struct Options {
    int         width = 0;         //!< 0 = ask the terminal
    int         height = 0;
    std::string layout = "browse";
    std::string scriptPath;
    bool        help = false;

    /**
     * Start a real engine.
     *
     * On by default interactively, and OFF by default under `--script`, so
     * that a shell golden stays a test of the shell. A screen recorded with
     * an engine attached would move whenever the engine's output moved,
     * which is exactly the coupling the golden corpus exists to avoid.
     */
    bool session = false;

    /**
     * Open the help panel on the first-steps page at startup.
     *
     * On interactively, because the first person to run lens could not tell
     * how to open a menu, close a window or find a tutorial -- and a screen
     * that does not say is a screen you have to be told about. Off under
     * `--script`, so a golden stays a test of what the script did.
     */
    bool welcome = false;

    /**
     * Emit an observation after every step instead of the final screen.
     *
     * "model" is the compact projection an interaction bug lives in --
     * which line is selected, where the viewport is, where the highlight
     * lands. "screen" is the whole grid per step, for when you need the film.
     *
     * This exists because the highlight is an ATTRIBUTE, and the screen dump
     * records characters only. A selection that moves changes nothing a
     * golden can see -- so without this there is no way to show, from
     * outside the program, that a key did what it should.
     */
    std::string trace;
};

void printUsage( std::FILE* out )
{
    std::fprintf( out,
        "usage: unify-lens [options]\n"
        "  --layout NAME         start in a named layout (default: browse)\n"
        "  --geometry COLSxROWS  force geometry; required with --script\n"
        "  --script FILE         replay a key script, dump the screen, exit\n"
        "  --session             start a Unify engine (default interactively)\n"
        "  --welcome             open help at startup (default interactively)\n"
        "  --no-welcome          start without the help panel\n"
        "  --trace=model         after each step print where the selection\n"
        "                        and viewport are, instead of the screen\n"
        "  --trace=screen        after each step print the whole screen\n"
        "  --no-session          do not start an engine\n"
        "  -h, --help            show this text\n"
        "\n"
        "Layouts: " );
    const std::vector<std::string> names = stockLayoutNames();
    for( std::size_t i = 0; i < names.size(); ++i ) {
        std::fprintf( out, "%s%s", i ? ", " : "", names[i].c_str() );
    }
    std::fprintf( out, "\n"
        "\n"
        "A key script is one key sequence per line, spelled as in the keymap\n"
        "(`C-x 2`, `Tab`, `F1`); blank lines and lines starting with # are\n"
        "ignored. The final screen is written to stdout.\n" );
}


bool parseGeometry( const std::string& text, int& out_w, int& out_h )
{
    const std::string::size_type x = text.find_first_of( "xX" );
    if( x == std::string::npos ) {
        return false;
    }
    out_w = std::atoi( text.substr( 0, x ).c_str() );
    out_h = std::atoi( text.substr( x + 1 ).c_str() );
    return out_w > 0 && out_h > 0;
}


bool parseArgs( int argc, char** argv, Options& out, std::string& out_error )
{
    for( int i = 1; i < argc; ++i ) {
        const std::string arg = argv[i];

        if( arg == "-h" || arg == "--help" ) {
            out.help = true;
            return true;
        }

        const char* value = NULL;
        std::string name = arg;
        const std::string::size_type eq = arg.find( '=' );
        if( eq != std::string::npos ) {
            name = arg.substr( 0, eq );
            value = argv[i] + eq + 1;
        }

        const auto takeValue = [ & ]( const char*& slot ) -> bool {
            if( value ) { slot = value; return true; }
            if( i + 1 >= argc ) { return false; }
            slot = argv[ ++i ];
            return true;
        };

        if( name == "--layout" ) {
            const char* v = NULL;
            if( !takeValue( v ) ) { out_error = "--layout needs a name"; return false; }
            out.layout = v;
        } else if( name == "--geometry" ) {
            const char* v = NULL;
            if( !takeValue( v ) ) { out_error = "--geometry needs COLSxROWS"; return false; }
            if( !parseGeometry( v, out.width, out.height ) ) {
                out_error = std::string( "cannot parse geometry '" ) + v + "'";
                return false;
            }
        } else if( name == "--script" ) {
            const char* v = NULL;
            if( !takeValue( v ) ) { out_error = "--script needs a file"; return false; }
            out.scriptPath = v;
        } else if( name == "--trace" ) {
            const char* v = NULL;
            if( !takeValue( v ) ) {
                out_error = "--trace needs model or screen";
                return false;
            }
            out.trace = v;
            if( out.trace != "model" && out.trace != "screen" ) {
                out_error = "--trace must be 'model' or 'screen'";
                return false;
            }
        } else if( name == "--welcome" ) {
            out.welcome = true;
        } else if( name == "--no-welcome" ) {
            out.welcome = false;
        } else if( name == "--session" ) {
            out.session = true;
        } else if( name == "--no-session" ) {
            out.session = false;
        } else {
            out_error = "unknown option '" + arg + "'";
            return false;
        }
    }
    return true;
}


/**
 * One step of a key script.
 *
 * `resize COLSxROWS` is a step too, not just keys: G1.3's claim is about
 * what a resize does to a layout, and a script that could only type keys
 * could not express it. It also makes the 80x24 degradation reproducible
 * from a file rather than from remembering to pass a different --geometry.
 */
struct ScriptStep {
    enum class Kind { Keys, Resize };
    Kind   kind = Kind::Keys;
    KeySeq keys;
    int    width = 0;
    int    height = 0;
};


/** One step per line; `#` comments and blank lines ignored. */
bool readKeyScript( const std::string& path, std::vector<ScriptStep>& out_steps,
                    std::string& out_error )
{
    std::ifstream in( path.c_str() );
    if( !in ) {
        out_error = "cannot open script '" + path + "'";
        return false;
    }

    std::string line;
    int lineNumber = 0;
    while( std::getline( in, line ) ) {
        ++lineNumber;

        {
            const std::string::size_type lead = line.find_first_not_of( " \t\r" );
            const bool isType =
                lead != std::string::npos
                && line.compare( lead, 5, "type " ) == 0;
            if( !isType ) {
                const std::string::size_type hash = line.find( '#' );
                if( hash != std::string::npos ) {
                    line = line.substr( 0, hash );
                }
            }
        }
        if( line.find_first_not_of( " \t\r" ) == std::string::npos ) {
            continue;
        }

        const std::string::size_type first = line.find_first_not_of( " \t\r" );
        const std::string trimmed = line.substr( first );

        if( trimmed.compare( 0, 5, "type " ) == 0 ) {
            /*
             * Literal text, one key per character. Without this a script
             * that types a goal is forty `U+0063` lines, which nobody can
             * read and therefore nobody will maintain -- and an unreadable
             * repro script is not a repro script.
             *
             * Note it deliberately does NOT strip a trailing `#`: a comment
             * marker inside typed text is text. Comments are only stripped
             * from lines that are not `type`.
             */
            const std::string text = line.substr( first + 5 );
            ScriptStep step;
            step.kind = ScriptStep::Kind::Keys;
            const std::vector<char32_t> codepoints = decodeUtf8( text );
            for( std::size_t i = 0; i < codepoints.size(); ++i ) {
                step.keys.push_back( Key::character( codepoints[i] ) );
            }
            if( !step.keys.empty() ) {
                out_steps.push_back( step );
            }
            continue;
        }

        if( trimmed.compare( 0, 7, "resize " ) == 0 ) {
            ScriptStep step;
            step.kind = ScriptStep::Kind::Resize;
            if( !parseGeometry( trimmed.substr( 7 ), step.width, step.height ) ) {
                std::ostringstream os;
                os << path << ":" << lineNumber << ": cannot parse geometry '"
                   << trimmed.substr( 7 ) << "'";
                out_error = os.str();
                return false;
            }
            out_steps.push_back( step );
            continue;
        }

        ScriptStep step;
        step.kind = ScriptStep::Kind::Keys;
        if( !parseKeySeq( trimmed, step.keys ) ) {
            std::ostringstream os;
            os << path << ":" << lineNumber << ": cannot parse key sequence '"
               << trimmed << "'";
            out_error = os.str();
            return false;
        }
        out_steps.push_back( step );
    }
    return true;
}


void buildModel( Model& model, const Options& options, int width, int height )
{
    model.setGeometry( width, height );
    registerShellCommands( model );
    model.keymap() = defaultKeymap();
    applyStockLayout( model, options.layout );

    /* After both, so the generated keymap page reflects real bindings. */
    model.rebuildHelp();

    if( options.welcome ) {
        model.openHelp( HelpBook::welcomeTopicId(), /* takeLargestTile */ true );
    }

    std::ostringstream status;
    status << "gen 0 \xc2\xb7 0 clauses \xc2\xb7 idle \xc2\xb7 no session \xc2\xb7 "
           << options.layout;
    model.setStatus( status.str() );
}


/** One `--trace=model` record: what a user would perceive right now. */
std::string traceLine( const Model& model, const std::string& what )
{
    std::ostringstream os;
    os << what;
    while( os.str().size() < 14 ) { os << ' '; }

    const Buffer* focused = model.focusedBuffer();
    os << " focus=" << ( focused ? focused->title : std::string( "-" ) );

    bool valid = false;
    const ScrollView scroll = model.observeFocusedScroll( valid );
    if( valid ) {
        os << " line=" << scroll.cursorLine
           << " top=" << scroll.topLine
           << " row=" << scroll.cursorScreenRow()
           << " rows=" << scroll.viewportRows
           << " of=" << scroll.totalLines;
    }
    os << " tiles=" << model.layout().tileCount();
    if( !model.message().empty() ) {
        os << " msg=\"" << model.message() << "\"";
    }
    return os.str();
}


int runScript( const Options& options )
{
    /*
     * Headless. A geometry is required rather than guessed: the whole point
     * of this mode is that its output is reproducible, and inheriting
     * whatever terminal happened to be attached would make a golden depend
     * on the window it was recorded in.
     */
    if( options.width <= 0 || options.height <= 0 ) {
        std::fprintf( stderr, "unify-lens: --script requires --geometry\n" );
        return 2;
    }

    if( options.width < Model::kMinWidth || options.height < Model::kMinHeight ) {
        std::fprintf( stderr, "%s\n",
            Model::tooSmallMessage( options.width, options.height ).c_str() );
        return 1;
    }

    Model model;
    buildModel( model, options, options.width, options.height );

    SessionBridge bridge;
    if( options.session ) {
        const std::string error = bridge.start();
        if( !error.empty() ) {
            std::fprintf( stderr, "unify-lens: %s\n", error.c_str() );
            return 2;
        }
        model.setCapabilities( bridge.capabilities() );
    }

    /*
     * Everything the script does, then everything the engine has to say
     * about it, before the next step. A golden screen has to be a function
     * of the script rather than of how fast this machine is.
     */
    const auto settle = [ & ]() {
        if( !options.session ) {
            return;
        }
        bridge.settle();
        const std::vector<us::Event> events = bridge.drain();
        for( std::size_t i = 0; i < events.size(); ++i ) {
            model.foldSession( events[i] );
        }
    };

    /* The layout name was validated in main() before anything was built. */
    std::vector<ScriptStep> script;
    std::string error;
    if( !readKeyScript( options.scriptPath, script, error ) ) {
        std::fprintf( stderr, "unify-lens: %s\n", error.c_str() );
        return 2;
    }

    const auto emitTrace = [ & ]( const std::string& what ) {
        if( options.trace == "model" ) {
            std::printf( "%s\n", traceLine( model, what ).c_str() );
        } else if( options.trace == "screen" ) {
            std::printf( "--- %s\n%s", what.c_str(),
                         view( model ).toText().c_str() );
        }
    };
    emitTrace( "<start>" );

    for( std::size_t i = 0; i < script.size(); ++i ) {
        const ScriptStep& step = script[i];

        if( step.kind == ScriptStep::Kind::Resize ) {
            if( step.width < Model::kMinWidth
                || step.height < Model::kMinHeight ) {
                std::fprintf( stderr, "%s\n",
                    Model::tooSmallMessage( step.width, step.height ).c_str() );
                return 1;
            }
            Event event;
            event.kind = Event::Kind::Resize;
            event.width = step.width;
            event.height = step.height;
            ( void ) fold( model, event );
            {
                std::ostringstream what;
                what << "resize " << step.width << "x" << step.height;
                emitTrace( what.str() );
            }
            continue;
        }

        for( std::size_t k = 0; k < step.keys.size(); ++k ) {
            Event event;
            event.kind = Event::Kind::Key;
            event.key = step.keys[k];

            const std::vector<CommandRequest> requests = fold( model, event );
            for( std::size_t r = 0; r < requests.size(); ++r ) {
                bridge.issue( model, requests[r] );
            }
            if( !requests.empty() ) {
                settle();
            }
            emitTrace( step.keys[k].toString() );
        }
        if( model.quitting() ) {
            break;
        }
    }

    settle();

    if( !options.trace.empty() ) {
        return 0;   /* you asked for the film, not the frame */
    }

    const CellGrid grid = view( model );
    std::fputs( grid.toText().c_str(), stdout );
    return 0;
}


#if defined( LENS_HAVE_TERM )
int runInteractive( const Options& options )
{
    std::string error;
    std::unique_ptr<ITerminal> terminal = makeTerminal( error );
    if( !terminal ) {
        std::fprintf( stderr, "%s\n", error.c_str() );
        return 2;
    }

    TerminalCapabilities caps = terminal->capabilities();
    int width = options.width > 0 ? options.width : caps.width;
    int height = options.height > 0 ? options.height : caps.height;

    /*
     * ACCEPTANCE.md G1.4: below the minimum lens exits with the pinned
     * message and a non-zero status. It does NOT render something
     * illegible and hope, and it does not silently pick a different layout.
     */
    if( width < Model::kMinWidth || height < Model::kMinHeight ) {
        std::fprintf( stderr, "%s\n",
            Model::tooSmallMessage( width, height ).c_str() );
        return 1;
    }

    Model model;
    buildModel( model, options, width, height );

    SessionBridge bridge;
    if( options.session ) {
        const std::string sessionError = bridge.start();
        if( !sessionError.empty() ) {
            std::fprintf( stderr, "unify-lens: %s\n", sessionError.c_str() );
            return 2;
        }
        model.setCapabilities( bridge.capabilities() );
    }

    for( ;; ) {
        /*
         * Session events first, then the terminal. The UI thread NEVER
         * blocks on the session -- it drains whatever has arrived and
         * carries on, which is the rule the whole boundary was shaped
         * around.
         */
        const std::vector<us::Event> events = bridge.drain();
        for( std::size_t i = 0; i < events.size(); ++i ) {
            model.foldSession( events[i] );
        }

        terminal->draw( view( model ) );

        const TerminalEvent te = terminal->poll( 50 );
        if( te.kind == TerminalEvent::Kind::Closed ) {
            break;
        }

        Event event;
        if( te.kind == TerminalEvent::Kind::Key ) {
            event.kind = Event::Kind::Key;
            event.key = te.key;
        } else if( te.kind == TerminalEvent::Kind::Resize ) {
            if( te.width < Model::kMinWidth || te.height < Model::kMinHeight ) {
                std::fprintf( stderr, "%s\n",
                    Model::tooSmallMessage( te.width, te.height ).c_str() );
                return 1;
            }
            event.kind = Event::Kind::Resize;
            event.width = te.width;
            event.height = te.height;
        } else {
            continue;
        }

        const std::vector<CommandRequest> requests = fold( model, event );
        for( std::size_t i = 0; i < requests.size(); ++i ) {
            bridge.issue( model, requests[i] );
        }

        if( model.quitting() ) {
            break;
        }
    }

    return 0;
}
#endif

} // namespace

int main( int argc, char** argv )
{
    Options options;
    std::string error;

    if( !parseArgs( argc, argv, options, error ) ) {
        std::fprintf( stderr, "unify-lens: %s\n", error.c_str() );
        printUsage( stderr );
        return 2;
    }

    if( options.help ) {
        printUsage( stdout );
        return 0;
    }

    {
        /* Reject an unknown layout before anything else happens. */
        Model probe;
        if( !applyStockLayout( probe, options.layout ) ) {
            std::fprintf( stderr, "unify-lens: no such layout '%s'\n",
                          options.layout.c_str() );
            printUsage( stderr );
            return 2;
        }
    }

    if( !options.scriptPath.empty() ) {
        return runScript( options );
    }

    /*
     * Interactively an engine is the point, so it is the default; under
     * --script it is opt-in, so a shell golden stays a test of the shell.
     */
    {
        bool sessionOff = false;
        bool welcomeOff = false;
        for( int i = 1; i < argc; ++i ) {
            const std::string arg = argv[i];
            if( arg == "--no-session" ) { sessionOff = true; }
            if( arg == "--no-welcome" ) { welcomeOff = true; }
        }
        if( !options.session ) { options.session = !sessionOff; }
        if( !options.welcome ) { options.welcome = !welcomeOff; }
    }

#if defined( LENS_HAVE_TERM )
    return runInteractive( options );
#else
    std::fprintf( stderr,
        "unify-lens: built without a terminal backend "
        "(-DLENS_BUILD_TERM=OFF).\n"
        "  Only --script is available in this build.\n" );
    return 2;
#endif
}
