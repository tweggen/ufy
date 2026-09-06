#if !defined( _LENS_MODREG_KEYMAP_HPP )
#define _LENS_MODREG_KEYMAP_HPP

/**
 * @file keymap.hpp
 *
 * Keys, key sequences, and the binding table -- UI.md section 6.
 *
 * Keymaps bind COMMAND IDS, never functions, and are loaded from config, so
 * rebinding needs no rebuild. That is also what lets the generated help page
 * be correct by construction: it reads the same table the input loop reads,
 * so it cannot describe a binding that is not in effect.
 *
 * `Key` lives here, in `modreg/`, rather than in `term/` with the terminal
 * library, and that is deliberate. If the model's key type were FTXUI's,
 * every panel would transitively include FTXUI and gate G1.5's containment
 * grep would be enforcing nothing. `term/` decodes whatever the terminal
 * gives it INTO this type; nothing above `term/` knows how that happened.
 */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lens {

struct Key {
    enum class Code {
        Char,
        Tab, Enter, Escape, Backspace, Delete, Insert,
        Up, Down, Left, Right, Home, End, PageUp, PageDown,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12
    };

    Code     code = Code::Char;
    char32_t ch = 0;        //!< meaningful when code == Char
    bool     ctrl = false;
    bool     alt = false;
    bool     shift = false; //!< only for named keys; a shifted letter is its
                            //!< own character, not 'a' plus a flag

    Key() = default;
    static Key character( char32_t c, bool withCtrl = false, bool withAlt = false );
    static Key named( Code c, bool withCtrl = false, bool withAlt = false,
                      bool withShift = false );

    bool operator == ( const Key& o ) const;
    bool operator < ( const Key& o ) const;

    /**
     * Emacs-style rendering: `C-x`, `M-x`, `F1`, `S-Tab`, `a`.
     *
     * The same spelling `parse()` accepts, and the same spelling the config
     * file uses, so a binding a user reads in help is a binding they can
     * type back into `lens.toml`.
     */
    std::string toString() const;

    /** Parse one key. Returns false on anything unrecognised. */
    static bool parse( const std::string& text, Key& out_key );
};

/** A chord: `C-x 2` is two keys. Most bindings are one. */
using KeySeq = std::vector<Key>;

std::string toString( const KeySeq& seq );
bool parseKeySeq( const std::string& text, KeySeq& out_seq );

class Keymap {
public:
    /**
     * Bind a sequence to a command id.
     *
     * @return an empty string, or why it was rejected. A sequence that is a
     *     strict PREFIX of an existing binding is refused, and so is one
     *     that would extend an existing binding: `C-x` and `C-x 2` cannot
     *     both be bound, because the input loop would have to guess whether
     *     to fire the first or wait for the second, and whichever it chose
     *     would be wrong half the time.
     */
    std::string bind( const KeySeq& seq, const std::string& commandId );

    /** The command bound to a sequence, or an empty string. */
    std::string lookup( const KeySeq& seq ) const;

    /** True if `seq` is a proper prefix of some binding -- keep reading. */
    bool isPrefix( const KeySeq& seq ) const;

    /** The first sequence bound to `commandId`, or an empty sequence. */
    KeySeq bindingFor( const std::string& commandId ) const;

    const std::map<KeySeq, std::string>& bindings() const { return m_bindings; }

    std::size_t size() const { return m_bindings.size(); }

private:
    std::map<KeySeq, std::string> m_bindings;
};

/** The stock bindings of UI.md section 2 and its hint line. */
Keymap defaultKeymap();

} // namespace lens

#endif // _LENS_MODREG_KEYMAP_HPP
