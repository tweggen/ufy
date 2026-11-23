/**
 * @file pty-interaction-test.cpp
 *
 * The fourth test category, and the one that finally covers src/term/.
 *
 * ftxui-terminal.cpp says of itself: "its INTERACTIVE behaviour -- that a
 * real terminal produces the keys this expects, that resize arrives, that
 * the alternate screen is restored on exit -- is not verified in the
 * environment this was written in, which has no tty."
 *
 * That exemption cost a user-visible bug: every keystroke appeared to take
 * effect one keystroke late. Nothing on the other side of the ITerminal seam
 * could see it. The model was right, the grid handed to draw() was right,
 * the goldens were right -- and the frame was never put on the screen,
 * because FTXUI skips a redraw when no event of its own was processed and
 * lens mutates the grid out of band.
 *
 * So this suite opens a real pseudo-terminal, runs the real unify-lens
 * binary on it, types at it, and reads what comes back. No fake, no seam, no
 * exemption. It is deliberately small -- three cases, seconds to run -- and
 * deliberately not the place to test behaviour: everything that CAN be
 * asserted on the model side still is, in interaction-test.cpp. What is left
 * here is exactly the question a fake terminal cannot answer:
 *
 *     when the user presses a key, does a frame reach the screen?
 *
 * POSIX only. Windows has ConPTY, which is a different enough API to be its
 * own file if it is ever worth writing; the bug class this catches is not
 * platform-specific, so catching it on one platform catches it.
 */

#include "../../unify/test/session/test-harness.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined( __APPLE__ ) || defined( __FreeBSD__ )
#  include <util.h>
#endif

namespace {

using namespace unify_test;

/** The binary under test, from argv[1]. */
std::string g_lensBinary;

/**
 * Everything a terminal emulator sends that is not text.
 *
 * Stripped before matching, because what a frame LOOKS like is not the
 * subject here -- whether it arrived is. FTXUI splits a run of text at every
 * attribute change, so "Menu" in a bold panel title can reach us with an SGR
 * sequence sitting in the middle of it; searching the raw stream would make
 * this suite fail on a colour change.
 */
std::string stripEscapes( const std::string& raw )
{
    std::string out;
    std::size_t i = 0;

    while( i < raw.size() ) {
        if( (unsigned char) raw[i] != 0x1B ) {
            out += raw[i];
            ++i;
            continue;
        }

        ++i;   /* the ESC itself */
        if( i >= raw.size() ) {
            break;
        }

        if( raw[i] == '[' ) {
            /* CSI: parameters, then one final byte in 0x40..0x7E. */
            ++i;
            while( i < raw.size()
                   && ( (unsigned char) raw[i] < 0x40
                        || (unsigned char) raw[i] > 0x7E ) ) {
                ++i;
            }
            ++i;
        } else if( raw[i] == ']' ) {
            /* OSC: up to BEL or ST. */
            ++i;
            while( i < raw.size() && raw[i] != '\a' ) {
                if( (unsigned char) raw[i] == 0x1B
                    && i + 1 < raw.size() && raw[i + 1] == '\\' ) {
                    ++i;
                    break;
                }
                ++i;
            }
            ++i;
        } else {
            ++i;   /* ESC + one byte */
        }
    }

    return out;
}


/** The last few hundred characters of what was seen, for a failure message. */
std::string tail( const std::string& text, std::size_t limit = 600 )
{
    if( text.size() <= limit ) {
        return text;
    }
    return "..." + text.substr( text.size() - limit );
}


/**
 * unify-lens, running on a pseudo-terminal we hold the other end of.
 *
 * The child gets the slave as a controlling terminal, which is what makes
 * `isatty` true and lets makeTerminal() take the path a user takes. Anything
 * less -- a pipe, a fake -- and the file under test is not the file running.
 */
class LensOnPty {
public:
    LensOnPty( int columns, int rows )
    {
        m_master = ::posix_openpt( O_RDWR | O_NOCTTY );
        if( m_master < 0 ) {
            UT_SKIP( "no pseudo-terminal available (posix_openpt: "
                     << std::strerror( errno ) << ")" );
        }
        if( 0 != ::grantpt( m_master ) || 0 != ::unlockpt( m_master ) ) {
            UT_SKIP( "cannot grant/unlock a pseudo-terminal" );
        }

        const char* slaveName = ::ptsname( m_master );
        if( !slaveName ) {
            UT_SKIP( "ptsname failed" );
        }
        const std::string slavePath = slaveName;

        /*
         * The size is set on the master BEFORE the fork, so the child's
         * first TIOCGWINSZ already answers 120x40 -- lens reads the terminal
         * size once at startup and would otherwise lay out for whatever the
         * pty defaults to.
         */
        struct winsize ws;
        std::memset( &ws, 0, sizeof( ws ) );
        ws.ws_col = (unsigned short) columns;
        ws.ws_row = (unsigned short) rows;
        ::ioctl( m_master, TIOCSWINSZ, &ws );

        m_child = ::fork();
        if( m_child < 0 ) {
            UT_FAIL( "fork failed: " << std::strerror( errno ) );
        }

        if( 0 == m_child ) {
            /* Child. Nothing here may return; _exit on every failure. */
            ::setsid();

            const int slave = ::open( slavePath.c_str(), O_RDWR );
            if( slave < 0 ) {
                ::_exit( 127 );
            }
#if defined( TIOCSCTTY )
            ::ioctl( slave, TIOCSCTTY, 0 );
#endif
            ::dup2( slave, STDIN_FILENO );
            ::dup2( slave, STDOUT_FILENO );
            ::dup2( slave, STDERR_FILENO );
            if( slave > STDERR_FILENO ) {
                ::close( slave );
            }
            ::close( m_master );

            ::setenv( "TERM", "xterm-256color", 1 );
            ::unsetenv( "COLORTERM" );

            ::execl( g_lensBinary.c_str(), g_lensBinary.c_str(),
                     "--no-session", "--welcome", (char*) NULL );
            ::_exit( 127 );
        }
    }

    ~LensOnPty()
    {
        if( m_child > 0 ) {
            ::kill( m_child, SIGKILL );
            int status = 0;
            ::waitpid( m_child, &status, 0 );
        }
        if( m_master >= 0 ) {
            ::close( m_master );
        }
    }

    void send( const std::string& bytes )
    {
        const ssize_t written = ::write( m_master, bytes.data(), bytes.size() );
        if( written != (ssize_t) bytes.size() ) {
            UT_FAIL( "short write to the pty" );
        }
    }

    /** Read whatever arrives for `ms`, appending to the transcript. */
    void drainFor( int ms )
    {
        waitFor( std::string(), ms );
    }

    /**
     * Read until `needle` appears in the (escape-stripped) transcript, or
     * `ms` elapses. An empty needle just reads for the whole time.
     */
    bool waitFor( const std::string& needle, int ms )
    {
        const int kSliceMs = 25;
        int waited = 0;

        for( ;; ) {
            if( !needle.empty() && m_text.find( needle ) != std::string::npos ) {
                return true;
            }
            if( waited >= ms ) {
                return !needle.empty()
                       && m_text.find( needle ) != std::string::npos;
            }

            struct pollfd pfd;
            pfd.fd = m_master;
            pfd.events = POLLIN;
            pfd.revents = 0;

            const int ready = ::poll( &pfd, 1, kSliceMs );
            waited += kSliceMs;

            if( ready > 0 ) {
                char buffer[ 4096 ];
                const ssize_t got = ::read( m_master, buffer, sizeof( buffer ) );
                if( got > 0 ) {
                    m_raw.append( buffer, (std::size_t) got );
                    m_text = stripEscapes( m_raw );
                } else if( got == 0 || ( got < 0 && errno == EIO ) ) {
                    /* The child closed the slave: EOF, EIO on Linux. */
                    m_eof = true;
                    return !needle.empty()
                           && m_text.find( needle ) != std::string::npos;
                }
            }
        }
    }

    /** Forget everything seen so far, so the next assertion is about new output. */
    void clear()
    {
        m_raw.clear();
        m_text.clear();
    }

    /** Wait for the child to exit; returns false on timeout. */
    bool waitExit( int ms, int& out_status )
    {
        const int kSliceMs = 25;
        for( int waited = 0; waited <= ms; waited += kSliceMs ) {
            /* Keep reading: a full pty buffer would block the child's exit. */
            drainFor( kSliceMs );

            const pid_t done = ::waitpid( m_child, &out_status, WNOHANG );
            if( done == m_child ) {
                m_child = -1;
                return true;
            }
        }
        return false;
    }

    const std::string& text() const { return m_text; }
    bool eof() const { return m_eof; }

private:
    int   m_master = -1;
    pid_t m_child = -1;
    std::string m_raw;
    std::string m_text;
    bool  m_eof = false;
};


/** F10 as a terminal sends it, and the C-x C-c chord that quits lens. */
const char* const kF10 = "\x1B[21~";
const char* const kQuit = "\x18\x03";

/** Something only the welcome screen's browse layout draws. */
const char* const kWelcomeMarker = "Catalogue";

/** Something only the menu draws -- the panel title, box corner included. */
const char* const kMenuMarker = "\xE2\x94\x8C Menu";

} // namespace


int main( int argc, char** argv )
{
    if( argc < 2 ) {
        std::fprintf( stderr,
                      "usage: %s <path-to-unify-lens>\n", argv[0] );
        return 2;
    }
    g_lensBinary = argv[1];

    Registry registry;

    /*
     * The baseline. If this fails, the two below tell you nothing -- lens
     * did not start, or the pty is not usable in this environment.
     */
    registry.add( "lens draws a first frame on a real terminal", []() {
        LensOnPty lens( 120, 40 );

        UT_CHECK_MSG( lens.waitFor( kWelcomeMarker, 5000 ),
                      "no first frame within 5s; lens wrote:\n"
                          << tail( lens.text() ) );
    } );

    /*
     * THE ONE THIS FILE EXISTS FOR.
     *
     * One keystroke in, one frame out. With the FTXUI redraw bug, the F10
     * arrives, the model opens the menu, lens hands the new grid to draw()
     * -- and FTXUI returns early without rendering it, because from its
     * point of view nothing happened. The menu then appears only when the
     * NEXT key is pressed, which is exactly what "the UI reacts one
     * keystroke late" feels like from the other side of the screen.
     *
     * So: press once, and require the frame WITHOUT pressing again.
     */
    registry.add( "one keystroke produces its frame, with no second keystroke",
                  []() {
        LensOnPty lens( 120, 40 );

        UT_CHECK_MSG( lens.waitFor( kWelcomeMarker, 5000 ),
                      "no first frame within 5s; lens wrote:\n"
                          << tail( lens.text() ) );

        /* Let the first frame finish, then forget it. */
        lens.drainFor( 300 );
        lens.clear();

        lens.send( kF10 );

        UT_CHECK_MSG( lens.waitFor( kMenuMarker, 3000 ),
                      "F10 was pressed and the menu never reached the screen "
                      "within 3s.\n"
                      "  The model opened it -- interaction-test.cpp proves "
                      "that -- so the frame\n"
                      "  was computed and dropped: the terminal backend is "
                      "not presenting what\n"
                      "  draw() was given. Everything after this keystroke is "
                      "one keystroke stale.\n"
                      "  What arrived instead:\n"
                          << tail( lens.text() ) );
    } );

    /*
     * The chord still works through a real terminal, and lens leaves. This
     * is also what proves the pty is being read the way a user's terminal
     * would read it: C-x C-c is two bytes with no escape sequence around
     * them, and a backend that swallowed the first would hang here.
     */
    registry.add( "C-x C-c quits, and lens exits 0", []() {
        LensOnPty lens( 120, 40 );

        UT_CHECK_MSG( lens.waitFor( kWelcomeMarker, 5000 ),
                      "no first frame within 5s; lens wrote:\n"
                          << tail( lens.text() ) );

        lens.drainFor( 300 );
        lens.send( kQuit );

        int status = 0;
        UT_CHECK_MSG( lens.waitExit( 5000, status ),
                      "C-x C-c did not make lens exit within 5s" );
        UT_CHECK_MSG( WIFEXITED( status ) && WEXITSTATUS( status ) == 0,
                      "lens exited abnormally after C-x C-c (status "
                          << status << ")" );
    } );

    return registry.run( "lens on a real pseudo-terminal" ) == 0 ? 0 : 1;
}
