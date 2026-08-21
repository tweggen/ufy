

#include <stdio.h>
#include <stdlib.h>
#include <cerrno>

/*
 * Workaround for a but in boost 1.58: It does not support
 * empty structs in BOOST_FUSION_ADOPT_STRUCT
 */
#undef BOOST_PP_VARIADICS
#define BOOST_PP_VARIADICS 0

#include <vault-unify-parser.hpp>

#include <vault-unify-clause-standard.hpp>

#include <vault-unify-debug.hpp>

namespace vault {
namespace unify {
namespace PrologParser {

int ClauseContext::m_anonClauseIndex;

namespace {

/**
 * ROADMAP ("for"/"foreach" + ranges, language owner request 2026-08-21):
 * a range with both bounds LITERAL integer atoms is expanded eagerly, right
 * here at parse time (AnyTermFactory::operator()(const RangeTermInput&)
 * below), into a plain ArrayTerm -- so it needs the same "does this atom's
 * text parse fully as an int64" check
 * vault-unify-clause-builtin-arith.cpp's parseInt64() performs for solve-
 * time arithmetic. Duplicated locally (same ~15 lines) rather than shared
 * across translation units, matching this module's existing per-file
 * anonymous-namespace-helper style (e.g. every builtin .cpp under src/ has
 * its own small local helpers rather than a shared utility header).
 */
bool parseRangeLiteralInt64( const std::string& s, int64_t& out_value )
{
    if( s.empty() ) {
        return false;
    }
    errno = 0;
    char* pEnd = NULL;
    long long v = strtoll( s.c_str(), &pEnd, 10 );
    if( pEnd != s.c_str() + s.length() ) {
        return false;
    }
    if( ERANGE == errno ) {
        return false;
    }
    out_value = (int64_t) v;
    return true;
}

/**
 * Cap on the number of elements an eagerly-expanded (literal-bounds) range
 * literal may produce, mirroring the same cap `RangeBuiltinClause`
 * (vault-unify-clause-builtin-arith.cpp) enforces for a solve-time,
 * variable-bounds range -- see SPEC.md's ranges section. A literal range
 * this large in SOURCE TEXT is almost certainly a typo, not a genuine 100k-
 * element array literal; there is no clean way to fail a parse-time
 * desugaring the way a builtin can return UnifyError, so this silently
 * truncates (logged) rather than attempting the full allocation.
 */
const int64_t RANGE_MAX_ELEMENTS = 100000;



/**
 * Collect the distinct VarTerms reachable from pTerm, in first-encounter
 * order (an otherwise-arbitrary but deterministic-within-one-call order),
 * skipping anything already in out_visited. Used by
 * AnyTermFactory::operator()(IfStatementInput) to find the free variables
 * of an `if` statement's cond+body -- see there for why they need to be
 * collected up front (to build each synthesized clause's own fresh
 * variables via cloneTermTree(), and the call-site term left in the
 * enclosing goal chain).
 *
 * pTerm's actual VarTerm nodes are not const in memory (they live on as
 * ordinary VarTerm* in ClauseContext::m_mapSymbols and are reused directly
 * at the if-statement's call site); the const-qualified traversal here
 * only matches collectTermTree()'s style, so the const is cast away when
 * a VarTerm is found and stored.
 */
void collectVarTermsOrdered(
        const vault::unify::AbstractTerm* pTerm,
        std::set<const vault::unify::AbstractTerm*>& out_visited,
        std::vector<vault::unify::VarTerm*>& out_vars )
{
    if( !pTerm ) {
        return;
    }
    if( !out_visited.insert( pTerm ).second ) {
        return;
    }
    if( const vault::unify::VarTerm* pVarTerm = dynamic_cast<const vault::unify::VarTerm*>( pTerm ) ) {
        out_vars.push_back( const_cast<vault::unify::VarTerm*>( pVarTerm ) );
        return;
    }
    vault::unify::AbstractTermIterator* pIt = pTerm->abstractTermIterator();
    if( pIt ) {
        while( pIt->isValid() ) {
            const vault::unify::TermTraversable* pChildTraversable = pIt->getTermTraversable();
            if( pChildTraversable ) {
                const vault::unify::AbstractTerm* pChildTerm =
                    dynamic_cast<const vault::unify::AbstractTerm*>( pChildTraversable );
                if( pChildTerm ) {
                    collectVarTermsOrdered( pChildTerm, out_visited, out_vars );
                }
            }
            pIt->next();
        }
        delete pIt;
    }
}

/**
 * Delete the structural (non-VarTerm) nodes of a scratch term tree built
 * by AnyTermFactory::operator()(IfStatementInput) while assembling an
 * `if` statement's cond+body via the enclosing ClauseContext -- once
 * cloneTermTree() has copied that scratch tree into a synthesized clause,
 * the ConsTerm/MapTerm scaffolding is no longer referenced from anywhere
 * and must be freed here, or it leaks.
 *
 * VarTerm leaves are deliberately left alone (not deleted, not recursed
 * into -- they are always leaves): every VarTerm reachable from cond+body
 * is, by construction, exactly one of the "free variables" collected by
 * collectVarTermsOrdered() above, which the if-statement's call-site term
 * reuses directly. That call-site term becomes part of the enclosing
 * clause/query's own tree, so those VarTerms must survive along with it --
 * deleting them here would leave the call site holding a dangling pointer.
 */
void deleteScratchTermTree(
        const vault::unify::AbstractTerm* pTerm,
        std::set<const vault::unify::AbstractTerm*>& out_visited )
{
    if( !pTerm ) {
        return;
    }
    if( !out_visited.insert( pTerm ).second ) {
        return;
    }
    if( dynamic_cast<const vault::unify::VarTerm*>( pTerm ) ) {
        return;
    }
    vault::unify::AbstractTermIterator* pIt = pTerm->abstractTermIterator();
    if( pIt ) {
        while( pIt->isValid() ) {
            const vault::unify::TermTraversable* pChildTraversable = pIt->getTermTraversable();
            if( pChildTraversable ) {
                const vault::unify::AbstractTerm* pChildTerm =
                    dynamic_cast<const vault::unify::AbstractTerm*>( pChildTraversable );
                if( pChildTerm ) {
                    deleteScratchTermTree( pChildTerm, out_visited );
                }
            }
            pIt->next();
        }
        delete pIt;
    }
    delete pTerm;
}

} // anonymous namespace

/**
 * Factory class to factor a Goal from the syntax tree.
 *
 * This class traverses the XxxInput classes as generated by the parser
 * and generates appropriate AbstractTerm term trees that eventually are
 * used to create a set of goals representing the original goal input.
 * One main output goal is created from the main AbstractTerm tree as 
 * requested by the user. However, different additional trees may be
 * generated along the way.
 */
class AnyTermFactory
        : public boost::static_visitor<vault::unify::AbstractTerm*>
{
#if 0
    void storeTermDebugInfo( const AbstractTerm* pTerm,  )  {
        vault::unify::FileDebugInfo* pFDI = m_clauseContext.m_context.getFileDebugInfo();
        if( !pFDI ) return;
        if( pFDI ) {
            vault::unify::WorldPtr spWorld = m_clauseContext.m_context.getWorld();
            vault::unify::TermDebugInfo* pTDI = new vault::unify::TermDebugInfo(
                pTerm, pFDI, )
            spWorld.setTermDebugInfo( pTerm, )
        }
    }
#endif
public:
    /**
     * Create a new term factory within the given clause context. 
     */
    AnyTermFactory( 
        ClauseContext& clauseContext,
        std::list<vault::unify::AbstractTerm*>& out_lsTerms )
            : m_clauseContext( clauseContext ),
              m_lsTerms( out_lsTerms )
    {
    }

#if USE_NIL
    /**
     * We cannot factory anything of an unset variant.
     */
    vault::unify::AbstractTerm* operator()( const unifynil& ) const 
    {
        // do nothing.
        //BOOST_ASSERT(0);
        return 0;
    }
#endif


    vault::unify::AbstractTerm* operator()( const AnyTermInput& anyTermInput ) const 
    {
        return boost::apply_visitor( *this, anyTermInput.term );
    }

    vault::unify::AbstractTerm* operator()( const AnyStatementInput& anyStatementInput ) const 
    {
        return boost::apply_visitor( *this, anyStatementInput.statement );
    }

    vault::unify::AbstractTerm* operator()( const SingleGoalInput& singleGoalInput ) const 
    {
        vault::unify::AbstractTerm* pAbstractTerm = (*this)( singleGoalInput.rhs );

        if( singleGoalInput.first ) {
            vault::unify::ConsTerm* pConsTerm = dynamic_cast<vault::unify::ConsTerm*>( pAbstractTerm );
            if( pConsTerm ) {
                char op = singleGoalInput.first.get();
                if( '!'==op ) {
                    pConsTerm->setNegated( true );
                }
            } else {
                VAULT_UNIFY_DI( ALWAYS, "Unable to apply not operator: No consterm.\n" );
            }
        }

        return pAbstractTerm;
    }

    vault::unify::AbstractTerm* operator()( const PrefixTermInput& prefixTermInput ) const 
    {
        vault::unify::AbstractTerm* rhs;
        rhs = (*this)( prefixTermInput.rhs );

        // Do we have prefix operator at all?
        if( !prefixTermInput.first ) {
            // We don't, so just return right hand side.
            return rhs;
        }

        vault::unify::ConsTerm* termResult = NULL;

        // We have a prefix operator. Convert it to an internal call.
        char op = prefixTermInput.first.get();
        std::string atomName;
        switch( op ) {
        case '!': {
            atomName = "__builtin_not";
            break;
        default:
            break;
        }
        }

        // Create cons term, if not done yet.
        if( !termResult ) {
            termResult = new vault::unify::ConsTerm( atomName.c_str(), rhs );
            // storeTermDebugInfo( termResult );
        }

        return termResult;
    }

    /**
     * Fold a multiplicative ('*','/') or additive ('+','-') chain
     * left-to-right into nested `__builtin_arith(opAtom, lhs, rhs)`
     * ConsTerms -- e.g. `2 + 3 * 4` (parsed as additive(lhs=2,
     * rhsList=[{op='+',second=multiplicative(lhs=3,
     * rhsList=[{op='*',second=4}])}])) becomes
     * `__builtin_arith("+", 2, __builtin_arith("*", 3, 4))`. Both
     * m_ruleMultiplicative and m_ruleAdditive produce this same
     * ArithTermInput struct; precedence is already resolved by the
     * grammar's nesting (a multiplicative chain is always fully built
     * before it ever becomes one operand of an enclosing additive chain),
     * so this fold does not need to know which level it came from.
     *
     * Every ConsTerm allocated here (the op atom and the __builtin_arith
     * wrapper) becomes part of the enclosing clause/query's own term
     * tree exactly like any other ConsTerm this factory builds -- reachable
     * from the flat Goal term list Context::createGoal() assembles, and
     * freed by the same collectTermTree()-based sweep that already owns
     * every other parse-time term (World::~World() for a clause,
     * ~SolveJob() for a query). No new ownership mechanism is needed here.
     */
    vault::unify::AbstractTerm* operator()( const ArithTermInput& arithTermInput ) const
    {
        vault::unify::AbstractTerm* acc = (*this)( arithTermInput.lhs );

        std::vector<ArithTermRhsInput>::const_iterator it, itEnd = arithTermInput.rhsList.end();
        for( it = arithTermInput.rhsList.begin(); it != itEnd; ++it ) {
            vault::unify::AbstractTerm* rhsTerm = (*this)( it->second );
            std::string opStr( 1, it->op );
            vault::unify::ConsTerm* pOpAtom = new vault::unify::ConsTerm( opStr.c_str() );
            vault::unify::ConsTerm* pArith = new vault::unify::ConsTerm(
                "__builtin_arith", pOpAtom, acc, rhsTerm );
            acc = pArith;
        }

        return acc;
    }

    /**
     * Comparison level ('==','!=','<=','>=','<','>'): single-shot, unlike
     * the arithmetic chain above. No operator present -> just the lhs,
     * unchanged (the same "single operand" fast path InfixTermsInput uses
     * below). Otherwise builds `__builtin_compare(opAtom, lhs, rhs)`,
     * resolved by CompareBuiltinClause
     * (vault-unify-clause-builtin-arith.cpp). Ownership: same reasoning as
     * operator()(const ArithTermInput&) above -- ordinary parse-time
     * ConsTerms, no new ownership mechanism needed.
     */
    vault::unify::AbstractTerm* operator()( const CompareTermInput& compareTermInput ) const
    {
        vault::unify::AbstractTerm* lhs = (*this)( compareTermInput.lhs );

        if( !compareTermInput.rhs ) {
            return lhs;
        }
        const CompareTermRhsInput& cmpRhs = compareTermInput.rhs.get();
        vault::unify::AbstractTerm* rhs = (*this)( cmpRhs.second );

        vault::unify::ConsTerm* pOpAtom = new vault::unify::ConsTerm( cmpRhs.op.c_str() );
        vault::unify::ConsTerm* pCompare = new vault::unify::ConsTerm(
            "__builtin_compare", pOpAtom, lhs, rhs );

        return pCompare;
    }

    vault::unify::AbstractTerm* operator()( const InfixTermsInput& infixTermsInput ) const
    {
        vault::unify::AbstractTerm *lhs, *rhs, *out_pAbstractTerm = NULL;
        vault::unify::AbstractTerm *pPreGoal1 = NULL;
        vault::unify::VarTerm *pVarTerm1 = NULL;
        std::string strVarTerm1;

        // Get left hand side.
        lhs = (*this)( infixTermsInput.atilhs );

        // Special case: Just one operand.
        if( !infixTermsInput.rhs ) {
            out_pAbstractTerm = lhs;
            return out_pAbstractTerm;
        }
        const InfixTermRightHandSide& ifxrhs = infixTermsInput.rhs.get();

        char op = ifxrhs.first;
        rhs = (*this)( ifxrhs.second );

        WorldPtr spWorld( m_clauseContext.m_context.getWorld() );
        TermDebugInfo* pTermDebugInfo = spWorld->getTermDebugInfo( lhs );

        // Create the real atom from the infix operand.
        std::string atomName;
        switch( op ) {
        case '\0': {
            // Substitute deref here.
            //VAULT_UNIFY_DI( ALWAYS, "Creating deref member.\n" );
            atomName = "__builtin_member_deref";

            // =deref( lhs, rhs ) replaced by anon var __x
            // Pre-goal added
            // __builtin_member_deref( lhs, rhs, __x )
            pVarTerm1 = new vault::unify::VarTerm();
            strVarTerm1 = m_clauseContext.nextAnonVarName();
            pVarTerm1->setOriginalVarName( strVarTerm1 );
            m_clauseContext.m_mapSymbols[strVarTerm1] = pVarTerm1;

            pPreGoal1 = new vault::unify::ConsTerm( 
                atomName.c_str(), lhs, rhs, pVarTerm1 );

            if( pTermDebugInfo ) {
                spWorld->setTermDebugInfo( pPreGoal1, pTermDebugInfo);
            }

            // Add the new variable definition that resolves the member.
            m_lsTerms.push_back( pPreGoal1 );

            // Now create the variable reference in place of the 
            // original member deref
            out_pAbstractTerm = pVarTerm1;

            break;
        }
        case '=': {
            // Arithmetic-in-'=' desugar (ROADMAP Phase 2, SPEC.md section
            // 4): if either side is a `__builtin_arith` tree (built by
            // operator()(const ArithTermInput&) above), unify's plain
            // structural-equality semantics would never match it against
            // the evaluated result, so `=` means something different here:
            // evaluate the arithmetic side and unify the *result* with the
            // other side, via `__builtin_eval(arithSide, otherSide)`
            // (ArithEvalBuiltinClause, vault-unify-clause-builtin-arith.cpp).
            // A plain `=` between ordinary terms (neither side arithmetic)
            // is completely unchanged: still "unify".
            //
            // If BOTH sides happen to be arithmetic (e.g. `$x + 1 = $y + 2`),
            // the lhs wins arbitrarily (not a case any conformance test or
            // sample program exercises; not over-engineered here) -- the
            // rhs is then unified as a structural term against the
            // computed number, which simply will not unify.
            vault::unify::ConsTerm* pLhsCons = dynamic_cast<vault::unify::ConsTerm*>( lhs );
            vault::unify::ConsTerm* pRhsCons = dynamic_cast<vault::unify::ConsTerm*>( rhs );
            bool lhsIsArith = pLhsCons && 3==pLhsCons->getArity()
                && pLhsCons->getName().value() == "__builtin_arith";
            bool rhsIsArith = pRhsCons && 3==pRhsCons->getArity()
                && pRhsCons->getName().value() == "__builtin_arith";

            if( lhsIsArith || rhsIsArith ) {
                atomName = "__builtin_eval";
                if( lhsIsArith ) {
                    out_pAbstractTerm = new vault::unify::ConsTerm( atomName.c_str(), lhs, rhs );
                } else {
                    out_pAbstractTerm = new vault::unify::ConsTerm( atomName.c_str(), rhs, lhs );
                }
                if( pTermDebugInfo ) {
                    spWorld->setTermDebugInfo( out_pAbstractTerm, pTermDebugInfo );
                }
            } else {
                /*
                 * findall desugar (ROADMAP Phase 2, SPEC.md section 11):
                 * `$out = findall( $tmpl, $goal );` -- if either side of
                 * '=' is a ConsTerm literally named "findall" with EXACTLY
                 * 2 arguments, it is the reserved findall(...) call shape
                 * (mirroring how "cut" is reserved by exact name+arity,
                 * section 8) -- NOT an ordinary 2-arity user predicate
                 * named "findall" used as a bare goal (any OTHER arity, or
                 * not written on a side of '=' at all, stays a completely
                 * ordinary ConsTerm/call; a design choice documented in
                 * SPEC.md rather than over-engineered here). Desugars to
                 * `__builtin_findall(tmpl, goal, otherSide)`, the solver
                 * special form SolveJob::performSlice() (vault-unify-
                 * solvejob.cpp) recognizes directly, mirroring cut.
                 */
                vault::unify::ConsTerm* pFindallCons = NULL;
                vault::unify::AbstractTerm* pOtherSide = NULL;
                if( pLhsCons && 2==pLhsCons->getArity()
                        && pLhsCons->getName().value() == "findall" ) {
                    pFindallCons = pLhsCons;
                    pOtherSide = rhs;
                } else if( pRhsCons && 2==pRhsCons->getArity()
                        && pRhsCons->getName().value() == "findall" ) {
                    pFindallCons = pRhsCons;
                    pOtherSide = lhs;
                }

                if( pFindallCons ) {
                    atomName = "__builtin_findall";
                    // Reuse the template/subgoal terms directly (they are
                    // already fully built AbstractTerm trees); the type
                    // system only ever hands them back to us as const via
                    // getTermAt(), so strip that (matching the const_cast
                    // pattern already used elsewhere in this file, e.g.
                    // collectVarTermsOrdered() above) -- we are simply
                    // reusing pointers this SAME factory just allocated,
                    // not touching anyone else's data.
                    vault::unify::AbstractTerm* pTmpl =
                        const_cast<vault::unify::AbstractTerm*>( pFindallCons->getTermAt( 0 ) );
                    vault::unify::AbstractTerm* pSubgoal =
                        const_cast<vault::unify::AbstractTerm*>( pFindallCons->getTermAt( 1 ) );
                    out_pAbstractTerm = new vault::unify::ConsTerm(
                        atomName.c_str(), pTmpl, pSubgoal, pOtherSide );
                    if( pTermDebugInfo ) {
                        spWorld->setTermDebugInfo( out_pAbstractTerm, pTermDebugInfo );
                    }
                    /*
                     * The wrapping "findall(tmpl, goal)" ConsTerm node
                     * itself (built by the generic ConsTermInput factory
                     * above, BEFORE this shape could even be recognized)
                     * is now unreachable from the assembled tree -- its
                     * two children were just reused directly above. Free
                     * just that one wrapper node; ConsTerm::~ConsTerm() is
                     * trivial and does not touch children (include/
                     * vault-unify.hpp), so this cannot double-free
                     * pTmpl/pSubgoal. Mirrors deleteScratchTermTree()'s
                     * established pattern (this file, the `if` desugaring
                     * above) of discarding an orphaned structural scratch
                     * node: its TermDebugInfo map entry, if any, is left as
                     * a harmless dangling key -- World::~World()'s cleanup
                     * sweep only ever dereferences the map's VALUES, never
                     * compares through a stale key beyond ordinary pointer
                     * ordering (ConsTermInput's factory above registers a
                     * TermDebugInfo for every ConsTerm it builds, including
                     * this now-discarded wrapper).
                     */
                    delete pFindallCons;
                } else {
                    atomName = "unify";
                }
            }
            break;
        }
        case '[':
            atomName = "__builtin_array_deref";
            break;
        default:
            atomName ="__builtin_undefined";
            break;
        }

        // Create cons term, if not done yet.
        if( !out_pAbstractTerm ) {
            out_pAbstractTerm = new vault::unify::ConsTerm( 
                atomName.c_str(), lhs, rhs );
            if( pTermDebugInfo ) {
                spWorld->setTermDebugInfo( out_pAbstractTerm, pTermDebugInfo );
            }        
        }

        return out_pAbstractTerm;
    }

    /**
     * `if( cond ) { body }` -- real if-then-else semantics (SPEC.md
     * sections 4, 9; ROADMAP Phase 2 "Cut"): like Prolog's
     * `( cond, !, body ; true )`. Desugars to a two-clause auxiliary
     * predicate appended to World's root ExecutionState:
     *
     *   __if_N( V1..Vk ) { clonedCond; __builtin_cut; clonedBody...; }
     *                                                      // then-branch,
     *                                                      // tried first
     *   __if_N( V1..Vk );                                 // fallback,
     *                                                      // tried only if
     *                                                      // cond fails
     *
     * plus a call site __if_N( origV1..origVk ) spliced into the
     * ENCLOSING goal chain in place of the `if` statement. V1..Vk are the
     * free variables of cond+body; origV1..origVk are the very same
     * VarTerm objects already in use by the enclosing clause/query
     * (m_clauseContext.m_mapSymbols), so the call site is simply part of
     * the enclosing term tree. Each synthesized clause instead gets its
     * OWN fresh variables (a fresh substitution built per clause, so the
     * two synthesized clauses do not even alias each other) via
     * cloneTermTree() -- see its declaration next to
     * collectTermTree()/deleteTermTree() in include/vault-unify.hpp. That
     * means neither synthesized clause shares a single term node with the
     * enclosing clause/query, killing the aliasing the previous
     * (broken -- see git history / ROADMAP) desugaring relied on.
     *
     * The `__builtin_cut` inserted right after clonedCond (see below) is
     * the reserved term SolveJob::performSlice() (vault-unify-solvejob.cpp)
     * recognizes directly -- once cond succeeds for the first time, it
     * commits: no further clause is tried for the __if_N(...) call site
     * (so the fallback below never runs once cond has succeeded at least
     * once) and cond itself does not backtrack for a second solution
     * either. If cond fails outright, the then-branch clause never reaches
     * the cut at all, and the fallback (always succeeds, empty body) runs
     * normally -- exactly if-then-else. This used to be a soft-if
     * (`( cond, body ; true )`, no commit, SPEC.md section 9 QUIRK) before
     * cut existed; see test/conformance/if-statement.ufy, whose first
     * query used to print the line after the `if` twice for exactly that
     * reason and now prints it once.
     */
    vault::unify::AbstractTerm* operator()( const IfStatementInput& ifStatementInput ) const
    {
        // a. Build cond and body as term trees using the ENCLOSING
        // clause/query's own variable scope (m_clauseContext), exactly
        // like any other goal built in this context -- this is what lets
        // us later tell which VarTerms are free (shared with the rest of
        // the enclosing clause/query) instead of guessing from the AST.
        // Each gets its own local pre-goal list (mirroring how
        // Context::createGoal() always hands a fresh AnyTermFactory a
        // fresh output list bound to the SAME ClauseContext), so any
        // pre-goals from a nested desugaring (e.g. '->') land next to the
        // term that needs them, not in the enclosing goal.
        std::list<vault::unify::AbstractTerm*> lsCondPreGoals;
        AnyTermFactory condFactory( m_clauseContext, lsCondPreGoals );
        vault::unify::AbstractTerm* pCondTerm = condFactory( ifStatementInput.lhs );

        std::list<vault::unify::AbstractTerm*> lsBodyTerms;
        (void) m_clauseContext.m_context.createGoal(
            m_clauseContext, ifStatementInput.rhs, lsBodyTerms );

        // All roots that make up the synthesized rule's body, in the
        // order they must run in: cond's own pre-goals, then cond itself,
        // then the body's statements (already pre-goal-inclusive, since
        // createGoal() interleaves them itself).
        std::vector<const vault::unify::AbstractTerm*> auxRuleRoots;
        auxRuleRoots.reserve( lsCondPreGoals.size() + 1 + lsBodyTerms.size() );
        {
            std::list<vault::unify::AbstractTerm*>::const_iterator it, itEnd;
            for( it = lsCondPreGoals.begin(), itEnd = lsCondPreGoals.end(); it != itEnd; ++it ) {
                auxRuleRoots.push_back( *it );
            }
        }
        auxRuleRoots.push_back( pCondTerm );
        {
            std::list<vault::unify::AbstractTerm*>::const_iterator it, itEnd;
            for( it = lsBodyTerms.begin(), itEnd = lsBodyTerms.end(); it != itEnd; ++it ) {
                auxRuleRoots.push_back( *it );
            }
        }

        // b. Collect the distinct free VarTerms across cond+body.
        std::vector<vault::unify::VarTerm*> freeVars;
        {
            std::set<const vault::unify::AbstractTerm*> visited;
            std::vector<const vault::unify::AbstractTerm*>::const_iterator it, itEnd = auxRuleRoots.end();
            for( it = auxRuleRoots.begin(); it != itEnd; ++it ) {
                collectVarTermsOrdered( *it, visited, freeVars );
            }
        }
        const int nVars = (int) freeVars.size();

        // c. Unique auxiliary predicate name (existing anon-clause
        // counter machinery, e.g. "__if__3").
        vault::unify::Atom auxAtom( m_clauseContext.nextAnonClauseName( "if" ) );

        WorldPtr spWorld( m_clauseContext.m_context.getWorld() );

        // d./e. Give each synthesized clause its OWN fresh substitution
        // (and thus its own head variables) -- the Rule and the Fact
        // clause below do not share a single VarTerm, let alone anything
        // with the enclosing clause/query.
        std::map<const vault::unify::VarTerm*, vault::unify::VarTerm*> subMapRule;
        vault::unify::AbstractTerm** ppHeadArgsRule = nVars ? new vault::unify::AbstractTerm*[nVars] : NULL;
        for( int i = 0; i < nVars; ++i ) {
            vault::unify::VarTerm* pFreshVar = new vault::unify::VarTerm();
            pFreshVar->setOriginalVarName( freeVars[i]->getOriginalVarName() );
            subMapRule[freeVars[i]] = pFreshVar;
            ppHeadArgsRule[i] = pFreshVar;
        }
        vault::unify::ConsTerm* pHeadRule = new vault::unify::ConsTerm( auxAtom, nVars, ppHeadArgsRule );
        // ConsTerm's ctor (see AnyTermFactory::operator()(ConsTermInput))
        // only copies the pointer VALUES out of ppHeadArgsRule into its own
        // m_vecTerms; it never takes ownership of the buffer itself.
        delete[] ppHeadArgsRule;

        std::map<const vault::unify::VarTerm*, vault::unify::VarTerm*> subMapFact;
        vault::unify::AbstractTerm** ppHeadArgsFact = nVars ? new vault::unify::AbstractTerm*[nVars] : NULL;
        for( int i = 0; i < nVars; ++i ) {
            vault::unify::VarTerm* pFreshVar = new vault::unify::VarTerm();
            pFreshVar->setOriginalVarName( freeVars[i]->getOriginalVarName() );
            subMapFact[freeVars[i]] = pFreshVar;
            ppHeadArgsFact[i] = pFreshVar;
        }
        vault::unify::ConsTerm* pHeadFact = new vault::unify::ConsTerm( auxAtom, nVars, ppHeadArgsFact );
        delete[] ppHeadArgsFact;

        // Clone cond's pre-goals + cond + body into the Rule clause's
        // body, via subMapRule -- shares nothing with the scratch trees
        // built in step (a), nor with the Fact clause's own clone below.
        // Right after cond's own clone, splice in a fresh `__builtin_cut`
        // term (ROADMAP Phase 2 "Cut", see the doc comment above): it
        // needs no cloning/substitution (it has no VarTerm of its own),
        // and it is a genuinely new, parse-time-allocated term reachable
        // from pRuleGoal's Goal::m_listAbstractTerms exactly like every
        // other term of this synthesized clause below -- freed by the
        // same collectTermTree()-based sweep that already owns the rest
        // of it (World::~World()), so no new ownership tracking is needed.
        std::list<const vault::unify::AbstractTerm*> lsClonedRuleBody;
        {
            std::vector<const vault::unify::AbstractTerm*>::const_iterator it, itEnd = auxRuleRoots.end();
            for( it = auxRuleRoots.begin(); it != itEnd; ++it ) {
                lsClonedRuleBody.push_back( vault::unify::cloneTermTree( *it, subMapRule ) );
                if( *it == pCondTerm ) {
                    lsClonedRuleBody.push_back( new vault::unify::ConsTerm( "__builtin_cut" ) );
                }
            }
        }
        vault::unify::Goal* pRuleGoal = new vault::unify::Goal(
            lsClonedRuleBody.begin(), lsClonedRuleBody.end() );

        Clause* pRuleClause = new vault::unify::StandardClause( pHeadRule, pRuleGoal );
        Clause* pFactClause = new vault::unify::StandardClause( pHeadFact, NULL );

        // Definition order: then-branch first, fallback second (so a
        // depth-first, all-solutions search tries "cond succeeded" before
        // "cond was skipped" -- see SPEC.md section 5 and the caveat
        // above this function).
        spWorld->getRootState()->appendClause( spWorld, pRuleClause );
        spWorld->getRootState()->appendClause( spWorld, pFactClause );
        VAULT_UNIFY_DI( ALWAYS, "Added clause '%s'.\n", pRuleClause->toString().c_str() );
        VAULT_UNIFY_DI( ALWAYS, "Added clause '%s'.\n", pFactClause->toString().c_str() );

        // The scratch cond/body trees built in step (a) (using the
        // enclosing context) have now been fully cloned into the
        // synthesized Rule clause; free their structural (non-VarTerm)
        // nodes. The VarTerms among them are NOT scratch -- they are
        // exactly `freeVars`, which the call site below reuses directly,
        // so they remain reachable from (and owned along with) the
        // enclosing clause/query's own term tree.
        {
            std::set<const vault::unify::AbstractTerm*> visited;
            std::vector<const vault::unify::AbstractTerm*>::const_iterator it, itEnd = auxRuleRoots.end();
            for( it = auxRuleRoots.begin(); it != itEnd; ++it ) {
                deleteScratchTermTree( *it, visited );
            }
        }

        // f. The call site: __if_N( origV1..origVk ), built from the
        // ORIGINAL enclosing VarTerms. This is the if-statement's own
        // contribution to the enclosing goal chain.
        //
        // g. K==0 (no free variables, as in e.g. `if( flag( on ) ) {...}`
        // with no variables anywhere in cond/body) is handled the same
        // way arity-0 calls always are in this parser (see
        // operator()(ConsTermInput) above): nVars==0, ppCallArgs==NULL,
        // ConsTerm's vector is constructed from an empty [ppTerms,
        // ppTerms+0) range -- nothing special required.
        vault::unify::AbstractTerm** ppCallArgs = nVars ? new vault::unify::AbstractTerm*[nVars] : NULL;
        for( int i = 0; i < nVars; ++i ) {
            ppCallArgs[i] = freeVars[i];
        }
        vault::unify::ConsTerm* pCallTerm = new vault::unify::ConsTerm( auxAtom, nVars, ppCallArgs );
        delete[] ppCallArgs;

        // h. Deliberately not registering any TermDebugInfo for the
        // synthesized clauses/call site -- the old code didn't either,
        // and doing so here would mean sharing one TermDebugInfo across
        // several term keys again (World::~World() already has to guard
        // against exactly that for the '->' desugaring).
        return pCallTerm;
    }


    /**
     * `<a>..<b>` (ROADMAP: language owner request "for/foreach + ranges",
     * 2026-08-21). No operator present -> just the (unchanged) lhs, exactly
     * like every other precedence level's single-operand fast path
     * (ArithTermInput/CompareTermInput above). Otherwise: if BOTH bounds,
     * once built, turn out to be literal 0-arity ConsTerm atoms whose text
     * parses fully as an int64 (parseRangeLiteralInt64() above), the range
     * is expanded EAGERLY into a plain ArrayTerm literal, right here at
     * parse time -- indistinguishable from writing the array out by hand
     * (SPEC.md's "simplest honest rule": a literal-bounds range IS an array
     * literal, nothing more). The two scratch bound terms just built are
     * then immediately redundant and are deleted directly (mirroring the
     * orphaned-wrapper-ConsTerm deletion in the findall desugar above --
     * same reasoning: never reachable from anywhere collectAllTermTrees()
     * walks, so leaving them would leak, and any TermDebugInfo map entry
     * for them becomes a harmless dangling KEY, never dereferenced by
     * World::~World()'s de-duplicating sweep over map VALUES).
     *
     * Otherwise (either bound is a variable, or itself an arithmetic
     * expression) this is a genuinely dynamic range: a fresh anonymous var
     * is substituted for it (mirroring '->' 's own pre-goal pattern,
     * operator()(const InfixTermsInput&) above) and a
     * `__builtin_range(lhs, rhs, freshVar)` pre-goal is pushed onto
     * m_lsTerms, resolved at SOLVE time by RangeBuiltinClause
     * (vault-unify-clause-builtin-arith.cpp). Because this level sits below
     * every other precedence level (comparison, '='), a dynamic range is
     * usable anywhere any other AnyTerm is -- not only inside foreach's
     * header.
     */
    vault::unify::AbstractTerm* operator()( const RangeTermInput& rangeTermInput ) const
    {
        vault::unify::AbstractTerm* lhs = (*this)( rangeTermInput.lhs );

        if( !rangeTermInput.rhs ) {
            return lhs;
        }
        vault::unify::AbstractTerm* rhs = (*this)( rangeTermInput.rhs.get().second );

        const vault::unify::ConsTerm* pLhsCons = dynamic_cast<const vault::unify::ConsTerm*>( lhs );
        const vault::unify::ConsTerm* pRhsCons = dynamic_cast<const vault::unify::ConsTerm*>( rhs );
        int64_t a = 0, b = 0;
        bool lhsLiteral = pLhsCons && 0==pLhsCons->getArity()
            && parseRangeLiteralInt64( pLhsCons->getName().value(), a );
        bool rhsLiteral = pRhsCons && 0==pRhsCons->getArity()
            && parseRangeLiteralInt64( pRhsCons->getName().value(), b );

        if( lhsLiteral && rhsLiteral ) {
            delete lhs;
            delete rhs;

            int64_t count = (b>=a) ? (b - a + 1) : 0;
            if( count > RANGE_MAX_ELEMENTS ) {
                VAULT_UNIFY_DI( ALWAYS, "Range %lld..%lld exceeds the %lld-element cap; truncating.\n",
                    (long long) a, (long long) b, (long long) RANGE_MAX_ELEMENTS );
                count = RANGE_MAX_ELEMENTS;
            }
            vault::unify::AbstractTerm** ppTerms = count ? new vault::unify::AbstractTerm*[(size_t) count] : NULL;
            for( int64_t i = 0; i < count; ++i ) {
                char buf[32];
                snprintf( buf, sizeof(buf), "%lld", (long long)(a + i) );
                ppTerms[i] = new vault::unify::ConsTerm( buf );
            }
            vault::unify::ArrayTerm* pArrayTerm = new vault::unify::ArrayTerm( ppTerms, (int) count );
            delete[] ppTerms;
            return pArrayTerm;
        }

        vault::unify::VarTerm* pVarTerm1 = new vault::unify::VarTerm();
        std::string strVarTerm1 = m_clauseContext.nextAnonVarName();
        pVarTerm1->setOriginalVarName( strVarTerm1 );
        m_clauseContext.m_mapSymbols[strVarTerm1] = pVarTerm1;

        vault::unify::ConsTerm* pPreGoal = new vault::unify::ConsTerm(
            "__builtin_range", lhs, rhs, pVarTerm1 );
        m_lsTerms.push_back( pPreGoal );

        return pVarTerm1;
    }


    /**
     * `foreach ( $x : arrExpr ) { body }` (ROADMAP: language owner request
     * "classic for/foreach loops", 2026-08-21). Mirrors the `if` desugaring
     * above closely: any term spliced into a clause permanently appended to
     * World's root ExecutionState must not alias a VarTerm with the
     * enclosing, per-query/-clause-lifetime term tree (see World::~World()/
     * ~SolveJob()'s comments), so every synthesized clause below gets its
     * own fresh substitution via cloneTermTree(), exactly like `if`'s
     * rule/fact pair.
     *
     * Two mutually-recursive auxiliary predicates are synthesized (unique
     * names via ClauseContext::nextAnonClauseName(), e.g. "__fe__3"/
     * "__feb__3"):
     *
     *   __fe__3( $arr, $i, $xSlot, V1..Vm ) {   // main loop
     *       __builtin_array_at( $arr, $i, $xSlot ); // fails -> index OOB
     *       __feb__3( $xSlot, V1..Vm );              // body-or-true, below
     *       cut;                                     // commit THIS iter
     *       $j = $i + 1;
     *       $arrNext = $arr; $v1Next = V1; ...; $vmNext = Vm; // rebind, see below
     *       __fe__3( $arrNext, $j, $freshSlot, $v1Next..$vmNext ); // next iter
     *   }
     *   __fe__3( $arr, $i, $xSlot, V1..Vm );    // fallback: index OOB -> end
     *
     *   __feb__3( $x, V1..Vm ) { clonedBody...; cut; } // body-or-true, rule
     *   __feb__3( $x, V1..Vm );                         // body-or-true, fallback
     *
     * `freeVars` = the free VarTerms of ($x, body), with $x (the loop
     * variable) FORCE-INCLUDED as freeVars[0] even if body never mentions
     * it (so `freeVars[0] == $x` always, by construction -- the same
     * "force-include as the first collection root" trick `for` uses for its
     * own control variable below); V1..Vm = freeVars[1..] (every OTHER free
     * variable). $arr/$i are BRAND NEW synthesized parameters (never part
     * of the enclosing scope); $xSlot occupies the loop variable's own
     * position (position 2) so the call site below can legitimately pass
     * the ORIGINAL enclosing $x there (keeping it reachable/owned -- see
     * the OWNERSHIP NOTE further down; an EARLIER draft excluded $x from
     * __fe__3's parameters entirely, which leaked it).
     *
     * TWO THINGS MUST NEVER BE THREADED UNCHANGED THROUGH THE RECURSIVE
     * CALL (both caught by hand-tracing against VarTerm unification,
     * section 6, before this ever reached CI):
     *
     * (1) $xSlot itself -- it is rebound to a DIFFERENT array element every
     * iteration by `__builtin_array_at`, so the recursive call passes a
     * BRAND NEW, never-bound `$freshSlot` for that position instead (an
     * earlier draft reused $xSlot directly there, which broke the loop
     * after its first element: the next invocation's own $xSlot would
     * already be bound to THIS element, so its own `__builtin_array_at`
     * would then try to bind an already-bound variable to the NEXT
     * element and fail outright).
     *
     * (2) $arr and V1..Vm -- even though these genuinely ARE invariant
     * (the same value every iteration, unlike $xSlot), they are STILL not
     * passed as the literal same head-parameter object again. Doing so
     * would rely on `VarTerm::unifyVarTerm`'s `this==pOther` identical-
     * object fast path (vault-unify-term-var.cpp) -- which returns
     * `UnifyLast` immediately WITHOUT recording any `AssignmentId`
     * binding at all, since both sides are literally the same pointer.
     * Whether that is actually safe several recursion levels down (i.e.
     * whether some OTHER mechanism still makes the value visible to a
     * deeper invocation) was not a risk worth taking here: `$arrNext`/
     * `$v1Next`../`$vmNext` are BRAND NEW variables, and `$arrNext = $arr`
     * etc. (an ordinary `unify(...)` goal -- see AnyTermFactory::
     * operator()(const InfixTermsInput&)'s `=` case, section 4) forces a
     * genuine, ordinary variable-to-variable binding through the
     * established `UnifyBuiltinClause` path for every iteration instead,
     * which this task's investigation independently confirmed correct
     * (a fresh var linked to an existing one always succeeds and is
     * resolvable from any descendant context, regardless of any subtlety
     * of same-object reuse) -- see this task's session report.
     *
     * SEMANTICS DECISION (documented in SPEC.md, per this task's brief): a
     * body failure for one element does NOT stop the loop -- it fails that
     * one iteration silently and the loop CONTINUES (the recommended
     * default, matching the language's overall silent-failure character).
     * This is exactly what `__feb__3` buys: WITHOUT it (i.e. if body were
     * inlined directly into `__fe__3`'s own rule-clause), a failing body
     * would fail `__fe__3`'s rule-clause candidate outright -- no
     * continuation would ever reach the recursive call -- backtracking
     * straight past it to `__fe__3`'s OWN fallback fact, i.e. STOPPING the
     * loop rather than continuing it (indistinguishable from "index out of
     * bounds"). `__feb__3`'s own fallback fact absorbs exactly that
     * failure (always succeeds trivially when its rule-clause -- i.e. body
     * -- has no solution at all), so `__fe__3`'s rule-clause always reaches
     * `cut` and the recursive step regardless of whether body succeeded.
     *
     * CUT-INTERACTION ANALYSIS (verified against SolveJob::performSlice(),
     * vault-unify-solvejob.cpp -- see also this task's report for the full
     * hand-trace): the `cut` right after the `__feb__3` call commits
     * `__fe__3`'s OWN clause choice for THIS call (rule vs. fallback) and
     * every choice point to its LEFT within this one activation -- which
     * includes pruning `body`'s own remaining alternatives (via `__feb__3`'s
     * choice, still on the stack at that point) down to its first solution,
     * standard cut semantics (section 8). Crucially it does NOT reach the
     * recursive `__fe__3(...)` call written a few lines later: that call has
     * not been pushed onto the SolveContext stack yet when this cut runs
     * (`performSlice()` only ever invalidates `m_itNextChildClause` on
     * contexts ALREADY on the stack, walking from the top down to and
     * including THIS activation's own entry context) -- so the recursive
     * call gets its own, entirely unaffected, fresh entry context and fresh
     * choice points once its turn comes. This is why the recursion is not
     * itself pruned/truncated by this cut.
     *
     * `arrExpr` is evaluated exactly ONCE, in the ENCLOSING scope, via
     * `this` factory directly -- any of ITS OWN pre-goals (e.g. a nested
     * `->`, or a variable-bounds range's `__builtin_range` pre-goal) land in
     * the enclosing goal chain (m_lsTerms) exactly once, before the loop
     * starts. This is different from `if`'s `cond`, which is deliberately
     * rebuilt/cloned fresh into the synthesized clause since it must
     * re-run every iteration -- `arrExpr` is a single, fixed value for the
     * whole loop, evaluated once up front, exactly like a classic
     * for-each's collection expression.
     */
    vault::unify::AbstractTerm* operator()( const ForeachStatementInput& foreachStatementInput ) const
    {
        // a. arrExpr, evaluated ONCE in the enclosing scope.
        vault::unify::AbstractTerm* pArrTerm = (*this)( foreachStatementInput.arrExpr );

        // b. Loop variable, same enclosing scope. Documented assumption
        // (SPEC.md): this is a `$name` variable; if not (not enforced by
        // the grammar), fall back to a fresh, unbound variable rather than
        // crash -- an unsupported, never-exercised shape.
        vault::unify::AbstractTerm* pLoopVarRaw = (*this)( foreachStatementInput.loopVar );
        vault::unify::VarTerm* pLoopVar = dynamic_cast<vault::unify::VarTerm*>( pLoopVarRaw );
        if( !pLoopVar ) {
            VAULT_UNIFY_DI( ALWAYS, "foreach: loop-variable position \"%s\" is not a $-variable; using a fresh variable instead.\n",
                pLoopVarRaw->toString().c_str() );
            pLoopVar = new vault::unify::VarTerm();
        }

        // c. Body, built in the enclosing scope (pre-goal-inclusive, via
        // createGoal() exactly like `if`'s body).
        std::list<vault::unify::AbstractTerm*> lsBodyTerms;
        (void) m_clauseContext.m_context.createGoal(
            m_clauseContext, foreachStatementInput.body, lsBodyTerms );

        // d. Free variables: the loop variable is force-included (an extra
        // collection root) even if body never mentions it; then every
        // VarTerm actually occurring in body.
        std::vector<const vault::unify::AbstractTerm*> varRoots;
        varRoots.push_back( pLoopVar );
        {
            std::list<vault::unify::AbstractTerm*>::const_iterator it, itEnd = lsBodyTerms.end();
            for( it = lsBodyTerms.begin(); it != itEnd; ++it ) {
                varRoots.push_back( *it );
            }
        }
        std::vector<vault::unify::VarTerm*> freeVars;
        {
            std::set<const vault::unify::AbstractTerm*> visited;
            std::vector<const vault::unify::AbstractTerm*>::const_iterator it, itEnd = varRoots.end();
            for( it = varRoots.begin(); it != itEnd; ++it ) {
                collectVarTermsOrdered( *it, visited, freeVars );
            }
        }
        const int nVars = (int) freeVars.size();

        WorldPtr spWorld( m_clauseContext.m_context.getWorld() );
        vault::unify::Atom feAtom( m_clauseContext.nextAnonClauseName( "fe" ) );
        vault::unify::Atom febAtom( m_clauseContext.nextAnonClauseName( "feb" ) );

        // e. __feb__N (body-or-true): built FIRST since __fe__N's own
        // rule-clause body calls it. Rule clause: clonedBody; cut. Fallback
        // fact: empty body, always succeeds (absorbs a failing body).
        std::map<const vault::unify::VarTerm*, vault::unify::VarTerm*> subMapFebRule;
        vault::unify::AbstractTerm** ppFebRuleArgs = nVars ? new vault::unify::AbstractTerm*[nVars] : NULL;
        for( int i = 0; i < nVars; ++i ) {
            vault::unify::VarTerm* pFresh = new vault::unify::VarTerm();
            pFresh->setOriginalVarName( freeVars[i]->getOriginalVarName() );
            subMapFebRule[freeVars[i]] = pFresh;
            ppFebRuleArgs[i] = pFresh;
        }
        vault::unify::ConsTerm* pFebHeadRule = new vault::unify::ConsTerm( febAtom, nVars, ppFebRuleArgs );
        delete[] ppFebRuleArgs;

        std::list<const vault::unify::AbstractTerm*> lsFebRuleBody;
        {
            std::list<vault::unify::AbstractTerm*>::const_iterator it, itEnd = lsBodyTerms.end();
            for( it = lsBodyTerms.begin(); it != itEnd; ++it ) {
                lsFebRuleBody.push_back( vault::unify::cloneTermTree( *it, subMapFebRule ) );
            }
        }
        lsFebRuleBody.push_back( new vault::unify::ConsTerm( "__builtin_cut" ) );
        vault::unify::Goal* pFebRuleGoal = new vault::unify::Goal( lsFebRuleBody.begin(), lsFebRuleBody.end() );
        Clause* pFebRuleClause = new vault::unify::StandardClause( pFebHeadRule, pFebRuleGoal );

        vault::unify::AbstractTerm** ppFebFactArgs = nVars ? new vault::unify::AbstractTerm*[nVars] : NULL;
        for( int i = 0; i < nVars; ++i ) {
            vault::unify::VarTerm* pFresh = new vault::unify::VarTerm();
            pFresh->setOriginalVarName( freeVars[i]->getOriginalVarName() );
            ppFebFactArgs[i] = pFresh;
        }
        vault::unify::ConsTerm* pFebHeadFact = new vault::unify::ConsTerm( febAtom, nVars, ppFebFactArgs );
        delete[] ppFebFactArgs;
        Clause* pFebFactClause = new vault::unify::StandardClause( pFebHeadFact, NULL );

        spWorld->getRootState()->appendClause( spWorld, pFebRuleClause );
        spWorld->getRootState()->appendClause( spWorld, pFebFactClause );

        // f. __fe__N (main loop). $arr/$i are brand-new synthesized
        // parameters (positions 0/1); the loop variable itself occupies
        // position 2 (so the call site below can legitimately pass the
        // ORIGINAL enclosing pLoopVar there -- see the ownership note
        // just below); outerVars (freeVars[1..nVars-1]) follow after it.
        //
        // OWNERSHIP NOTE: pLoopVar (freeVars[0]) must remain reachable
        // from SOMEWHERE this factory's caller ultimately keeps alive (the
        // enclosing query/clause's own term tree), or it leaks -- exactly
        // like every other VarTerm this parser ever allocates (see
        // World::~World()/~SolveJob()'s ownership comments). The call
        // site below (`ppCallArgs[2] = pLoopVar`) is what keeps it
        // reachable.
        //
        // RECURSION NOTE (the actual bug this design avoids -- an earlier
        // draft shipped it, then a hand-trace against VarTerm unification,
        // section 6, caught it before this ever reached CI): a value that
        // must change every iteration (the loop variable, rebound to a
        // NEW array element each time by `__builtin_array_at`) must NEVER
        // be threaded UNCHANGED through a recursive call the way an
        // invariant argument (outerVars, `$arr` itself) legitimately is --
        // doing so would hand the NEXT invocation's own head parameter an
        // ALREADY-bound value (this iteration's element), so that
        // invocation's OWN `__builtin_array_at` would then try to bind an
        // ALREADY-bound variable to the NEXT element (e.g. unifying "a"
        // against "b"), which fails outright and breaks the loop after its
        // first element. So: the call site passes pLoopVar (unbound) for
        // position 2 ONLY at the very first call; the RECURSIVE call
        // instead passes a BRAND NEW, never-bound placeholder for that
        // SAME position every time -- never the current iteration's own
        // (by-then-bound) copy. Each invocation's own head-parameter copy
        // for position 2 is therefore always freshly unbound when
        // `__builtin_array_at` reaches it, exactly like `for`'s `$i -> $i2`
        // step (section 12.2) achieves the same thing for its own
        // per-iteration-changing value.
        const int mVars = nVars - 1; // outerVars count (nVars >= 1: pLoopVar itself).

        std::map<const vault::unify::VarTerm*, vault::unify::VarTerm*> subMapFeRule;
        vault::unify::VarTerm* pArrParamRule = new vault::unify::VarTerm();
        vault::unify::VarTerm* pIdxParamRule = new vault::unify::VarTerm();
        vault::unify::VarTerm* pXSlotRule = new vault::unify::VarTerm();
        vault::unify::AbstractTerm** ppFeRuleArgs = new vault::unify::AbstractTerm*[3 + mVars];
        ppFeRuleArgs[0] = pArrParamRule;
        ppFeRuleArgs[1] = pIdxParamRule;
        ppFeRuleArgs[2] = pXSlotRule;
        for( int i = 0; i < mVars; ++i ) {
            vault::unify::VarTerm* pFresh = new vault::unify::VarTerm();
            pFresh->setOriginalVarName( freeVars[1 + i]->getOriginalVarName() );
            subMapFeRule[freeVars[1 + i]] = pFresh;
            ppFeRuleArgs[3 + i] = pFresh;
        }
        vault::unify::ConsTerm* pFeHeadRule = new vault::unify::ConsTerm( feAtom, 3 + mVars, ppFeRuleArgs );
        delete[] ppFeRuleArgs;

        std::list<const vault::unify::AbstractTerm*> lsFeRuleBody;

        // __builtin_array_at( $arr, $i, $xSlot )
        lsFeRuleBody.push_back( new vault::unify::ConsTerm(
            "__builtin_array_at", pArrParamRule, pIdxParamRule, pXSlotRule ) );

        // __feb__N( $xSlot, freshOuterVars... ) -- position 0 is this
        // iteration's element (from $xSlot, just bound above); freeVars[1..]
        // map to their fresh subMapFeRule slot, same order __feb__N's own
        // head expects.
        vault::unify::AbstractTerm** ppFebCallArgs = new vault::unify::AbstractTerm*[nVars];
        ppFebCallArgs[0] = pXSlotRule;
        for( int i = 0; i < mVars; ++i ) {
            ppFebCallArgs[1 + i] = subMapFeRule[freeVars[1 + i]];
        }
        lsFeRuleBody.push_back( new vault::unify::ConsTerm( febAtom, nVars, ppFebCallArgs ) );
        delete[] ppFebCallArgs;

        lsFeRuleBody.push_back( new vault::unify::ConsTerm( "__builtin_cut" ) );

        // $j = $i + 1
        vault::unify::VarTerm* pFreshJ = new vault::unify::VarTerm();
        pFreshJ->setOriginalVarName( m_clauseContext.nextAnonVarName() );
        vault::unify::ConsTerm* pOneAtom = new vault::unify::ConsTerm( "1" );
        vault::unify::ConsTerm* pPlusAtom = new vault::unify::ConsTerm( "+" );
        vault::unify::ConsTerm* pArithNext = new vault::unify::ConsTerm(
            "__builtin_arith", pPlusAtom, pIdxParamRule, pOneAtom );
        lsFeRuleBody.push_back( new vault::unify::ConsTerm( "__builtin_eval", pArithNext, pFreshJ ) );

        // Explicit rebinds before the recursive call: `unify($fresh, $cur)`
        // for `$arr` and every outerVar. This is deliberately NOT just
        // "pass the same head-parameter object again unchanged" (the
        // ordinary idiom an ordinary user-written recursive predicate's
        // OWN parser output already relies on) -- seeing every OTHER
        // synthesized value here (the loop-var slot, the index) needed a
        // genuinely fresh variable to cross a recursive call safely, this
        // desugaring plays it safe for `$arr`/outerVars too, rather than
        // rely on this engine's `VarTerm::unifyVarTerm`'s `this==pOther`
        // identical-object fast path (which records no `AssignmentId`
        // binding at all -- see vault-unify-term-var.cpp) to somehow still
        // make the value visible several recursion levels down. A plain
        // `unify(fresh, cur)` goal, run once per iteration, forces a real,
        // ordinary (non-identity) variable-to-variable binding through the
        // established `UnifyBuiltinClause` path instead -- correctness here
        // does not depend on any subtler property of how same-object head
        // parameters behave across recursive calls.
        vault::unify::VarTerm* pArrNext = new vault::unify::VarTerm();
        lsFeRuleBody.push_back( new vault::unify::ConsTerm( "unify", pArrNext, pArrParamRule ) );
        std::vector<vault::unify::VarTerm*> outerNext( mVars );
        for( int i = 0; i < mVars; ++i ) {
            outerNext[i] = new vault::unify::VarTerm();
            lsFeRuleBody.push_back( new vault::unify::ConsTerm(
                "unify", outerNext[i], subMapFeRule[freeVars[1 + i]] ) );
        }

        // __fe__N( $arrNext, $j, freshPlaceholder, outerNext... ) --
        // freshPlaceholder is a BRAND NEW, never-bound var (see the
        // RECURSION NOTE above): never $xSlot's own (by-now-bound) copy.
        vault::unify::VarTerm* pFreshPlaceholder = new vault::unify::VarTerm();
        vault::unify::AbstractTerm** ppFeRecurseArgs = new vault::unify::AbstractTerm*[3 + mVars];
        ppFeRecurseArgs[0] = pArrNext;
        ppFeRecurseArgs[1] = pFreshJ;
        ppFeRecurseArgs[2] = pFreshPlaceholder;
        for( int i = 0; i < mVars; ++i ) {
            ppFeRecurseArgs[3 + i] = outerNext[i];
        }
        lsFeRuleBody.push_back( new vault::unify::ConsTerm( feAtom, 3 + mVars, ppFeRecurseArgs ) );
        delete[] ppFeRecurseArgs;

        vault::unify::Goal* pFeRuleGoal = new vault::unify::Goal( lsFeRuleBody.begin(), lsFeRuleBody.end() );
        Clause* pFeRuleClause = new vault::unify::StandardClause( pFeHeadRule, pFeRuleGoal );

        vault::unify::AbstractTerm** ppFeFactArgs = new vault::unify::AbstractTerm*[3 + mVars];
        ppFeFactArgs[0] = new vault::unify::VarTerm();
        ppFeFactArgs[1] = new vault::unify::VarTerm();
        ppFeFactArgs[2] = new vault::unify::VarTerm();
        for( int i = 0; i < mVars; ++i ) {
            vault::unify::VarTerm* pFresh = new vault::unify::VarTerm();
            pFresh->setOriginalVarName( freeVars[1 + i]->getOriginalVarName() );
            ppFeFactArgs[3 + i] = pFresh;
        }
        vault::unify::ConsTerm* pFeHeadFact = new vault::unify::ConsTerm( feAtom, 3 + mVars, ppFeFactArgs );
        delete[] ppFeFactArgs;
        Clause* pFeFactClause = new vault::unify::StandardClause( pFeHeadFact, NULL );

        spWorld->getRootState()->appendClause( spWorld, pFeRuleClause );
        spWorld->getRootState()->appendClause( spWorld, pFeFactClause );

        // g. body's scratch term trees have now been fully cloned into
        // __feb__N's rule clause; free their structural nodes (VarTerms
        // among them ARE freeVars, reused directly at the call site below,
        // so leave those alone -- see deleteScratchTermTree()'s comment).
        {
            std::set<const vault::unify::AbstractTerm*> visited;
            std::list<vault::unify::AbstractTerm*>::const_iterator it, itEnd = lsBodyTerms.end();
            for( it = lsBodyTerms.begin(); it != itEnd; ++it ) {
                deleteScratchTermTree( *it, visited );
            }
        }

        // h. Call site: __fe__N( arrTerm, 0, pLoopVar, origOuterVars... ),
        // spliced into the enclosing goal chain in place of the foreach
        // statement -- pLoopVar (freeVars[0], the ORIGINAL enclosing
        // VarTerm) is reused directly here, exactly like every other
        // freeVars entry, keeping it reachable/owned (see the OWNERSHIP
        // NOTE above); it starts this one call unbound, same as any other
        // fresh loop variable.
        vault::unify::ConsTerm* pZeroAtom = new vault::unify::ConsTerm( "0" );
        vault::unify::AbstractTerm** ppCallArgs = new vault::unify::AbstractTerm*[3 + mVars];
        ppCallArgs[0] = pArrTerm;
        ppCallArgs[1] = pZeroAtom;
        ppCallArgs[2] = pLoopVar;
        for( int i = 0; i < mVars; ++i ) {
            ppCallArgs[3 + i] = freeVars[1 + i];
        }
        vault::unify::ConsTerm* pCallTerm = new vault::unify::ConsTerm( feAtom, 3 + mVars, ppCallArgs );
        delete[] ppCallArgs;

        return pCallTerm;
    }


    /**
     * `for ( $i = init; cond; $i = step ) { body }` (ROADMAP: language owner
     * request "classic for/foreach loops", 2026-08-21). Same ownership
     * discipline as `if`/`foreach` above (fresh cloneTermTree() substitution
     * per synthesized clause), but simpler: only ONE synthesized auxiliary
     * predicate is needed (no `feb`-style body-or-true wrapper -- body/cond
     * failure STOPS the loop here, unlike `foreach`; SPEC.md documents both
     * loop constructs' choice side by side):
     *
     *   __for__3( $i, V1..Vk ) {
     *       clonedCond;
     *       clonedBody...;
     *       cut;                       // commit THIS iteration
     *       $i2 = clonedStep;          // fresh $i2, NOT one of V1..Vk
     *       $v1Next = V1; ...; $vkNext = Vk;  // rebind, see below
     *       __for__3( $i2, $v1Next..$vkNext );
     *   }
     *   __for__3( $i, V1..Vk );         // fallback: cond false -> loop ends
     *                                    // (also reached, via ordinary
     *                                    // failure+backtracking, if cond
     *                                    // or body ever fails)
     *
     * V1..Vk (every captured variable OTHER than $i) are genuinely
     * invariant across the whole recursion, but are still NOT passed to
     * the recursive call as the literal same head-parameter object again
     * -- doing so would rely on `VarTerm::unifyVarTerm`'s `this==pOther`
     * identical-object fast path (vault-unify-term-var.cpp), which
     * records no `AssignmentId` binding at all, to still make the value
     * visible several recursion levels down. `$v1Next = V1;` etc. (an
     * ordinary `unify(...)` goal per variable, run once per iteration)
     * forces a ordinary variable-to-variable binding through
     * `UnifyBuiltinClause` instead -- see `foreach`'s own identical
     * treatment of `$arr`/its own V1..Vm, section 12.3, for the full
     * reasoning (this task's session report has the investigation).
     *
     * The `cut` right after clonedCond+clonedBody mirrors `foreach`'s own
     * `__fe__N` cut placement (right after the goals that must succeed for
     * this iteration to "count", before the step+recursive call), for the
     * SAME two reasons: (a) WITHOUT it, once the loop eventually ends and
     * the rest of the enclosing query runs to completion, this engine's
     * exhaustive all-solutions backtracking would ALSO retry THIS call's
     * own fallback fact as a sibling alternative -- once per iteration
     * already passed through -- printing everything after the loop once
     * per iteration instead of once total (the exact "soft-if" duplicate-
     * output bug section 8/10 document for the pre-cut `if`); (b) it also
     * commits `for` to cond/body's FIRST solution each iteration, matching
     * `if`'s own commit-to-first-solution semantics, rather than letting a
     * non-deterministic cond/body multiply the recursion. Crucially this
     * does NOT weaken "body failure stops the loop": cut is only reached
     * once cond+body have ALREADY succeeded for this call -- a failing
     * body never reaches it, and the whole rule-clause candidate fails
     * exactly as it would without the cut, falling back to the fallback
     * fact and ending the loop.
     *
     * $i is the for-header's own control variable (`initAssign`'s LHS),
     * force-included as the FIRST var-collection root -- guaranteeing
     * freeVars[0]==$i by construction (collectVarTermsOrdered's dedup means
     * a later reference to the same VarTerm, e.g. from cond, is not added
     * again) -- so it always has a head-parameter slot, and so the
     * recursive call can unambiguously replace exactly that ONE slot with
     * $i2 while carrying every other captured variable through via its
     * own rebind (see below).
     *
     * The step value is handled specially: `stepAssign` as WRITTEN
     * ("$i = $i + 1") would, if cloned verbatim like cond/body, produce
     * `__builtin_eval(__builtin_arith("+", freshI, "1"), freshI)` -- binding
     * freshI (already bound, from this call's own head-parameter
     * unification, to THIS iteration's value V) to V+1 AGAIN, which never
     * unifies (V+1 != V). Instead, only the step's RHS EXPRESSION
     * (`stepAssign.rhs.rhs.second`, e.g. just "$i + 1") is built and cloned
     * via the SAME per-clause substitution as cond/body (so any "$i" within
     * it correctly reads the CURRENT iteration's fresh value), and its
     * result is assigned to a BRAND NEW fresh local $i2 (never one of
     * V1..Vk) -- exactly mirroring `foreach`'s `$j = $i + 1` step, and
     * sidestepping the double-occurrence problem entirely.
     */
    vault::unify::AbstractTerm* operator()( const ForStatementInput& forStatementInput ) const
    {
        // a. The control variable, from the init assignment's LHS, built in
        // the enclosing scope (same object as every other reference to the
        // same name elsewhere in this clause/query). Documented assumption
        // (SPEC.md): a `$name` variable; not enforced by the grammar.
        vault::unify::AbstractTerm* pForVarRaw = (*this)( forStatementInput.initAssign.rhs.atilhs );
        vault::unify::VarTerm* pForVar = dynamic_cast<vault::unify::VarTerm*>( pForVarRaw );
        if( !pForVar ) {
            VAULT_UNIFY_DI( ALWAYS, "for: init clause's left-hand side \"%s\" is not a $-variable; using a fresh variable instead.\n",
                pForVarRaw->toString().c_str() );
            pForVar = new vault::unify::VarTerm();
        }

        // b. The init assignment itself ($i = E0), run ONCE in the
        // enclosing scope, right before the loop starts (any pre-goals, and
        // the assignment goal itself, land in m_lsTerms via `this` factory).
        vault::unify::AbstractTerm* pInitGoal = (*this)( forStatementInput.initAssign );
        m_lsTerms.push_back( pInitGoal );

        // c. cond/body/step, all built ONCE in the enclosing scope -- cond
        // and step get their own scratch pre-goal lists (mirroring `if`'s
        // cond), since they are cloned into, and re-run fresh by, EVERY
        // iteration of the synthesized clause; body reuses createGoal()
        // exactly like `if`'s body.
        std::list<vault::unify::AbstractTerm*> lsCondPreGoals;
        AnyTermFactory condFactory( m_clauseContext, lsCondPreGoals );
        vault::unify::AbstractTerm* pCondTerm = condFactory( forStatementInput.condGoal );

        std::list<vault::unify::AbstractTerm*> lsBodyTerms;
        (void) m_clauseContext.m_context.createGoal(
            m_clauseContext, forStatementInput.body, lsBodyTerms );

        std::list<vault::unify::AbstractTerm*> lsStepPreGoals;
        AnyTermFactory stepFactory( m_clauseContext, lsStepPreGoals );
        vault::unify::AbstractTerm* pStepValueTerm = forStatementInput.stepAssign.rhs.rhs
            ? stepFactory( forStatementInput.stepAssign.rhs.rhs.get().second )
            // Defensive fallback if the step clause has no top-level '='
            // (malformed for-header; not exercised by any test) -- treat
            // the whole clause as the "next value" expression directly
            // rather than crash.
            : stepFactory( forStatementInput.stepAssign );

        // d. auxRuleRoots: cond+body, in order -- cloned-and-emitted
        // verbatim as this synthesized clause's own goal statements (step
        // is handled specially below, never emitted verbatim).
        std::vector<const vault::unify::AbstractTerm*> auxRuleRoots;
        {
            std::list<vault::unify::AbstractTerm*>::const_iterator it, itEnd;
            for( it = lsCondPreGoals.begin(), itEnd = lsCondPreGoals.end(); it != itEnd; ++it ) {
                auxRuleRoots.push_back( *it );
            }
        }
        auxRuleRoots.push_back( pCondTerm );
        {
            std::list<vault::unify::AbstractTerm*>::const_iterator it, itEnd;
            for( it = lsBodyTerms.begin(), itEnd = lsBodyTerms.end(); it != itEnd; ++it ) {
                auxRuleRoots.push_back( *it );
            }
        }

        // Free variables: pForVar is force-included FIRST (guaranteeing
        // freeVars[0]==pForVar), then everything reachable from
        // auxRuleRoots (cond+body) and from step's own pre-goals/value.
        std::vector<const vault::unify::AbstractTerm*> varRoots;
        varRoots.push_back( pForVar );
        {
            std::vector<const vault::unify::AbstractTerm*>::const_iterator it, itEnd = auxRuleRoots.end();
            for( it = auxRuleRoots.begin(); it != itEnd; ++it ) {
                varRoots.push_back( *it );
            }
        }
        {
            std::list<vault::unify::AbstractTerm*>::const_iterator it, itEnd;
            for( it = lsStepPreGoals.begin(), itEnd = lsStepPreGoals.end(); it != itEnd; ++it ) {
                varRoots.push_back( *it );
            }
        }
        varRoots.push_back( pStepValueTerm );

        std::vector<vault::unify::VarTerm*> freeVars; // freeVars[0] == pForVar, by construction.
        {
            std::set<const vault::unify::AbstractTerm*> visited;
            std::vector<const vault::unify::AbstractTerm*>::const_iterator it, itEnd = varRoots.end();
            for( it = varRoots.begin(); it != itEnd; ++it ) {
                collectVarTermsOrdered( *it, visited, freeVars );
            }
        }
        const int nVars = (int) freeVars.size(); // always >= 1 (pForVar itself)

        vault::unify::Atom forAtom( m_clauseContext.nextAnonClauseName( "for" ) );
        WorldPtr spWorld( m_clauseContext.m_context.getWorld() );

        // e. Rule clause: its own fresh substitution.
        std::map<const vault::unify::VarTerm*, vault::unify::VarTerm*> subMapRule;
        vault::unify::AbstractTerm** ppHeadArgsRule = new vault::unify::AbstractTerm*[nVars];
        for( int i = 0; i < nVars; ++i ) {
            vault::unify::VarTerm* pFresh = new vault::unify::VarTerm();
            pFresh->setOriginalVarName( freeVars[i]->getOriginalVarName() );
            subMapRule[freeVars[i]] = pFresh;
            ppHeadArgsRule[i] = pFresh;
        }
        vault::unify::ConsTerm* pHeadRule = new vault::unify::ConsTerm( forAtom, nVars, ppHeadArgsRule );
        delete[] ppHeadArgsRule;

        std::list<const vault::unify::AbstractTerm*> lsClonedRuleBody;
        {
            std::vector<const vault::unify::AbstractTerm*>::const_iterator it, itEnd = auxRuleRoots.end();
            for( it = auxRuleRoots.begin(); it != itEnd; ++it ) {
                lsClonedRuleBody.push_back( vault::unify::cloneTermTree( *it, subMapRule ) );
            }
        }

        // Commit THIS iteration once cond+body have both succeeded --
        // mirrors `foreach`'s own `__fe__N` cut placement exactly (right
        // after the goals that must succeed for this iteration to
        // "count", before the step+recursive call), and for the SAME two
        // reasons: (a) WITHOUT it, after the loop eventually ends (however
        // many iterations later) and the rest of the enclosing query runs
        // to completion, this engine's exhaustive backtracking search would
        // ALSO retry THIS call's own fallback fact as a sibling alternative
        // -- once per iteration already passed through -- printing
        // everything after the `for` loop once per iteration instead of
        // once total (the exact "soft-if" duplicate-output bug section 8/10
        // document for the pre-cut `if`); (b) it also commits `for` to
        // cond/body's FIRST solution each iteration, matching `if`'s own
        // commit-to-first-solution semantics, rather than letting a
        // non-deterministic cond/body multiply the recursion. Crucially
        // this does NOT weaken "body failure stops the loop": cut is only
        // ever reached once cond+body have ALREADY succeeded for this
        // call -- if body fails, execution never reaches this cut at all,
        // and the whole rule-clause candidate fails exactly as before,
        // falling back to the fallback fact and ending the loop.
        lsClonedRuleBody.push_back( new vault::unify::ConsTerm( "__builtin_cut" ) );

        // Step: clone pStepValueTerm using the SAME subMapRule (so any "$i"
        // within it reads THIS iteration's fresh value), then assign the
        // result to a BRAND NEW fresh local $i2 (see the file comment
        // above for why this must not reuse subMapRule's mapping of
        // freeVars[0]).
        vault::unify::AbstractTerm* pClonedStep = vault::unify::cloneTermTree( pStepValueTerm, subMapRule );
        vault::unify::VarTerm* pFreshI2 = new vault::unify::VarTerm();
        pFreshI2->setOriginalVarName( m_clauseContext.nextAnonVarName() );
        const vault::unify::ConsTerm* pClonedStepCons = dynamic_cast<const vault::unify::ConsTerm*>( pClonedStep );
        bool stepIsArith = pClonedStepCons && 3==pClonedStepCons->getArity()
            && pClonedStepCons->getName().value() == "__builtin_arith";
        vault::unify::ConsTerm* pStepAssignGoal = new vault::unify::ConsTerm(
            stepIsArith ? "__builtin_eval" : "unify", pClonedStep, pFreshI2 );
        lsClonedRuleBody.push_back( pStepAssignGoal );

        // Explicit rebinds for every OTHER captured var before the
        // recursive call -- same reasoning as `foreach`'s own `$arr`/
        // outerVar rebinds above: rather than pass subMapRule[freeVars[i]]
        // (the SAME head-parameter object) unchanged into the recursive
        // call -- which would rely on `VarTerm::unifyVarTerm`'s
        // `this==pOther` identical-object fast path recording no
        // `AssignmentId` binding at all (vault-unify-term-var.cpp) to
        // still make the value visible several recursion levels down --
        // force a genuine, ordinary variable-to-variable `unify(fresh,
        // cur)` binding through `UnifyBuiltinClause` instead.
        std::vector<vault::unify::VarTerm*> outerNext( nVars );
        for( int i = 1; i < nVars; ++i ) {
            outerNext[i] = new vault::unify::VarTerm();
            lsClonedRuleBody.push_back( new vault::unify::ConsTerm(
                "unify", outerNext[i], subMapRule[freeVars[i]] ) );
        }

        // Recursive call: $i2 in place of freeVars[0] (pForVar)'s fresh
        // slot; every other captured var carried through via outerNext.
        vault::unify::AbstractTerm** ppRecurseArgs = new vault::unify::AbstractTerm*[nVars];
        ppRecurseArgs[0] = pFreshI2;
        for( int i = 1; i < nVars; ++i ) {
            ppRecurseArgs[i] = outerNext[i];
        }
        lsClonedRuleBody.push_back( new vault::unify::ConsTerm( forAtom, nVars, ppRecurseArgs ) );
        delete[] ppRecurseArgs;

        vault::unify::Goal* pRuleGoal = new vault::unify::Goal( lsClonedRuleBody.begin(), lsClonedRuleBody.end() );
        Clause* pRuleClause = new vault::unify::StandardClause( pHeadRule, pRuleGoal );

        // f. Fallback fact: its OWN, independent fresh substitution (must
        // not alias the rule clause's head vars).
        vault::unify::AbstractTerm** ppHeadArgsFact = new vault::unify::AbstractTerm*[nVars];
        for( int i = 0; i < nVars; ++i ) {
            vault::unify::VarTerm* pFresh = new vault::unify::VarTerm();
            pFresh->setOriginalVarName( freeVars[i]->getOriginalVarName() );
            ppHeadArgsFact[i] = pFresh;
        }
        vault::unify::ConsTerm* pHeadFact = new vault::unify::ConsTerm( forAtom, nVars, ppHeadArgsFact );
        delete[] ppHeadArgsFact;
        Clause* pFactClause = new vault::unify::StandardClause( pHeadFact, NULL );

        spWorld->getRootState()->appendClause( spWorld, pRuleClause );
        spWorld->getRootState()->appendClause( spWorld, pFactClause );

        // g. Scratch trees (cond's/step's own pre-goals, cond, body, and
        // the step-value expression) have now all been cloned; free their
        // structural nodes (freeVars' VarTerms are reused directly at the
        // call site below, left alone).
        {
            std::set<const vault::unify::AbstractTerm*> visited;
            std::vector<const vault::unify::AbstractTerm*>::const_iterator it, itEnd = auxRuleRoots.end();
            for( it = auxRuleRoots.begin(); it != itEnd; ++it ) {
                deleteScratchTermTree( *it, visited );
            }
            std::list<vault::unify::AbstractTerm*>::const_iterator it2, it2End;
            for( it2 = lsStepPreGoals.begin(), it2End = lsStepPreGoals.end(); it2 != it2End; ++it2 ) {
                deleteScratchTermTree( *it2, visited );
            }
            deleteScratchTermTree( pStepValueTerm, visited );
        }

        // h. Call site: __for__N( origFreeVars... ) -- freeVars[0] is
        // pForVar, already bound by the init pre-goal pushed above.
        vault::unify::AbstractTerm** ppCallArgs = new vault::unify::AbstractTerm*[nVars];
        for( int i = 0; i < nVars; ++i ) {
            ppCallArgs[i] = freeVars[i];
        }
        vault::unify::ConsTerm* pCallTerm = new vault::unify::ConsTerm( forAtom, nVars, ppCallArgs );
        delete[] ppCallArgs;

        return pCallTerm;
    }


    vault::unify::AbstractTerm* operator()( const ConsTermInput& consTermInput ) const
    {
        vault::unify::AbstractTerm* out_pAbstractTerm = NULL;
        std::string consTermInputName = consTermInput.atom.name;

        // ROADMAP Phase 2 (Cut): the bareword goal `cut` (zero arguments)
        // is a reserved word -- mirroring how `query` is reserved for a
        // zero-argument top-level head (SPEC.md) -- for the committed-
        // choice construct SolveJob::performSlice() (vault-unify-solvejob.cpp)
        // recognizes directly by this exact internal name; cut is NEVER a
        // registered Clause/builtin (it needs access to the SolveContext
        // stack itself, which no Clause ever gets). Renamed here, at
        // term-construction time, so every position a bareword ConsTerm
        // can occur in -- a goal statement, a clause head, or a plain
        // argument -- is covered by one change. `cut(...)` with one or
        // more arguments is completely unaffected: a normal clause/call
        // named "cut", exactly like `query(a)` stays a normal clause head
        // once it takes an argument.
        if( consTermInputName=="cut" && consTermInput.values.empty() ) {
            consTermInputName = "__builtin_cut";
        }

        // ROADMAP Phase 2 (runtime assert/retract, SPEC.md): `assert(...)`
        // and `retract(...)`, each with EXACTLY one argument, are reserved
        // goal names -- the same exact-name+arity reservation `cut` gets
        // above (not a purely positional/grammar reservation like `query`),
        // applied at the same single choke point so a goal statement, a
        // clause head, or a plain argument are all covered by one change.
        // Renamed to the internal names SolveJob::performSlice()
        // (vault-unify-solvejob.cpp) recognizes directly, mirroring
        // cut/findall: neither is ever a registered Clause/builtin, since
        // assert needs the World (to append a new clause) and retract needs
        // to scan+mutate the clause database directly, neither of which a
        // Clause::startUnification() implementation ever gets access to.
        // `assert(...)`/`retract(...)` with any OTHER arity (zero, or two
        // or more) are unaffected and remain ordinary clause heads/calls,
        // exactly like `cut(a)` remains ordinary once cut takes an
        // argument.
        if( consTermInputName=="assert" && 1==consTermInput.values.size() ) {
            consTermInputName = "__builtin_assert";
        } else if( consTermInputName=="retract" && 1==consTermInput.values.size() ) {
            consTermInputName = "__builtin_retract";
        }

        // Atom used, if at all, only to copy-construct ConsTerm::m_name
        // (which stores it BY VALUE) below -- a stack instance avoids
        // orphaning a heap Atom on every single call (both the VarTerm
        // branch, which never even looks at it, and the ConsTerm branch,
        // whose constructor only ever copies from it).
        vault::unify::Atom atom( consTermInputName );

        // Is it a consterm or a varterm?
        char ch = consTermInputName[0];
        if( '$' == ch ) {
            // VarTerm.
            vault::unify::VarTerm* pVarTerm = NULL;
            std::map<std::string,vault::unify::VarTerm*>::iterator itSym =
                m_clauseContext.m_mapSymbols.find( consTermInputName );
            if( itSym==m_clauseContext.m_mapSymbols.end() ) {
                // Does not exist, create.
                pVarTerm = new vault::unify::VarTerm();
                pVarTerm->setOriginalVarName( consTermInputName );
                m_clauseContext.m_mapSymbols[consTermInputName] = pVarTerm;
            } else {
                pVarTerm = itSym->second;
            }
            out_pAbstractTerm = pVarTerm;
        } else {
            vault::unify::AbstractTerm** ppTerms;
            int nTerms;
            if( consTermInput.values.empty() ) {
                nTerms = 0;
                ppTerms = NULL;
            } else {
                nTerms = consTermInput.values.size();
                ppTerms = new vault::unify::AbstractTerm*[nTerms];
                for( int i=0; i<nTerms; ++i ) {
                    vault::unify::AbstractTerm* pAbstractTerm = NULL;
                    pAbstractTerm = (*this)( consTermInput.values[i] );
                    ppTerms[i] = pAbstractTerm;
                }
            }
            vault::unify::ConsTerm* pConsTerm = new vault::unify::ConsTerm(
                atom, nTerms, ppTerms );
            // ConsTerm's ctor copies the pointer VALUES from ppTerms into
            // its own m_vecTerms (constructed from the [ppTerms,ppTerms+n)
            // range) -- it does not take ownership of the ppTerms buffer
            // itself, only of the children it points to. Free the
            // now-redundant transient buffer (safe/no-op when NULL).
            delete[] ppTerms;
            out_pAbstractTerm = pConsTerm;
        }

        // Add debug info.
        if( out_pAbstractTerm ) {
            const vault::unify::FileDebugInfo* pFileDebugInfo =
                m_clauseContext.m_context.getFileDebugInfo();
            vault::unify::TermDebugInfo* pTermDebugInfo =
                new vault::unify::TermDebugInfo( out_pAbstractTerm, pFileDebugInfo, 
                    consTermInput.atom.line );
            m_clauseContext.m_context.getWorld()->setTermDebugInfo(
                out_pAbstractTerm, pTermDebugInfo );
        }

        return out_pAbstractTerm;
    }


    /**
     * ROADMAP Phase 2 ("consistent list/array semantics", SPEC.md section
     * 11): builds a first-class ArrayTerm, ordered and positional --
     * replacing the previous desugaring straight into a MapTerm keyed by
     * stringified numeric indices (a documented SPEC.md quirk, now
     * removed).
     */
    vault::unify::AbstractTerm* operator()( const ArrayTermInput& arrayTermInput ) const
    {
        vault::unify::AbstractTerm* out_pAbstractTerm = NULL;
        int nTerms;
        vault::unify::AbstractTerm** ppTerms;

        if( arrayTermInput.members.empty() ) {
            nTerms = 0;
            ppTerms = NULL;
        } else {
            nTerms = arrayTermInput.members.size();
            ppTerms = new vault::unify::AbstractTerm*[nTerms];
            for( int i=0; i<nTerms; ++i ) {
                ppTerms[i] = (*this)( arrayTermInput.members[i] );
            }
        }

        vault::unify::ArrayTerm* pArrayTerm = new vault::unify::ArrayTerm(
            ppTerms, nTerms );
        // ArrayTerm's ctor copies the *pointer values* out of ppTerms into
        // its own std::vector; it never takes ownership of the transient
        // buffer itself. Free the now-redundant buffer (safe/no-op when
        // NULL).
        delete[] ppTerms;
        out_pAbstractTerm = pArrayTerm;

        return out_pAbstractTerm;
    }

    vault::unify::AbstractTerm* operator()( const MapTermInput& mapTermInput ) const
    {
        vault::unify::AbstractTerm* out_pAbstractTerm = NULL;
        int nTerms;
        const vault::unify::Atom** ppAtoms;
        vault::unify::AbstractTerm** ppTerms;

        if( mapTermInput.pairs.empty() ) {
            nTerms = 0;
            ppAtoms = NULL;
            ppTerms = NULL;
        } else {
            nTerms = mapTermInput.pairs.size();
            ppAtoms = new const vault::unify::Atom*[nTerms];
            ppTerms = new vault::unify::AbstractTerm*[nTerms];
            for( int i=0; i<nTerms; ++i ) {
                ppAtoms[i] = new vault::unify::Atom( mapTermInput.pairs[i].pairKey.name );
                vault::unify::AbstractTerm* pAbstractTerm = NULL;
                pAbstractTerm = (*this)( mapTermInput.pairs[i].pairValue );
                ppTerms[i] = pAbstractTerm;
            }
        }

        vault::unify::MapTerm* pMapTerm = new vault::unify::MapTerm(
            ppAtoms, ppTerms, nTerms );
        // See ArrayTermInput overload above: MapTerm only takes ownership
        // of the Atoms/terms these buffers point to, not of the buffers.
        delete[] ppAtoms;
        delete[] ppTerms;
        out_pAbstractTerm = pMapTerm;

        return out_pAbstractTerm;
    }

private:
    /**
     * The actual context we are running in. This defines a current set of
     * variable symbol definitions as required for interpreting the AST.
     */
    ClauseContext& m_clauseContext;

    /**
     * The list of goals we create alongside. The main goal will not
     * be created by this class. It shall be pushed back by the caller.
     */
    std::list<vault::unify::AbstractTerm*>& m_lsTerms;
};

}
}
}


/**
 * Factor a goal from a goal input term.
 * 
 * @param clauseContext
 *     The context of the current clause. This context e.g. contains the
 *     variable mappings as valid within the current clause.
 * @param out_lsGoals
 *     A list of goals as created by this input goal. Several goals
 *     maybe created due to conversion of builtin syntax-terms to
 *     auxialiare goals.
 */
int vault::unify::PrologParser::Context::createGoal(
        vault::unify::PrologParser::ClauseContext& clauseContext,
        const vault::unify::PrologParser::GoalInput& goalInput,
        std::list<vault::unify::AbstractTerm*>& out_lsTerms
        )
{
    std::list<vault::unify::AbstractTerm*> localTermList;
    std::vector<vault::unify::PrologParser::AnyStatementInput>::const_iterator
        itBegin = goalInput.consTerms.begin(),
        itEnd = goalInput.consTerms.end();

    AnyTermFactory anyTermFactory( clauseContext, out_lsTerms );

    for( ; itBegin != itEnd; ++itBegin ) {
        vault::unify::AbstractTerm* pAbstractTerm = NULL;

        pAbstractTerm = anyTermFactory( *itBegin );
        // Make this a AST parsing time assumption.
        //localTermList.push_back( pAbstractTerm );
        out_lsTerms.push_back( pAbstractTerm );
    }
    // Do not create a goal at this point. Instead, return the term list.
    // vault::unify::Goal* pGoal = new vault::unify::Goal(
    //     consTermList.begin(), consTermList.end() );
    // out_lsGoals.push_back( pGoal );
    return 0;
}


int vault::unify::PrologParser::Context::createClause(
        vault::unify::PrologParser::ClauseContext& clauseContext,
        const vault::unify::PrologParser::ClauseInput& clauseInput,
        vault::unify::Clause*& out_pClause )
{
    // Left hand side.
    vault::unify::AbstractTerm* pAbstractTerm = NULL;
    {
        std::list<vault::unify::AbstractTerm*> lsClauseTerm;
        AnyTermFactory anyTermFactory( clauseContext, lsClauseTerm );
        pAbstractTerm = anyTermFactory( clauseInput.leftHandTerm );
        if( !lsClauseTerm.empty() ) {
            // TXWTODO: Error handling?
            VAULT_UNIFY_DI( ALWAYS, "Clause lhs has more then one term.\n" );
            return -1;
        }
    }
    // Right hand side.
    vault::unify::Goal* pGoal = NULL;
    {
        // The terms of the goal, which is our right hand side.
        std::list<vault::unify::AbstractTerm*> lsGoalTerms;
        // The rhs goal.
        if( !clauseInput.rightHandGoal.consTerms.empty() ) { 
            (void) createGoal( clauseContext, clauseInput.rightHandGoal, lsGoalTerms );
            pGoal = new vault::unify::Goal(
                lsGoalTerms.begin(), lsGoalTerms.end() );
        }
    }
    {
        // Make up clause from lhs and rhs.
        vault::unify::Clause* pClause = new vault::unify::StandardClause(
            dynamic_cast<vault::unify::ConsTerm*>( pAbstractTerm ), pGoal );
        out_pClause = pClause;
    }
    return 0;
}


