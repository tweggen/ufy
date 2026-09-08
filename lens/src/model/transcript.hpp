#if !defined( _LENS_MODEL_TRANSCRIPT_HPP )
#define _LENS_MODEL_TRANSCRIPT_HPP

/**
 * @file transcript.hpp
 *
 * The Transcript panel -- UI.md section 3, gate G2. "The REPL, better."
 *
 * It holds what happened, in order: what was typed, what came back, what the
 * program printed, and what went wrong. Past input is re-runnable in place
 * (Oberon: text is executable), which is why an entry keeps the text that
 * produced it rather than only its rendering.
 *
 * This file sees `vault-unify-session.hpp` and nothing else of the engine.
 * That header is pure standard C++ -- gate G0.1 proves it -- so including it
 * costs no link dependency, and the transcript can be tested with no engine
 * present at all. That is the whole reason the boundary was drawn.
 */

#include "vault-unify-session.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lens {

namespace us = vault::unify::session;

/** One line of the transcript. */
struct TranscriptEntry {
    enum class Kind {
        Input,        //!< a goal or definition the user submitted
        Solution,     //!< one solution's bindings
        Status,       //!< "3 solutions", "no solutions", "aborted"
        Output,       //!< print/emit, via an Output event -- engine item E4
        Diagnostic,   //!< a parse or runtime error -- engine item E10
        Info          //!< lens talking about itself
    };

    Kind        kind = Kind::Info;
    std::string text;

    /**
     * The text that produced this entry, for `F5` re-run.
     *
     * Only Input entries carry it. Kept rather than reconstructed from
     * `text`, because the rendering adds a prompt and may elide -- and
     * re-running an approximation of what the user typed is worse than not
     * offering to re-run at all.
     */
    std::string source;

    /** The query this belongs to, so a cancelled query's rows can be found. */
    us::QueryId query = us::kNoQuery;

    /** Diagnostic detail, for the file:line:column rendering of G2.2. */
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string sourceLine;
};

/** The panel's own state. */
struct TranscriptState {
    std::vector<TranscriptEntry> entries;

    /** The line being typed. */
    std::string input;

    /**
     * Where the cursor sits in `input`, counted in BYTES.
     *
     * Bytes rather than columns because every edit is a byte operation and
     * only the rendering cares about columns; converting at the edge is one
     * conversion, converting everywhere is a class of off-by-one bug.
     */
    std::size_t cursor = 0;

    /**
     * Previously submitted lines, oldest first -- the same list
     * `$HOME/.unify_history` holds, so lens and `unify-run -i` can read each
     * other's (G2.8).
     */
    std::vector<std::string> history;

    /** Where Up/Down currently are in `history`; == size() means "not browsing". */
    std::size_t historyIndex = 0;

    /** The query whose solutions are still arriving, or kNoQuery. */
    us::QueryId liveQuery = us::kNoQuery;

    /** Scroll offset from the bottom; 0 means pinned to the newest line. */
    int scrollBack = 0;

    /** Solutions delivered for the live query, for the status line. */
    std::uint64_t produced = 0;

    /** True once the live query reached a terminal status. */
    bool finished = true;
};

/** The prompt, matching `unify-run`'s so nobody has to unlearn it. */
extern const char* const kTranscriptPrompt;

/**
 * Render one `Value` for a transcript line.
 *
 * Deliberately duplicated rather than shared with the engine's
 * `toDisplayString()`: sharing it would make `model/` link the engine, and
 * the model being link-free from the engine is what lets the whole UI be
 * tested without one. It is twenty lines, and the two will not drift because
 * a golden screen pins this one.
 */
std::string renderValue( const us::Value& value );

/** `$x = 1, $y = hello` for one solution's bindings. */
std::string renderBindings(
    const std::vector<std::pair<std::string, us::Value>>& bindings );

/**
 * `file:line:column: message` plus the offending source line and a caret,
 * matching what `unify-run` already prints (G2.2).
 *
 * Returns one entry per line so the transcript can hold them separately and
 * a later gate can make the first of them navigable.
 */
std::vector<TranscriptEntry> renderDiagnostic( const us::Diagnostic& diagnostic );

} // namespace lens

#endif // _LENS_MODEL_TRANSCRIPT_HPP
