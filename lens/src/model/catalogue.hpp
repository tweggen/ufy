#if !defined( _LENS_MODEL_CATALOGUE_HPP )
#define _LENS_MODEL_CATALOGUE_HPP

/**
 * @file catalogue.hpp
 *
 * The browser's left half -- gate G3, UI.md section 3:
 *
 *   Catalogue   module -> (name, arity) -> clause count, with builtins and
 *               synthesized clauses collapsed into counted stubs.
 *               Filter-as-you-type.
 *
 * and the definition half of the Source panel it drives.
 *
 * Grouping is by PROVENANCE (engine item E1), never by name. The engine
 * used to decide "not user code" from a `__` prefix, and a user predicate
 * called `__cache` vanished from `:list` for it; a catalogue that copied
 * that rule would copy the bug. The listing's Origin is the fact, so the
 * group is read off it.
 */

#include "vault-unify-session.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace lens {

namespace us = vault::unify::session;

/** The catalogue as last listed, and the panel's own state over it. */
struct CatalogueState {
    std::vector<us::CatalogueEntry> entries;

    /**
     * Derived from `entries` once per listing by indexCatalogue(): the
     * display order, each entry's group, and its name in lower case for the
     * filter. Kept because catalogueRows() runs several times per keystroke
     * -- fold, cursor-following and the renderer each ask -- and re-sorting
     * five thousand keys every time is what put a keystroke over G3.5's
     * frame budget.
     */
    std::vector<int>         order;
    std::vector<std::string> groups;
    std::vector<std::string> lowerNames;

    /** The listing being assembled from pages; swapped in when complete. */
    std::vector<us::CatalogueEntry> incoming;

    /** A listing has been asked for and its last page has not arrived. */
    bool listingPending = false;

    /**
     * The world changed while a listing was in flight, so that listing may
     * predate the change. One more is asked for when it lands -- however
     * many changes there were, which is what keeps a burst of definitions
     * from becoming a burst of listings (G3.4).
     */
    bool dirty = false;

    /** At least one complete listing has arrived. */
    bool loaded = false;

    /** Filter-as-you-type (G3.5): a case-insensitive substring of the name. */
    std::string filter;

    /**
     * Groups whose open/closed state the user changed. Stored as the
     * DIFFERENCE from the default (stubs closed, modules open) so a group
     * that appears later starts the way groups of its kind start.
     */
    std::set<std::string> toggled;

    int selected = 0;
    int top = 0;
};

/** One line of the Catalogue panel. */
struct CatalogueRow {
    enum class Kind { Group, Predicate };

    Kind          kind = Kind::Predicate;
    std::string   group;       //!< the group this row is, or belongs to
    std::string   label;       //!< group name, or `name/arity`
    std::uint32_t count = 0;   //!< predicates in a group; clauses for a key
    int           entry = -1;  //!< index into CatalogueState::entries
    bool          open = false;
};

/** The group an entry belongs to, read off its provenance. */
std::string catalogueGroupOf( const us::CatalogueEntry& entry );

/** Recompute the derived index after `entries` changed. */
void indexCatalogue( CatalogueState& catalogue );

/** Stubs -- builtins, synthesized -- start closed; everything else open. */
bool catalogueGroupOpensByDefault( const std::string& group );

/**
 * The rows the panel shows, in order. Grouped and collapsible with no
 * filter; with one, a flat list of every matching key, stubs included,
 * because a filter is how you find a builtin.
 *
 * One definition for fold, the scroll projection and the renderer.
 */
std::vector<CatalogueRow> catalogueRows( const CatalogueState& catalogue );

/** Rows of a Catalogue tile the list gets: one goes to the filter, if any. */
inline int catalogueListRows( int innerHeight, const CatalogueState& catalogue )
{
    const int reserved = catalogue.filter.empty() ? 0 : 1;
    return innerHeight > reserved ? innerHeight - reserved : 0;
}

/** What the Source panel shows of a definition the catalogue selected. */
struct SourceDefinition {
    bool             set = false;
    us::PredicateKey key;
    std::vector<std::string> lines;
    us::Origin       origin;
    std::uint32_t    clauses = 0;

    /** The key whose `source` is in flight; an answer for any other is stale. */
    bool             pending = false;
    us::PredicateKey pendingKey;

    int cursor = 0;          //!< the line being edited
    std::size_t column = 0;  //!< in BYTES, as TranscriptState::cursor is
    int top = 0;

    /** G3.3: the text differs from what the engine last sent. */
    bool dirty = false;

    /** F3 was pressed and the core has not answered the define yet. */
    bool redefinePending = false;

    /**
     * Which OverwritePolicy F3 uses: kPolicyAppend, kPolicyReplace or
     * kPolicyFail. Replace by default, because editing a definition and
     * saving it means "this is the definition now" -- appending would leave
     * the old clauses answering beside the new ones.
     */
    int policy = 1;

    /**
     * The policy selector has the keyboard rather than the text: Up from
     * the first line reaches it, Down leaves it. Its own focus, so that F1
     * on it can explain the policies (G3 H) rather than the editor.
     */
    bool selectorFocused = false;
};

/** The policy indices F3 sends, in OverwritePolicy's order. */
constexpr int kPolicyAppend = 0;
constexpr int kPolicyReplace = 1;
constexpr int kPolicyFail = 2;

/** `append`, `replace`, `fail`: the selector's words for them. */
const char* policyName( int policy );

/** Rows of a Source tile the definition's lines get: a header and the
 * policy selector take the first two. */
inline int sourceTextRows( int innerHeight )
{
    return innerHeight > 2 ? innerHeight - 2 : 0;
}

} // namespace lens

#endif // _LENS_MODEL_CATALOGUE_HPP
