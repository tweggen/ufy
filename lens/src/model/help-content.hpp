#if !defined( _LENS_MODEL_HELP_CONTENT_HPP )
#define _LENS_MODEL_HELP_CONTENT_HPP

/**
 * @file help-content.hpp
 *
 * The help hypertext -- UI.md section 5.3.
 *
 * Content comes from three sources, merged. Two of them exist here:
 *
 *   Built-in topics   compiled-in pages: getting started, the tiling model,
 *                     each panel. Golden-screen tests render them.
 *   Command table     the keymap page, generated at run time from the live
 *                     table, so it cannot disagree with the bindings.
 *
 * The third -- builtin and keyword topics extracted at build time from
 * unify/LANGUAGE.md and SPEC.md -- arrives with the panels that need it. The
 * important property of that source is that lens will EXTRACT from those
 * documents rather than restate them, so `F1` on `findall` shows what the
 * spec says and cannot fall behind it. A hand-copied help text is wrong
 * within a month.
 *
 * Topics link to each other with `[[topic-id]]`. That is the whole markup:
 * a help system with a syntax to learn is a help system nobody reads.
 */

#include "../modreg/command.hpp"
#include "../modreg/keymap.hpp"

#include <string>
#include <vector>

namespace lens {

struct HelpTopic {
    std::string id;
    std::string title;

    /** Body text, one entry per line. `[[id]]` marks a link. */
    std::vector<std::string> lines;
};

/**
 * The merged book.
 *
 * Built once per model rather than looked up globally, because the generated
 * pages depend on the live command table and keymap -- and a help system
 * that read a stale copy of those would be exactly the failure UI.md's
 * "generated at runtime, so it cannot disagree" is guarding against.
 */
class HelpBook {
public:
    HelpBook( const CommandTable& commands, const Keymap& keymap );

    /** A topic by id, or null. */
    const HelpTopic* topic( const std::string& id ) const;

    const std::vector<HelpTopic>& topics() const { return m_topics; }

    /** The id every "I pressed F1 and nothing has a better answer" lands on. */
    static std::string defaultTopicId() { return "getting-started"; }

    /**
     * The page lens opens on a fresh start.
     *
     * Keystrokes first, prose second. UI.md section 5.5 asked for four lines
     * in the transcript instead, on the grounds that a splash screen is an
     * imposition -- but the first person to actually run this could not tell
     * how to open a menu, close a window or find a tutorial, which is the
     * evidence that four lines were not enough. Recorded as a deliberate
     * departure rather than an oversight; it is a tile like any other and
     * C-x 0 dismisses it.
     */
    static std::string welcomeTopicId() { return "first-steps"; }

    /**
     * Every `[[link]]` on one line, in order.
     *
     * Exposed so the panel can follow one and the test can assert that every
     * link in every topic resolves -- a help system with dead links teaches
     * users to stop pressing F1.
     */
    static std::vector<std::string> linksOn( const std::string& line );

    /** `[[foo]]` rendered for display: the brackets go, the word stays. */
    static std::string stripLinkMarkup( const std::string& line );

private:
    void addBuiltinTopics();
    void addGeneratedTopics( const CommandTable& commands,
                             const Keymap& keymap );

    std::vector<HelpTopic> m_topics;
};

} // namespace lens

#endif // _LENS_MODEL_HELP_CONTENT_HPP
