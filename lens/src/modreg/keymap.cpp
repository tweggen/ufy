/**
 * @file keymap.cpp
 */

#include "keymap.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace lens {

namespace {

struct NamedKey {
    const char*  text;
    Key::Code    code;
};

/* Order matters only for output: the first match wins when formatting. */
const NamedKey kNamedKeys[] = {
    { "Tab",       Key::Code::Tab },
    { "Enter",     Key::Code::Enter },
    { "Esc",       Key::Code::Escape },
    { "Backspace", Key::Code::Backspace },
    { "Delete",    Key::Code::Delete },
    { "Insert",    Key::Code::Insert },
    { "Up",        Key::Code::Up },
    { "Down",      Key::Code::Down },
    { "Left",      Key::Code::Left },
    { "Right",     Key::Code::Right },
    { "Home",      Key::Code::Home },
    { "End",       Key::Code::End },
    { "PageUp",    Key::Code::PageUp },
    { "PageDown",  Key::Code::PageDown },
    { "F1",  Key::Code::F1 },  { "F2",  Key::Code::F2 },
    { "F3",  Key::Code::F3 },  { "F4",  Key::Code::F4 },
    { "F5",  Key::Code::F5 },  { "F6",  Key::Code::F6 },
    { "F7",  Key::Code::F7 },  { "F8",  Key::Code::F8 },
    { "F9",  Key::Code::F9 },  { "F10", Key::Code::F10 },
    { "F11", Key::Code::F11 }, { "F12", Key::Code::F12 },
};

const std::size_t kNamedKeyCount = sizeof( kNamedKeys ) / sizeof( kNamedKeys[0] );

} // namespace

Key Key::character( char32_t c, bool withCtrl, bool withAlt )
{
    Key k;
    k.code = Code::Char;
    k.ch = c;
    k.ctrl = withCtrl;
    k.alt = withAlt;
    return k;
}


Key Key::named( Code c, bool withCtrl, bool withAlt, bool withShift )
{
    Key k;
    k.code = c;
    k.ctrl = withCtrl;
    k.alt = withAlt;
    k.shift = withShift;
    return k;
}


bool Key::operator == ( const Key& o ) const
{
    return code == o.code && ch == o.ch
        && ctrl == o.ctrl && alt == o.alt && shift == o.shift;
}


bool Key::operator < ( const Key& o ) const
{
    if( code != o.code ) { return code < o.code; }
    if( ch != o.ch ) { return ch < o.ch; }
    if( ctrl != o.ctrl ) { return ctrl < o.ctrl; }
    if( alt != o.alt ) { return alt < o.alt; }
    return shift < o.shift;
}


std::string Key::toString() const
{
    std::string out;
    if( ctrl )  { out += "C-"; }
    if( alt )   { out += "M-"; }
    if( shift ) { out += "S-"; }

    if( code == Code::Char ) {
        if( ch < 0x80 ) {
            out += (char) ch;
        } else {
            /* Non-ASCII binding: spell it as U+XXXX so the config round-trips. */
            char buffer[16];
            std::snprintf( buffer, sizeof( buffer ), "U+%04X", (unsigned) ch );
            out += buffer;
        }
        return out;
    }

    for( std::size_t i = 0; i < kNamedKeyCount; ++i ) {
        if( kNamedKeys[i].code == code ) {
            out += kNamedKeys[i].text;
            return out;
        }
    }
    out += "?";
    return out;
}


bool Key::parse( const std::string& text, Key& out_key )
{
    Key key;
    std::string rest = text;

    for( ;; ) {
        if( rest.size() > 2 && rest.compare( 0, 2, "C-" ) == 0 ) {
            key.ctrl = true; rest = rest.substr( 2 ); continue;
        }
        if( rest.size() > 2 && rest.compare( 0, 2, "M-" ) == 0 ) {
            key.alt = true; rest = rest.substr( 2 ); continue;
        }
        if( rest.size() > 2 && rest.compare( 0, 2, "S-" ) == 0 ) {
            key.shift = true; rest = rest.substr( 2 ); continue;
        }
        break;
    }

    if( rest.empty() ) {
        return false;
    }

    for( std::size_t i = 0; i < kNamedKeyCount; ++i ) {
        if( rest == kNamedKeys[i].text ) {
            key.code = kNamedKeys[i].code;
            out_key = key;
            return true;
        }
    }

    if( rest.size() > 2 && rest.compare( 0, 2, "U+" ) == 0 ) {
        key.code = Key::Code::Char;
        key.ch = (char32_t) std::strtoul( rest.c_str() + 2, NULL, 16 );
        out_key = key;
        return true;
    }

    if( rest.size() == 1 ) {
        key.code = Key::Code::Char;
        key.ch = (char32_t) (unsigned char) rest[0];
        out_key = key;
        return true;
    }

    return false;
}


std::string toString( const KeySeq& seq )
{
    std::string out;
    for( std::size_t i = 0; i < seq.size(); ++i ) {
        if( i ) { out += " "; }
        out += seq[i].toString();
    }
    return out;
}


bool parseKeySeq( const std::string& text, KeySeq& out_seq )
{
    KeySeq seq;
    std::istringstream is( text );
    std::string token;
    while( is >> token ) {
        Key key;
        if( !Key::parse( token, key ) ) {
            return false;
        }
        seq.push_back( key );
    }
    if( seq.empty() ) {
        return false;
    }
    out_seq = seq;
    return true;
}


std::string Keymap::bind( const KeySeq& seq, const std::string& commandId )
{
    if( seq.empty() ) {
        return "an empty key sequence cannot be bound";
    }
    if( commandId.empty() ) {
        return "a binding needs a command id";
    }

    /*
     * Reject ambiguity in both directions. If `C-x` were bound alongside
     * `C-x 2`, the input loop would have to decide, on seeing C-x, whether
     * to fire the command or wait for a second key -- and either choice is
     * wrong half the time. Refusing at bind time turns a subtle input bug
     * into a startup message naming both bindings.
     */
    for( std::map<KeySeq, std::string>::const_iterator it = m_bindings.begin();
         it != m_bindings.end(); ++it ) {
        const KeySeq& existing = it->first;
        const std::size_t shorter = std::min( existing.size(), seq.size() );
        if( existing.size() == seq.size() ) {
            continue;   /* an exact duplicate is handled below */
        }
        bool isPrefixOfOther = true;
        for( std::size_t i = 0; i < shorter; ++i ) {
            if( !( existing[i] == seq[i] ) ) { isPrefixOfOther = false; break; }
        }
        if( isPrefixOfOther ) {
            return "key sequence '" + toString( seq ) + "' conflicts with '"
                 + toString( existing ) + "' (one is a prefix of the other)";
        }
    }

    const std::map<KeySeq, std::string>::const_iterator existing =
        m_bindings.find( seq );
    if( existing != m_bindings.end() ) {
        return "key sequence '" + toString( seq ) + "' is already bound to '"
             + existing->second + "'";
    }

    m_bindings[ seq ] = commandId;
    return std::string();
}


std::string Keymap::lookup( const KeySeq& seq ) const
{
    const std::map<KeySeq, std::string>::const_iterator it =
        m_bindings.find( seq );
    return it == m_bindings.end() ? std::string() : it->second;
}


bool Keymap::isPrefix( const KeySeq& seq ) const
{
    if( seq.empty() ) {
        return !m_bindings.empty();
    }
    for( std::map<KeySeq, std::string>::const_iterator it = m_bindings.begin();
         it != m_bindings.end(); ++it ) {
        if( it->first.size() <= seq.size() ) {
            continue;
        }
        bool matches = true;
        for( std::size_t i = 0; i < seq.size(); ++i ) {
            if( !( it->first[i] == seq[i] ) ) { matches = false; break; }
        }
        if( matches ) {
            return true;
        }
    }
    return false;
}


KeySeq Keymap::bindingFor( const std::string& commandId ) const
{
    for( std::map<KeySeq, std::string>::const_iterator it = m_bindings.begin();
         it != m_bindings.end(); ++it ) {
        if( it->second == commandId ) {
            return it->first;
        }
    }
    return KeySeq();
}


Keymap defaultKeymap()
{
    Keymap map;

    struct Binding { const char* keys; const char* command; };

    /* UI.md sections 2 and 4. Emacs bindings for tiling, Borland for help. */
    static const Binding kDefaults[] = {
        { "C-x 2",  "window.split-rows" },
        { "C-x 3",  "window.split-columns" },
        { "C-x 0",  "window.close" },
        { "C-x 1",  "window.maximise" },
        { "Tab",    "window.focus-next" },
        { "S-Tab",  "window.focus-prev" },
        { "F1",     "help.contextual" },
        { "F10",    "menu.open" },
        { "M-x",    "command.palette" },
        { "C-g",    "app.abort" },
        { "C-x C-c","app.quit" },
    };

    for( std::size_t i = 0; i < sizeof( kDefaults ) / sizeof( kDefaults[0] ); ++i ) {
        KeySeq seq;
        if( parseKeySeq( kDefaults[i].keys, seq ) ) {
            map.bind( seq, kDefaults[i].command );
        }
    }
    return map;
}

} // namespace lens
