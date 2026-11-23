/**
 * @file ftxui-terminal.cpp
 *
 * The one file in lens that knows FTXUI exists -- ARCHITECTURE.md section
 * 6.1, gate G1.5.
 *
 * FTXUI is used for three things and deliberately not for a fourth: a screen
 * buffer, an input decoder, and a resize signal. Its layout system is NOT
 * used -- lens owns tiling (src/layout/), and the solver has already decided
 * where everything goes by the time a frame reaches here. So the whole of
 * the rendering below is "copy a CellGrid into a Screen", which is roughly a
 * tenth of FTXUI's surface and the tenth least likely to change under us.
 *
 * WHAT IS VERIFIED HERE. The grid this renders is tested exhaustively on
 * the other side of the seam, and everything a golden can prove is proved
 * without a terminal through `--script` (gate G1.7).
 *
 * This file used to end its comment with "and what is left is this file,
 * kept as small as it is so that 'we could not test it' covers as little as
 * possible." Two user-visible bugs then turned up inside exactly that
 * remainder -- a frame that was computed and never presented, and a Ctrl-C
 * that killed the process instead of completing a chord -- so the remainder
 * is no longer untested: test/pty-interaction-test.cpp runs the real binary
 * on a real pseudo-terminal and reads the bytes that come back. What is
 * still unverified is narrower and named: resize signals, and the alternate
 * screen being restored on an abnormal exit.
 */

#include "iterminal.hpp"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/dom/requirement.hpp>
#include <ftxui/screen/pixel.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <thread>

#if defined( _WIN32 )
#  include <io.h>
#  define LENS_ISATTY _isatty
#  define LENS_FILENO _fileno
#else
#  include <unistd.h>
#  define LENS_ISATTY isatty
#  define LENS_FILENO fileno
#endif

namespace lens {

namespace {

/**
 * Resolve a colour role for the tier the terminal actually has.
 *
 * The monochrome tier is not a degraded afterthought: ARCHITECTURE.md
 * section 6.2 requires every panel to be legible and every selection visible
 * with attributes alone, so a role that only ever produced a colour would be
 * a bug in the design rather than in the terminal.
 */
void applyAttr( ftxui::Pixel& pixel, const Attr& attr,
                TerminalCapabilities::ColourTier tier )
{
    pixel.bold = attr.bold;
    pixel.inverted = attr.reverse;
    pixel.underlined = attr.underline;

    if( tier == TerminalCapabilities::ColourTier::Monochrome ) {
        /* Attributes only. Roles that mean "stands out" already set bold or
         * reverse above, so nothing is lost beyond hue. */
        return;
    }

    switch( attr.colour ) {
    case Colour::Default:                                            break;
    case Colour::Dim:       pixel.foreground_color = ftxui::Color::GrayDark;  break;
    case Colour::Accent:    pixel.foreground_color = ftxui::Color::Cyan;      break;
    case Colour::Warning:   pixel.foreground_color = ftxui::Color::Yellow;    break;
    case Colour::Error:     pixel.foreground_color = ftxui::Color::Red;       break;
    case Colour::Selection: pixel.inverted = true;                            break;
    }
}


/**
 * A dom node that renders one CellGrid and occupies exactly its size.
 *
 * Bypasses FTXUI's layout entirely -- which is the point. The grid's
 * geometry was decided by lens's own solver, and letting a second layout
 * engine have an opinion about it is how tiles end up one column off.
 */
class GridNode : public ftxui::Node {
public:
    GridNode( const CellGrid* grid, TerminalCapabilities::ColourTier tier )
        : m_grid( grid ), m_tier( tier ) {}

    void ComputeRequirement() override
    {
        requirement_.min_x = m_grid ? m_grid->width() : 0;
        requirement_.min_y = m_grid ? m_grid->height() : 0;
    }

    void Render( ftxui::Screen& screen ) override
    {
        if( !m_grid ) {
            return;
        }

        const int width = std::min( m_grid->width(), screen.dimx() );
        const int height = std::min( m_grid->height(), screen.dimy() );

        for( int y = 0; y < height; ++y ) {
            for( int x = 0; x < width; ++x ) {
                const Cell& cell = m_grid->at( x, y );

                if( 0 == cell.ch ) {
                    /*
                     * The continuation column of a wide character. FTXUI
                     * treats a pixel holding a wide glyph as covering two
                     * columns itself, so this one is left empty rather than
                     * blanked -- writing a space here would overwrite the
                     * right half of the glyph next to it.
                     */
                    continue;
                }

                ftxui::Pixel& pixel = screen.PixelAt( x, y );
                pixel.character = encodeUtf8( cell.ch );
                applyAttr( pixel, cell.attr, m_tier );
            }
        }
    }

private:
    const CellGrid* m_grid;
    TerminalCapabilities::ColourTier m_tier;
};


/** Translate one FTXUI event into lens's own key vocabulary. */
bool translateKey( const ftxui::Event& event, Key& out_key )
{
    struct Named { const ftxui::Event* event; Key::Code code; bool shift; };
    static const Named kNamed[] = {
        { &ftxui::Event::ArrowUp,     Key::Code::Up,        false },
        { &ftxui::Event::ArrowDown,   Key::Code::Down,      false },
        { &ftxui::Event::ArrowLeft,   Key::Code::Left,      false },
        { &ftxui::Event::ArrowRight,  Key::Code::Right,     false },
        { &ftxui::Event::Backspace,   Key::Code::Backspace, false },
        { &ftxui::Event::Delete,      Key::Code::Delete,    false },
        { &ftxui::Event::Insert,      Key::Code::Insert,    false },
        { &ftxui::Event::Return,      Key::Code::Enter,     false },
        { &ftxui::Event::Escape,      Key::Code::Escape,    false },
        { &ftxui::Event::Tab,         Key::Code::Tab,       false },
        { &ftxui::Event::TabReverse,  Key::Code::Tab,       true  },
        { &ftxui::Event::Home,        Key::Code::Home,      false },
        { &ftxui::Event::End,         Key::Code::End,       false },
        { &ftxui::Event::PageUp,      Key::Code::PageUp,    false },
        { &ftxui::Event::PageDown,    Key::Code::PageDown,  false },
        { &ftxui::Event::F1,  Key::Code::F1,  false },
        { &ftxui::Event::F2,  Key::Code::F2,  false },
        { &ftxui::Event::F3,  Key::Code::F3,  false },
        { &ftxui::Event::F4,  Key::Code::F4,  false },
        { &ftxui::Event::F5,  Key::Code::F5,  false },
        { &ftxui::Event::F6,  Key::Code::F6,  false },
        { &ftxui::Event::F7,  Key::Code::F7,  false },
        { &ftxui::Event::F8,  Key::Code::F8,  false },
        { &ftxui::Event::F9,  Key::Code::F9,  false },
        { &ftxui::Event::F10, Key::Code::F10, false },
        { &ftxui::Event::F11, Key::Code::F11, false },
        { &ftxui::Event::F12, Key::Code::F12, false },
    };

    /*
     * Named keys first. Tab and Return also arrive as control characters
     * (0x09, 0x0D), so decoding the raw bytes first would turn Tab into
     * C-i -- correct as terminal history and wrong as a binding.
     */
    for( const Named& named : kNamed ) {
        if( event == *named.event ) {
            out_key = Key::named( named.code, false, false, named.shift );
            return true;
        }
    }

    const std::string& input = event.input();
    if( input.empty() ) {
        return false;
    }

    /*
     * Alt is an ESC prefix on every terminal that matters. Checked before
     * the bare-ESC case above only in the sense that Event::Escape has
     * already been matched: a lone 0x1B is Escape, 0x1B plus more is Alt.
     */
    if( input.size() >= 2 && (unsigned char) input[0] == 0x1B ) {
        const std::vector<char32_t> rest = decodeUtf8( input.substr( 1 ) );
        if( rest.size() == 1 ) {
            out_key = Key::character( rest[0], false, true );
            return true;
        }
        return false;
    }

    /* Control characters: 0x01..0x1A are C-a .. C-z. */
    if( input.size() == 1 ) {
        const unsigned char byte = (unsigned char) input[0];
        if( byte >= 0x01 && byte <= 0x1A ) {
            out_key = Key::character( (char32_t) ( byte + 0x60 ), true, false );
            return true;
        }
    }

    if( event.is_character() ) {
        const std::vector<char32_t> codepoints = decodeUtf8( event.character() );
        if( codepoints.size() == 1 ) {
            out_key = Key::character( codepoints[0] );
            return true;
        }
    }

    return false;
}


class FtxuiTerminal : public ITerminal {
public:
    FtxuiTerminal()
        : m_app( ftxui::App::Fullscreen() )
    {
        m_caps.width = m_app.dimx();
        m_caps.height = m_app.dimy();
        m_caps.unicode = true;

        /*
         * C-c and C-z belong to lens, not to FTXUI.
         *
         * FTXUI raises SIGINT on Ctrl-C whatever the component returns
         * (force_handle_ctrl_c_ defaults to true), which is the right
         * default for a widget in someone else's program and the wrong one
         * here: `C-x C-c` is the documented way to quit, and its second
         * key would kill the process by signal instead -- skipping the
         * quit path, and any confirmation a later gate puts in front of it.
         * The same argument applies to C-z: a tiling editor decides what
         * suspend means.
         */
        m_app.ForceHandleCtrlC( false );
        m_app.ForceHandleCtrlZ( false );

        /*
         * Tier detection from the environment, which is all a terminal
         * offers: COLORTERM is the only widely honoured signal for
         * truecolour, and TERM carrying "256color" for the middle tier.
         * Anything else is treated as monochrome -- deliberately the
         * pessimistic default, since a wrong guess upward produces
         * unreadable output and a wrong guess downward produces plain
         * output that still works.
         */
        const char* colorterm = std::getenv( "COLORTERM" );
        const char* term = std::getenv( "TERM" );
        const std::string colortermValue = colorterm ? colorterm : "";
        const std::string termValue = term ? term : "";

        if( colortermValue == "truecolor" || colortermValue == "24bit" ) {
            m_caps.colour = TerminalCapabilities::ColourTier::TrueColour;
        } else if( termValue.find( "256color" ) != std::string::npos ) {
            m_caps.colour = TerminalCapabilities::ColourTier::Ansi256;
        } else {
            m_caps.colour = TerminalCapabilities::ColourTier::Monochrome;
        }

        m_component = ftxui::CatchEvent(
            ftxui::Renderer( [ this ] {
                return std::make_shared<GridNode>( &m_grid, m_caps.colour );
            } ),
            [ this ]( ftxui::Event event ) {
                Key key;
                if( translateKey( event, key ) ) {
                    TerminalEvent te;
                    te.kind = TerminalEvent::Kind::Key;
                    te.key = key;
                    m_pending.push_back( te );
                }
                /* Consumed either way: no FTXUI component owns the input. */
                return true;
            } );

        m_loop = std::make_unique<ftxui::Loop>( &m_app, m_component );
    }

    TerminalCapabilities capabilities() const override { return m_caps; }

    /**
     * Present a frame -- and make FTXUI actually do it.
     *
     * The two lines this replaced were a one-keystroke lag on every screen
     * lens has. FTXUI's RunOnce() renders only if one of ITS tasks ran
     * (App::Internal::RunOnce returns early otherwise) and Draw() is a
     * no-op while its frame_valid_ flag stands, which only an event
     * clears. lens changes the grid out of band -- FTXUI's own component
     * tree never changes -- so after a key was consumed in poll(), the
     * frame computed from it found nothing to invalidate and was dropped.
     * It reached the screen on the NEXT keystroke, one behind forever.
     *
     * Posting a Custom event says the thing that is true: something
     * changed, this frame is stale. The event reaches the CatchEvent
     * handler below, translates to no key, and is swallowed.
     *
     * The equality check matters as much as the post. The UI loop calls
     * draw() every time poll() times out -- twenty times a second with
     * nobody touching the keyboard -- and without it every one of those
     * would repaint the whole screen.
     */
    void draw( const CellGrid& grid ) override
    {
        if( m_drawn && grid == m_grid ) {
            return;
        }

        m_grid = grid;
        m_drawn = true;

        m_app.PostEvent( ftxui::Event::Custom );
        m_loop->RunOnce();
    }

    TerminalEvent poll( int timeoutMs ) override
    {
        using clock = std::chrono::steady_clock;
        const auto deadline =
            clock::now() + std::chrono::milliseconds( timeoutMs < 0 ? 0 : timeoutMs );

        for( ;; ) {
            if( !m_pending.empty() ) {
                const TerminalEvent event = m_pending.front();
                m_pending.pop_front();
                return event;
            }

            if( m_loop->HasQuitted() ) {
                TerminalEvent event;
                event.kind = TerminalEvent::Kind::Closed;
                return event;
            }

            m_loop->RunOnce();

            /*
             * FTXUI resizes the App itself; lens learns about it by noticing
             * the dimensions moved. Synthesising the event here rather than
             * hooking a signal keeps the platform-specific part of resize
             * inside FTXUI, which is the whole reason for using it.
             */
            if( m_app.dimx() != m_caps.width || m_app.dimy() != m_caps.height ) {
                m_caps.width = m_app.dimx();
                m_caps.height = m_app.dimy();

                TerminalEvent event;
                event.kind = TerminalEvent::Kind::Resize;
                event.width = m_caps.width;
                event.height = m_caps.height;
                return event;
            }

            if( clock::now() >= deadline ) {
                return TerminalEvent();   /* Kind::None */
            }

            /*
             * A short sleep rather than RunOnceBlocking(): the UI thread has
             * to wake for session events too, and a blocking read on the
             * terminal would make an arriving solution wait for a keystroke.
             */
            std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
        }
    }

private:
    ftxui::App        m_app;
    ftxui::Component  m_component;
    std::unique_ptr<ftxui::Loop> m_loop;

    CellGrid m_grid;
    bool     m_drawn = false;   //!< has m_grid ever been presented?
    TerminalCapabilities m_caps;
    std::deque<TerminalEvent> m_pending;
};

} // namespace

std::unique_ptr<ITerminal> makeTerminal( std::string& out_error )
{
    /*
     * ARCHITECTURE.md section 6.3: under Git Bash (mintty) a native console
     * application gets no real console handle, and the required behaviour is
     * one line naming the problem and the workaround -- not a broken screen.
     * On a POSIX host the equivalent condition is "stdin is not a terminal",
     * which is also what happens when someone pipes into lens by mistake.
     */
    if( !LENS_ISATTY( LENS_FILENO( stdin ) ) || !LENS_ISATTY( LENS_FILENO( stdout ) ) ) {
        out_error =
            "unify-lens: standard input or output is not a terminal.\n"
            "  If this is Git Bash (mintty), run `winpty unify-lens` -- mintty "
            "is a pty front end,\n"
            "  not a Win32 console, so a native console application gets no "
            "console handle there.\n"
            "  To run without a screen at all, use --script FILE, or use "
            "`unify-run -i` for the plain REPL.";
        return NULL;
    }

    out_error.clear();
    return std::unique_ptr<ITerminal>( new FtxuiTerminal() );
}

} // namespace lens
