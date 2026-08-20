

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

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

    fprintf( stderr, "%s:%ld:%ld: parse error\n", strFile.c_str(), line, column );
    fprintf( stderr, "%s\n", strLine.c_str() );
    std::string strCaret( column > 1 ? (std::size_t)( column - 1 ) : (std::size_t) 0, ' ' );
    fprintf( stderr, "%s^\n", strCaret.c_str() );
}

} // anonymous namespace


int RuntimeContext::parseExecuteSegment(
        std::string::const_iterator itLine, 
        std::string::const_iterator itLineEnd,
        boost::function<void (boost::shared_ptr<vault::unify::Job>)> onFinished,
        const vault::unify::FileDebugInfo* pFileDebugInfo )
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
            // What did we parse?
            if( inputEvent.query.queryGoal.consTerms.empty() ) {
                vault::unify::Clause* pClause = NULL;
                (void) prologContext.createClause(
                    clauseContext, 
                    inputEvent.clause, pClause );
                m_esRoot->appendClause( m_spWorld, pClause );
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
            reportParseError( itPosBegin, itPosLine, itPosLineEnd, pFileDebugInfo );
            // Break, if we did not advance in the source. Otherwise, we would loop.
            if( !advanced ) break;
        }
        lastStartLine = (int) itPosLine.position();
    }

    return errorCount;
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