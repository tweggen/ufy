/**
 * @file unify-repl.cpp
 *
 * @author Timo Weggen
 *
 * The interactive REPL behind `unify-run` with no program argument (or
 * with `-i`) -- ROADMAP.md Phase 4, "REPL (readline is already linked)
 * with query, assert, and inspection".
 *
 * WHAT THE PROMPT ACCEPTS
 *
 *   1. Anything a .ufy file accepts, verbatim: a fact (`color( red );`), a
 *      rule (`warm( $x ) { color( $x ); }`), a `query { ... }` block, an
 *      `import "other.ufy";`. Each one is handed to
 *      RuntimeContext::parseExecuteSegment() exactly the way a file's
 *      contents are, against the SAME RuntimeContext for the whole
 *      session -- so clauses accumulate in the World's root execution
 *      state and every later query sees them, just as if the session had
 *      been typed into one file top to bottom. "assert" from the ROADMAP
 *      item therefore needs nothing REPL-specific: `assert`/`retract` are
 *      ordinary builtins and work at the prompt like anywhere else.
 *
 *   2. A `?` shorthand for a query -- `? color( $x );` is exactly
 *      `query { color( $x ); }`, and is the only REPL-only syntax here.
 *      It is unambiguous because no top-level .ufy form may start with
 *      `?` (see PrologParser's m_ruleEvent: import, query or clause, all
 *      of which start with a keyword or a term).
 *
 *   3. REPL commands, which all start with `:` -- also unambiguous, for
 *      the same reason. See s_commandHelp below for the list.
 *
 * Input is accumulated line by line until it forms a complete top-level
 * item (see isFormComplete()), so multi-line rules and queries can be
 * typed the way they would be written in a file.
 *
 * WHAT A QUERY PRINTS
 *
 * `print`/`emit` already write to stdout as a side effect of the search,
 * exactly as in a batch run. On top of that -- and this is the one place
 * where the REPL genuinely shows more than `unify-run program.ufy` does --
 * every finished query reports its variable bindings, one line per
 * solution, from SolveJob::getSolutionList(), followed by a solution
 * count. That is what makes a query at the prompt useful without having
 * to litter it with `print(...)` calls.
 */

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#if defined( _WIN32 )
#  include <io.h>
#  define UNIFY_STDIN_IS_TTY() ( 0 != _isatty( _fileno( stdin ) ) )
#else
#  include <unistd.h>
#  define UNIFY_STDIN_IS_TTY() ( 0 != isatty( STDIN_FILENO ) )
#endif

#if defined( UNIFY_HAVE_READLINE )
#  include <readline/readline.h>
#  include <readline/history.h>
#endif

#include <vault-unify.hpp>

/*
 * Private engine headers (live in src/, not include/); reachable here the
 * same way the module's own .cpp files reach them -- see the identical
 * note in unify-run.cpp.
 *   - vault-unify-solvejob.hpp: SolveJob::getSolutionList()/getErrorCount(),
 *     i.e. everything the per-query report below is made of.
 *   - vault-unify-debug.hpp: vault-unify.hpp only forward-declares
 *     `class FileDebugInfo;`; the full definition is needed to construct
 *     the one standing in for the prompt itself (see s_pReplFileDebugInfo).
 */
#include <vault-unify-solvejob.hpp>
#include <vault-unify-debug.hpp>

#include "unify-repl.hpp"
#include "unify-tool-support.hpp"


namespace unifytool {


namespace {


/**
 * The file URI parse errors from typed-in text are attributed to. It is
 * not a real path, and deliberately looks like one of the pseudo-files
 * the engine already uses ("<input>", see reportParseError() in
 * vault-unify-runtime-context.cpp) rather than anything that could be
 * mistaken for a file on disk.
 */
const char* const REPL_FILE_URI = "<repl>";

const char* const PROMPT_MAIN = "ufy> ";
const char* const PROMPT_CONTINUE = "...> ";


/*
 * =========================================================================
 * Line input
 * =========================================================================
 *
 * Two implementations behind one readInputLine(): GNU readline when the
 * build found it (UNIFY_HAVE_READLINE, see CMakeLists.txt) AND stdin is a
 * terminal, plain std::getline otherwise. The isatty() half of that
 * condition matters as much as the compile-time half: with stdin
 * redirected -- `unify-run < script.ufy`, or a golden test piping input in
 * -- readline would still try to drive a terminal, so a piped session
 * always takes the std::getline path and prints no prompts and no banner
 * at all, which keeps its stdout diffable.
 */

bool s_isInteractive = false;

#if defined( UNIFY_HAVE_READLINE )
bool s_useReadline = false;

/**
 * Session history file, `$HOME/.unify_history`, loaded at startup and
 * rewritten at exit. Empty if $HOME is unset, which disables persistence
 * (in-memory history still works for the running session).
 */
std::string s_historyPath;
#endif


void setupLineInput( bool isInteractive )
{
    s_isInteractive = isInteractive;

#if defined( UNIFY_HAVE_READLINE )
    s_useReadline = isInteractive;
    if( !s_useReadline ) {
        return;
    }
    const char* pHome = getenv( "HOME" );
    if( pHome && *pHome ) {
        s_historyPath = std::string( pHome ) + "/.unify_history";
        // Missing file on a first-ever run is not an error: read_history()
        // just returns ENOENT and leaves the history empty.
        (void) read_history( s_historyPath.c_str() );
    }
    stifle_history( 1000 );
#endif
}


void shutdownLineInput()
{
#if defined( UNIFY_HAVE_READLINE )
    if( s_useReadline && !s_historyPath.empty() ) {
        (void) write_history( s_historyPath.c_str() );
    }
#endif
}


/**
 * Read one line (without its newline) into `out_line`. Returns false at
 * end of input -- Ctrl-D at a terminal, or EOF on a redirected stdin.
 */
bool readInputLine( const char* pPrompt, std::string& out_line )
{
#if defined( UNIFY_HAVE_READLINE )
    if( s_useReadline ) {
        char* pLine = readline( pPrompt );
        if( !pLine ) {
            return false;
        }
        out_line = pLine;
        if( *pLine ) {
            add_history( pLine );
        }
        free( pLine );
        return true;
    }
#endif

    if( s_isInteractive ) {
        fputs( pPrompt, stdout );
        fflush( stdout );
    }
    return (bool) std::getline( std::cin, out_line );
}


/*
 * =========================================================================
 * Recognizing a complete top-level item
 * =========================================================================
 */

/**
 * The result of scanning the text accumulated at the prompt so far.
 */
struct FormScan {
    FormScan()
        : depth( 0 )
        , inString( false )
        , inBlockComment( false )
        , firstSignificant( '\0' )
        , firstSignificantPos( 0 )
        , lastSignificant( '\0' )
    {}

    /// Nesting depth of (), [] and {} taken together, clamped at 0: an
    /// unbalanced closer is left for the parser to complain about rather
    /// than wedging the prompt in a state it can never leave.
    int depth;

    /// Inside an unterminated "..." literal (\ escapes the next character,
    /// matching m_unescapedString in vault-unify-parser.hpp).
    bool inString;

    /// Inside an unterminated block comment.
    bool inBlockComment;

    /// First/last character that is neither whitespace nor part of a
    /// comment, and where the first one is. '\0' means the text holds
    /// nothing but whitespace and comments.
    char firstSignificant;
    std::string::size_type firstSignificantPos;
    char lastSignificant;
};


/**
 * Lex `text` far enough to answer "is this a complete top-level item yet,
 * and if so what kind". This deliberately understands only what it takes
 * to find the end of an item -- strings, both comment forms and bracket
 * nesting -- and nothing else about the grammar; deciding whether the
 * text is actually VALID stays the parser's job.
 */
FormScan scanForm( const std::string& text )
{
    FormScan scan;

    const std::string::size_type n = text.size();
    std::string::size_type i = 0;
    bool inLineComment = false;

    while( i < n ) {
        const char c = text[i];

        if( scan.inString ) {
            if( '\\' == c && i + 1 < n ) {
                ++i; // Skip the escaped character, whatever it is.
            } else if( '"' == c ) {
                scan.inString = false;
                scan.lastSignificant = c;
            }
            ++i;
            continue;
        }
        if( scan.inBlockComment ) {
            if( '*' == c && i + 1 < n && '/' == text[i + 1] ) {
                scan.inBlockComment = false;
                ++i;
            }
            ++i;
            continue;
        }
        if( inLineComment ) {
            if( '\n' == c ) {
                inLineComment = false;
            }
            ++i;
            continue;
        }

        if( '/' == c && i + 1 < n && '/' == text[i + 1] ) {
            inLineComment = true;
            i += 2;
            continue;
        }
        if( '/' == c && i + 1 < n && '*' == text[i + 1] ) {
            scan.inBlockComment = true;
            i += 2;
            continue;
        }
        if( '"' == c ) {
            scan.inString = true;
            if( !scan.firstSignificant ) {
                scan.firstSignificant = c;
                scan.firstSignificantPos = i;
            }
            scan.lastSignificant = c;
            ++i;
            continue;
        }

        if( ' ' == c || '\t' == c || '\r' == c || '\n' == c || '\f' == c || '\v' == c ) {
            ++i;
            continue;
        }

        if( '(' == c || '[' == c || '{' == c ) {
            ++scan.depth;
        } else if( ')' == c || ']' == c || '}' == c ) {
            if( scan.depth > 0 ) {
                --scan.depth;
            }
        }

        if( !scan.firstSignificant ) {
            scan.firstSignificant = c;
            scan.firstSignificantPos = i;
        }
        scan.lastSignificant = c;
        ++i;
    }

    return scan;
}


/**
 * True once `scan` describes text that can be handed to the parser: no
 * literal or comment left hanging open, all brackets closed, and the last
 * thing in it is an item terminator. Both terminators are needed: a fact,
 * an import and a plain goal end with `;`, while a rule body, a
 * `query { ... }` block and the block statements end with `}` and take no
 * trailing semicolon (see LANGUAGE.md, "Semicolon discipline").
 */
bool isFormComplete( const FormScan& scan )
{
    if( scan.inString || scan.inBlockComment || scan.depth > 0 ) {
        return false;
    }
    return ';' == scan.lastSignificant || '}' == scan.lastSignificant;
}


/*
 * =========================================================================
 * Reporting a finished query
 * =========================================================================
 */

/**
 * Counters shared with the per-query completion callback below. The
 * callback runs on the engine's single worker thread while the REPL
 * thread sits in waitForEngineIdle(), and the REPL thread only reads
 * these again after that wait has returned -- which synchronizes with the
 * worker thread through the barrier callback's mutex, so no extra locking
 * is needed here (the same argument unify-run.cpp makes for its own error
 * counter).
 */
struct SessionCounters {
    SessionCounters() : errorCount( 0 ) {}
    int errorCount;
};


/**
 * True for the variables the parser invents while desugaring (`$__0`,
 * `$__1`, ... -- see ClauseContext::nextAnonVarName() in
 * vault-unify-parser.hpp). They appear in a goal's term tree, and
 * therefore in SolveJob::getSolutionList(), exactly like the ones the user
 * typed, but they are an implementation detail of `if`/`for`/`foreach`/
 * member-deref lowering and would be pure noise at the prompt.
 */
bool isInternalVarName( const std::string& strName )
{
    return 0 == strName.compare( 0, 3, "$__" );
}


/**
 * Print one finished query's result: the bindings of every user-written
 * variable, one line per solution, then a count. A solution whose
 * variables are all internal (or which has no variables at all, e.g.
 * `? print( hello );`) contributes no binding line -- the count alone
 * already says it succeeded.
 */
void reportQueryResult( vault::unify::SolveJob* pSolveJob )
{
    vault::unify::SolveJob::SolutionListPtr spSolutions =
        pSolveJob->getSolutionList();

    int solutionCount = 0;
    if( spSolutions ) {
        vault::unify::SolveJob::SolutionList::const_iterator it, itEnd = spSolutions->end();
        for( it = spSolutions->begin(); it != itEnd; ++it ) {
            ++solutionCount;
            const vault::unify::SolveJob::SolutionMapPtr& spMap = *it;
            if( !spMap ) {
                continue;
            }

            // SolutionMap is a std::map, so this comes out sorted by
            // variable name -- a stable order the user can rely on,
            // rather than whatever order the goal's term tree happened to
            // be visited in.
            std::string strLine;
            vault::unify::SolveJob::SolutionMap::const_iterator itVar, itVarEnd = spMap->end();
            for( itVar = spMap->begin(); itVar != itVarEnd; ++itVar ) {
                if( isInternalVarName( itVar->first ) ) {
                    continue;
                }
                if( !strLine.empty() ) {
                    strLine += ", ";
                }
                strLine += itVar->first;
                strLine += " = ";
                strLine += itVar->second;
            }
            if( !strLine.empty() ) {
                printf( "%s\n", strLine.c_str() );
            }
        }
    }

    if( 0 == solutionCount ) {
        printf( "-- no solutions\n" );
    } else if( 1 == solutionCount ) {
        printf( "-- 1 solution\n" );
    } else {
        printf( "-- %d solutions\n", solutionCount );
    }

    const int errorCount = pSolveJob->getErrorCount();
    if( errorCount > 0 ) {
        // Unlike a parse error, an internal unification error (UnifyError:
        // division by zero, an unbound variable in arithmetic, ...) is only
        // recorded on the job, never printed (SolveJob::recordError()). A
        // batch run surfaces it as a count at exit; at the prompt it is far
        // more useful right here, with the message.
        fflush( stdout );
        fprintf( stderr, "unify: %d unification error(s) in this query; last was: %s\n",
            errorCount, pSolveJob->getLastError().c_str() );
    }
}


/*
 * =========================================================================
 * REPL commands
 * =========================================================================
 */

const char* const s_commandHelp =
    "Type .ufy source at the prompt: a fact (`color( red );`), a rule\n"
    "(`warm( $x ) { color( $x ); }`), a `query { ... }` block or an\n"
    "`import \"other.ufy\";`. Definitions accumulate for the whole session,\n"
    "so later queries see them. Multi-line input continues until the item\n"
    "is closed; an empty line abandons a half-typed one.\n"
    "\n"
    "  ? <goals>       shorthand for `query { <goals> }`, e.g. `? color( $x );`\n"
    "  :help, :h       this text\n"
    "  :list [name]    list the clauses defined so far, optionally only\n"
    "                  those whose head is `name`\n"
    "  :load <file>    read and run a .ufy file into this session (unlike\n"
    "                  `import`, this re-reads a file already loaded)\n"
    "  :quit, :q       leave the REPL (so does Ctrl-D)\n";


/**
 * `:list [name]` -- ROADMAP Phase 4's "inspection". Walks the World's root
 * execution state, which is where both parsed clauses
 * (RuntimeContext::parseExecuteSegment()) and asserted ones
 * (SolveJob::performSlice()'s `__builtin_assert`) land, in definition
 * order -- the order they are also tried in.
 *
 * Two kinds of clause live there but are not something the user wrote and
 * are filtered out:
 *   - the builtins World::init() seeds the state with (print, concat,
 *     ...), all of them SimpleBuiltinClause subclasses;
 *   - the clauses the parser synthesizes to lower `for`/`foreach` bodies,
 *     which are named `__foreach__<n>` and friends (see
 *     ClauseContext::nextAnonClauseName()).
 */
void listClauses( vault::unify::RuntimeContext& rt, const std::string& strFilter )
{
    vault::unify::ExecutionState* pRoot = rt.getWorld()->getRootState();
    vault::unify::ExecutionState::ClauseIterator it = pRoot->clauseIterator();

    int shown = 0;
    for( ; it.isValid(); it.next() ) {
        const vault::unify::Clause* pClause = it.getClause();
        if( !pClause ) {
            continue;
        }
        // Engine item E1 (clause provenance). This used to be two guesses:
        // a dynamic_cast to SimpleBuiltinClause for "is it a builtin", and a
        // `__` head-name prefix test for "is it a desugared for/foreach/if".
        // Both happened to be right, and neither was a fact -- the cast
        // because every builtin is currently that one subclass, the prefix
        // because the parser happens to name its artefacts that way. A user
        // predicate legitimately called `__cache` was silently unlistable.
        //
        // The clause now records where it came from, so this asks.
        //
        // One deliberate behaviour change comes with that, and it is the
        // point rather than a side effect: a user predicate whose name
        // begins with `__` is now listed, because it is the user's code.
        if( pClause->getOrigin().isInternal() ) {
            continue;
        }
        const vault::unify::ConsTerm* pHead = pClause->leftHandTerm();
        if( !pHead ) {
            continue;
        }
        const std::string& strName = pHead->getName().value();
        if( !strFilter.empty() && strName != strFilter ) {
            continue;
        }
        printf( "%s\n", pClause->toString().c_str() );
        ++shown;
    }

    if( 0 == shown ) {
        if( strFilter.empty() ) {
            printf( "-- no clauses defined\n" );
        } else {
            printf( "-- no clauses named '%s'\n", strFilter.c_str() );
        }
    }
}


} // anonymous namespace


int runRepl( vault::unify::RuntimeContext& rt )
{
    SessionCounters counters;

    /*
     * The completion callback every query typed this session gets. It is
     * the REPL's whole "print" step -- see reportQueryResult().
     */
    boost::function<void (boost::shared_ptr<vault::unify::Job>)> onQueryFinished =
        [&counters]( boost::shared_ptr<vault::unify::Job> spJob ) {
            vault::unify::SolveJob* pSolveJob =
                dynamic_cast<vault::unify::SolveJob*>( spJob.get() );
            if( !pSolveJob ) {
                return;
            }
            counters.errorCount += pSolveJob->getErrorCount();
            reportQueryResult( pSolveJob );
        };

    /*
     * One FileDebugInfo stands in for the prompt for the whole session, so
     * parse errors and term debug locations say "<repl>" instead of
     * "<input>". Ownership: registered with the World here, which owns it
     * exclusively from that point on and frees it exactly once in
     * ~World() -- see World::adoptFileDebugInfo() (include/vault-unify.hpp)
     * and the identical note in unify-run.cpp. It must NOT be deleted here.
     *
     * Line numbers in a parse error are counted within the ONE item being
     * submitted, not from the start of the session, which is what makes
     * them useful for a multi-line rule typed at the prompt.
     */
    vault::unify::FileDebugInfo* pReplFileDebugInfo =
        new vault::unify::FileDebugInfo( REPL_FILE_URI );
    rt.getWorld()->adoptFileDebugInfo( pReplFileDebugInfo );

    const bool isInteractive = UNIFY_STDIN_IS_TTY();
    setupLineInput( isInteractive );

    if( isInteractive ) {
        printf( "Unify REPL. :help for help, :quit to leave.\n" );
    }

    // Text accumulated for the item currently being typed; kept alive for
    // as long as the engine is working on it (parseExecuteSegment() takes
    // iterators into it).
    std::string pending;

    for( ;; ) {
        fflush( stdout );

        std::string line;
        if( !readInputLine( pending.empty() ? PROMPT_MAIN : PROMPT_CONTINUE, line ) ) {
            // End of input. A half-typed item is simply dropped -- saying
            // so matters, since its clauses/queries never ran.
            if( isInteractive ) {
                printf( "\n" );
            }
            if( !pending.empty() ) {
                // Counted as an error: for a piped session
                // (`unify-run < script.ufy`) a truncated last item is a
                // real failure of the input, and the exit code is the only
                // place that can say so.
                ++counters.errorCount;
                fprintf( stderr, "unify: end of input inside an unfinished item; discarded.\n" );
            }
            break;
        }

        /*
         * Commands and the empty line are only recognized at the START of
         * an item, never in the middle of one: `:` and a blank line can
         * both legitimately occur inside a multi-line map term or rule
         * body, and stealing them there would make perfectly good source
         * untypeable.
         */
        if( pending.empty() ) {
            std::string::size_type first = line.find_first_not_of( " \t\r\n" );
            if( std::string::npos == first ) {
                continue;
            }
            if( ':' == line[first] ) {
                std::string strCommand = line.substr( first + 1 );
                // Split off the (single, optional) argument.
                std::string strArg;
                std::string::size_type sp = strCommand.find_first_of( " \t" );
                if( std::string::npos != sp ) {
                    strArg = strCommand.substr( sp + 1 );
                    strCommand.erase( sp );
                    std::string::size_type argFirst = strArg.find_first_not_of( " \t\r\n" );
                    std::string::size_type argLast = strArg.find_last_not_of( " \t\r\n" );
                    strArg = ( std::string::npos == argFirst )
                        ? std::string()
                        : strArg.substr( argFirst, argLast - argFirst + 1 );
                }

                if( "quit" == strCommand || "q" == strCommand ) {
                    break;
                } else if( "help" == strCommand || "h" == strCommand || "?" == strCommand ) {
                    fputs( s_commandHelp, stdout );
                } else if( "list" == strCommand ) {
                    listClauses( rt, strArg );
                } else if( "load" == strCommand ) {
                    if( strArg.empty() ) {
                        fprintf( stderr, "unify: :load needs a file name.\n" );
                    } else {
                        std::string content;
                        if( !unifytool::readWholeFile( strArg.c_str(), content ) ) {
                            fprintf( stderr, "unify: cannot open '%s'.\n", strArg.c_str() );
                        } else {
                            /*
                             * A FileDebugInfo of its own, so diagnostics
                             * name the loaded file and so an `import` it
                             * contains resolves relative to ITS directory
                             * -- exactly what unify-run.cpp does for the
                             * program on its command line. Same ownership
                             * rule: the World frees it.
                             */
                            vault::unify::FileDebugInfo* pLoadedFileDebugInfo =
                                new vault::unify::FileDebugInfo( strArg );
                            rt.getWorld()->adoptFileDebugInfo( pLoadedFileDebugInfo );
                            counters.errorCount += rt.parseExecuteSegment(
                                content.begin(), content.end(),
                                onQueryFinished, pLoadedFileDebugInfo );
                            unifytool::waitForEngineIdle( rt );
                        }
                    }
                } else {
                    fprintf( stderr, "unify: unknown command ':%s'; try :help.\n",
                        strCommand.c_str() );
                }
                continue;
            }
        } else {
            // An empty line while an item is still open abandons it --
            // the way out of a bracket typo that can never close.
            if( std::string::npos == line.find_first_not_of( " \t\r\n" ) ) {
                fprintf( stderr, "unify: discarded unfinished input.\n" );
                pending.clear();
                continue;
            }
        }

        /*
         * The trailing newline is not cosmetic: the skipper's line-comment
         * rule is `"//" >> *(char_ - eol) >> eol` (vault-unify-parser.hpp),
         * so a `// ...` comment at the very end of the text would not be
         * skippable without it.
         */
        pending += line;
        pending += '\n';

        FormScan scan = scanForm( pending );
        if( '\0' == scan.firstSignificant ) {
            // Nothing but whitespace and comments so far; there is no item
            // here to parse, and handing it to the parser would only
            // produce a bogus "parse error" at end of text.
            pending.clear();
            continue;
        }
        if( !isFormComplete( scan ) ) {
            continue;
        }

        /*
         * `? <goals>` -> `query { <goals> }`. The `?` is blanked rather
         * than cut out, and the closing brace appended after the text's
         * own final newline, so every line of what was typed keeps its
         * line number in a parse error; only columns on the FIRST line
         * shift, by the length of the synthesized prefix.
         */
        std::string segment;
        if( '?' == scan.firstSignificant ) {
            segment = pending;
            segment[scan.firstSignificantPos] = ' ';
            segment = "query {" + segment + "}\n";
        } else {
            segment = pending;
        }

        // Engine item E1: text typed at the prompt is TRANSCRIPT, not
        // MODULE. It reaches parseExecuteSegment() looking exactly like a
        // file (FileDebugInfo and all -- see pReplFileDebugInfo above), so
        // this call site is the only place that knows the difference.
        counters.errorCount += rt.parseExecuteSegment(
            segment.begin(), segment.end(),
            onQueryFinished, pReplFileDebugInfo,
            vault::unify::ClauseOrigin::TRANSCRIPT );

        /*
         * Nothing is read from the prompt again until every job this item
         * queued has finished, so a query's output can never interleave
         * with the next prompt, and `pending`/`segment` stay alive for as
         * long as the engine could still be looking at them.
         */
        unifytool::waitForEngineIdle( rt );
        pending.clear();
    }

    shutdownLineInput();
    fflush( stdout );

    return counters.errorCount;
}


} // namespace unifytool
