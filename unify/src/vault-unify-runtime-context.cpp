

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

/*
 * Workaround for a but in boost 1.58: It does not support
 * empty structs in BOOST_FUSION_ADOPT_STRUCT
 */
#undef BOOST_PP_VARIADICS
#define BOOST_PP_VARIADICS 0


#include <boost/config/warning_disable.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>
#include <boost/spirit/include/phoenix_object.hpp>
#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/fusion/include/io.hpp>

#include <vault-unification.hpp>
#include <vault-unify-parser.hpp>

#include <vault-unify-solvejob.hpp>
#include <vault-unify-debug.hpp>

#include <vault-unify-clause-standard.hpp>

#include <boost/spirit/repository/include/qi_iter_pos.hpp>
#include <boost/spirit/include/support_line_pos_iterator.hpp>


namespace vault {
namespace unify {


namespace {

typedef boost::spirit::line_pos_iterator<std::string::const_iterator> ParseErrorIterator;

/**
 * Prints a gcc-style parse-error diagnostic to stderr:
 *
 *   <file>:<line>:<column>: parse error
 *   <offending source line>
 *   <spaces>^
 *
 * `itBegin` must be a copy of the iterator taken at the very start of the
 * segment being parsed, never advanced afterwards -- boost::spirit::
 * get_column() needs it as a lower bound to compute the column of
 * `itError` within its line (see boost/spirit/include/
 * support_line_pos_iterator.hpp). The offending source line itself is
 * recovered directly from the underlying std::string::const_iterator
 * (line_pos_iterator::base()) by scanning outward for the enclosing
 * newlines, bounded by [itBegin, itEnd).
 */
void reportParseError(
        vault::unify::Engine* pEngine,
        ParseErrorIterator itBegin,
        ParseErrorIterator itError,
        ParseErrorIterator itEnd,
        const vault::unify::FileDebugInfo* pFileDebugInfo )
{
    long line = (long) boost::spirit::get_line( itError );
    long column = (long) boost::spirit::get_column( itBegin, itError );

    std::string::const_iterator itBufBegin = itBegin.base();
    std::string::const_iterator itBufEnd = itEnd.base();
    std::string::const_iterator itErrBase = itError.base();

    std::string::const_iterator itLineStart = itErrBase;
    while( itLineStart != itBufBegin && *(itLineStart - 1) != '\n' ) {
        --itLineStart;
    }
    std::string::const_iterator itLineStop = itErrBase;
    while( itLineStop != itBufEnd && *itLineStop != '\n' ) {
        ++itLineStop;
    }
    std::string strLine( itLineStart, itLineStop );
    if( !strLine.empty() && strLine[ strLine.size() - 1 ] == '\r' ) {
        strLine.resize( strLine.size() - 1 );
    }

    std::string strFile = pFileDebugInfo
        ? pFileDebugInfo->getFileUri()
        : std::string( "<input>" );

    // Engine item E10: reported as data. The engine's default diagnostic
    // sink prints exactly the three lines this function used to fprintf()
    // itself -- header, source line, caret -- so nothing a user or a CI log
    // sees changes; what changes is that a front end can now receive this
    // with the file, line and column intact instead of having to parse them
    // back out of formatted text.
    vault::unify::Diagnostic diagnostic;
    diagnostic.severity = vault::unify::Diagnostic::ERROR;
    diagnostic.uriFile = strFile;
    diagnostic.line = (uint64_t) line;
    diagnostic.column = (uint64_t) column;
    diagnostic.message = "parse error";
    diagnostic.sourceLine = strLine;

    if( pEngine ) {
        pEngine->writeDiagnostic( diagnostic );
    }
}


/**
 * ROADMAP Phase 2 ("File imports / include", SPEC.md section 15): reports a
 * missing/unreadable `import "path";` target, gcc-style but on one line (no
 * source-line/caret -- unlike reportParseError() above, this is not a
 * grammar failure; the statement parsed fine, opening its target just
 * failed), attributed to the IMPORTING file/line, as specified in the
 * design ("<importing-file>:<line>: cannot open import \"path\"").
 */
void reportImportError(
        vault::unify::Engine* pEngine,
        const vault::unify::FileDebugInfo* pImportingFileDebugInfo,
        int line,
        const std::string& strRawPath )
{
    std::string strFile = pImportingFileDebugInfo
        ? pImportingFileDebugInfo->getFileUri()
        : std::string( "<input>" );

    // Engine item E10. No source line and no caret here, deliberately: the
    // import statement parsed fine, so there is no offending text to point
    // at -- only a target that would not open. The default sink prints the
    // one-line form when sourceLine is empty, reproducing exactly what this
    // function used to fprintf().
    vault::unify::Diagnostic diagnostic;
    diagnostic.severity = vault::unify::Diagnostic::ERROR;
    diagnostic.uriFile = strFile;
    diagnostic.line = (uint64_t) line;
    diagnostic.column = 0;
    diagnostic.message = "cannot open import \"" + strRawPath + "\"";

    if( pEngine ) {
        pEngine->writeDiagnostic( diagnostic );
    }
}

} // anonymous namespace


int RuntimeContext::parseExecuteSegment(
        std::string::const_iterator itLine, 
        std::string::const_iterator itLineEnd,
        boost::function<void (boost::shared_ptr<vault::unify::Job>)> onFinished,
        const vault::unify::FileDebugInfo* pFileDebugInfo,
        ClauseOrigin::Kind kind )
{
    typedef boost::spirit::line_pos_iterator<std::string::const_iterator> ParseIterator;
    typedef vault::unify::PrologParser::ufy_skipper<ParseIterator> ParseSkipper;

    ParseIterator itPosLine( itLine );
    ParseIterator itPosLineEnd( itLineEnd );
    // Never advanced; used as the lower bound for get_column()/error-line
    // extraction (see reportParseError() above).
    ParseIterator itPosBegin( itLine );

    vault::unify::PrologParser::EventInput inputEvent;
    vault::unify::PrologParser::Context prologContext( getWorld() );
    vault::unify::PrologParser::ClauseContext clauseContext( prologContext );
    vault::unify::PrologParser::ClauseParser< ParseIterator, ParseSkipper >
        parserEvent( itPosLine, prologContext );
    ParseSkipper ufySkipper( prologContext );

    if( pFileDebugInfo ) {
        prologContext.setFileDebugInfo( pFileDebugInfo );
    }

    int lastStartLine = 1;
    int errorCount = 0;

    while( itPosLine != itPosLineEnd ) {
        ParseIterator  itPosLineOld = itPosLine;
        bool r = boost::spirit::qi::phrase_parse( itPosLine, itPosLineEnd,
            parserEvent, ufySkipper, inputEvent );
        bool advanced = ( itPosLine != itPosLineOld );
        if( r && advanced ) {
            // What did we parse? Import is checked first: a successfully
            // matched import event leaves inputEvent.query/inputEvent.clause
            // at their default-empty state too (see EventInput's converting
            // constructors, src/vault-unify-parser.hpp), exactly the same
            // way a clause match leaves inputEvent.query empty below -- so
            // whichever alternative actually matched must be distinguished
            // in the same order m_ruleEvent tries them (import, then query,
            // then clause).
            if( !inputEvent.import.path.empty() ) {
                errorCount += processImport(
                    inputEvent.import.path, lastStartLine,
                    pFileDebugInfo, onFinished );
                clauseContext.reset();
            } else if( inputEvent.query.queryGoal.consTerms.empty() ) {
                vault::unify::Clause* pClause = NULL;
                (void) prologContext.createClause(
                    clauseContext, 
                    inputEvent.clause, pClause );
                // Engine item E1: the caller says whether this segment is a
                // module's text or a line typed at a prompt. Both arrive
                // here identically -- same parser, and the REPL supplies a
                // FileDebugInfo too (pseudo-URI "<repl>") -- so the
                // distinction cannot be recovered from anything visible in
                // this function, and deriving it from the URI would be
                // exactly the string heuristic E1 exists to remove.
                // Engine item E1. The file comes from the segment's
                // FileDebugInfo and the line from the parser's own
                // line counter -- neither is on the clause itself, since
                // the parser records debug info against TERMS via the
                // World's map, not against clauses.
                ClauseOrigin origin;
                origin.kind = kind;
                if( pFileDebugInfo ) {
                    origin.uriFile = pFileDebugInfo->getFileUri();
                }
                origin.line = (uint64_t) lastStartLine;
                m_esRoot->appendClause( m_spWorld, pClause, origin );
                VAULT_UNIFY_DI( ALWAYS, "line %d: Added clause '%s'.\n", 
                    lastStartLine,
                    pClause->toString().c_str() );
                clauseContext.reset();
            } else {
                // Main Goal
                vault::unify::Goal* pGoal = NULL;
                {
                    // Intermediate goals.
                    std::list<AbstractTerm*> lsGoalTerms;
                    (void) prologContext.createGoal( 
                        clauseContext, 
                        inputEvent.query.queryGoal, lsGoalTerms );
                    pGoal = new vault::unify::Goal(
                        lsGoalTerms.begin(), lsGoalTerms.end() );
                }
                vault::unify::SolveJob* jobSolve = new vault::unify::SolveJob();
                jobSolve->setWorld( m_spWorld );
                // jobSolve->setExecutionState( m_esRoot );
                jobSolve->setGoal( pGoal );
                // Query Goal is only referenced (not owned) by pGoal/setGoal;
                // adopt it here so it is deleted with the job that solves it
                // (ROADMAP Phase 1: SolveJob arena, pass 1 - query Goal
                // ownership).
                jobSolve->adoptGoal( pGoal );
                jobSolve->onFinished( onFinished );
                VAULT_UNIFY_DI( ALWAYS, "line %d: Adding goal '%s'.\n",
                    lastStartLine,
                    pGoal->toString().c_str() );
                jobSolve->startJob( m_pEngine );
                m_pEngine->addJob( boost::shared_ptr<vault::unify::Job>( jobSolve ) );
                clauseContext.reset();
            }
        } else {
            ++errorCount;
            reportParseError( m_pEngine, itPosBegin, itPosLine,
                              itPosLineEnd, pFileDebugInfo );
            // Break, if we did not advance in the source. Otherwise, we would loop.
            if( !advanced ) break;
        }
        lastStartLine = (int) itPosLine.position();
    }

    return errorCount;
}


/**
 * ROADMAP Phase 2 ("File imports / include", SPEC.md section 15): see the
 * declaration's comment (include/vault-unify.hpp) for the parameters and
 * overall contract (resolution, once-semantics, error reporting). The
 * FileDebugInfo ownership rule for the object constructed here is documented
 * on World::adoptFileDebugInfo() (include/vault-unify.hpp) and on
 * ~World()'s sweep (vault-unify-world.cpp) -- in short: this function
 * registers it with World immediately and then MUST NOT ever delete it
 * itself, whether or not the imported file goes on to build any term that
 * references it via TermDebugInfo (the case that registration exists to
 * cover).
 */
int RuntimeContext::processImport(
        const std::string& strRawPath,
        int line,
        const vault::unify::FileDebugInfo* pCurrentFileDebugInfo,
        boost::function<void (boost::shared_ptr<vault::unify::Job>)> onFinished )
{
    namespace bfs = boost::filesystem;

    // Resolve relative to the importing file's directory; if that file is
    // unknown (pCurrentFileDebugInfo == NULL, e.g. a REST-fed segment) or
    // its URI has no directory component of its own, baseDir stays empty
    // and resolvedPath is just strRawPath as-is, which boost::filesystem
    // (both ifstream-via-.c_str() below and canonical()) resolves against
    // the process CWD -- exactly the documented fallback.
    bfs::path baseDir;
    if( pCurrentFileDebugInfo ) {
        baseDir = bfs::path( pCurrentFileDebugInfo->getFileUri() ).parent_path();
    }
    bfs::path resolvedPath = baseDir.empty() ? bfs::path( strRawPath ) : baseDir / strRawPath;

    // canonical() both resolves the file (proving it exists and is
    // readable-as-a-path) and gives us an absolute, symlink-free key for the
    // once-semantics set below; a nonexistent/inaccessible target reports
    // via the error_code overload rather than an exception.
    boost::system::error_code ec;
    bfs::path canonicalPath = bfs::canonical( resolvedPath, ec );
    if( ec ) {
        reportImportError( m_pEngine, pCurrentFileDebugInfo, line, strRawPath );
        return 1;
    }

    // Once-semantics ("like #pragma once"): the canonicalized absolute path
    // string is the dedup key, so two different-looking relative spellings
    // of the same file are recognized as one. A duplicate import is a
    // silent no-op, not an error, and this is also what breaks import
    // cycles -- a file that (transitively) imports itself finds its own
    // canonical path already in the set on the recursive visit and simply
    // does not recurse again.
    std::string strCanonicalKey = canonicalPath.string();
    if( !m_importedFiles.insert( strCanonicalKey ).second ) {
        return 0;
    }

    std::ifstream in( canonicalPath.string().c_str(), std::ios::binary );
    if( !in ) {
        // Extremely unlikely race/permission issue between canonical()
        // succeeding above and this open -- handled the same way a missing
        // file is; the once-set entry is left in place regardless (retrying
        // the same import later in this file would just fail again the
        // same way, which is fine).
        reportImportError( m_pEngine, pCurrentFileDebugInfo, line, strRawPath );
        return 1;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    // Owned locally: only the imported file's *content* string needs to
    // outlive its own parse (the recursive parseExecuteSegment() call
    // below) -- nothing keeps a reference to it once that call returns
    // (unlike the FileDebugInfo just below, which terms parsed from this
    // content DO keep referencing, via TermDebugInfo, for the lifetime of
    // the World).
    std::string strContent = buf.str();

    // See this function's own header comment above for the ownership rule.
    // Using resolvedPath (not the canonical, absolute form) for the
    // FileDebugInfo's URI keeps diagnostics readable (it looks like the
    // path actually written/reachable from the importing file) and keeps
    // nested imports resolving relative to a sensible directory.
    vault::unify::FileDebugInfo* pImportedFileDebugInfo =
        new vault::unify::FileDebugInfo( resolvedPath.string() );
    getWorld()->adoptFileDebugInfo( pImportedFileDebugInfo );

    // An imported file is a module however it was reached: importing from
    // the prompt yields module clauses, not transcript ones, so the kind is
    // fixed here rather than inherited from the importing segment.
    return parseExecuteSegment(
        strContent.begin(), strContent.end(),
        onFinished, pImportedFileDebugInfo, ClauseOrigin::MODULE );
}


/**
 * ROADMAP Phase 1 (Ownership model), pass 2: frees the WorldChangeSink
 * allocated by setupDone() below. m_pEngine is intentionally NOT deleted:
 * Engine's worker thread (see Engine::addWorkerThread()/executionLoop())
 * has no shutdown path yet (ROADMAP Phase 5.1), so deleting the Engine
 * object out from under a thread that may still reference it would be
 * unsafe; the Engine (and its thread, and its job queues) is left alive
 * and leaked, exactly as it already was before this pass. m_spWorld (a
 * shared_ptr) is released via ordinary member destruction right after this
 * body returns, which runs ~World() once this is the last reference.
 *
 * All current callers only let a RuntimeContext go out of scope once every
 * job it started has finished and its worker thread is idle again (see
 * unify-run.cpp's barrier-job wait), so m_pWorldChangeSink is never in use
 * by an in-flight performSlice() at this point.
 */
RuntimeContext::~RuntimeContext()
{
    delete m_pWorldChangeSink;
    m_pWorldChangeSink = NULL;
}


int RuntimeContext::setupDone()
{
    /*
     * Initialize the world. The world contains the state of everything.
     */
    m_spWorld.reset( new vault::unify::World() );
    m_spWorld->init();

    /*
     * Initialize the engine. The engine groups everything that changes
     * world's state.
     */
    m_pEngine = new vault::unify::Engine();    

    /*
     * Create a world change sink.
     * Internally, every world can be monitored exactly once for changes.
     * A real world setup would multiplex this single connection for both
     * monitoring/debugging and real output.
     */
    m_pWorldChangeSink = new vault::unify::WorldChangeSink( m_pEngine );
    m_pEngine->setWorldChangeSink( m_pWorldChangeSink );
    
    // Convenience abbreviation.
    m_esRoot = m_spWorld->getRootState();

    /*
     * Basically, we could configure as much worker threads as we like or require to.
     * In this example, we use one worker thread.
     */
    m_pEngine->addWorkerThread();

    return 0;
}


};
};