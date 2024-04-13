

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

    while( itPosLine != itPosLineEnd ) {
        ParseIterator  itPosLineOld = itPosLine;
        bool r = boost::spirit::qi::phrase_parse( itPosLine, itPosLineEnd,
            parserEvent, ufySkipper, inputEvent );
        if( r  ) {
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
                jobSolve->onFinished( onFinished );
                VAULT_UNIFY_DI( ALWAYS, "line %d: Adding goal '%s'.\n",
                    lastStartLine,
                    pGoal->toString().c_str() );
                jobSolve->startJob( m_pEngine );
                m_pEngine->addJob( boost::shared_ptr<vault::unify::Job>( jobSolve ) );
                clauseContext.reset();
            }
        } else {
            fprintf( stderr, "Parse error.\n" );
            // Break, if we did not advance in the source. Otherwise, we would loop.
            if( itPosLineOld == itPosLine ) break;
        }
        lastStartLine = (int) itPosLine.position();
    }

    return 0;
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