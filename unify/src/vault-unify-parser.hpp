#if !defined( _VAULT_UNIFY_PARSER_HPP )
#define _VAULT_UNIFY_PARSER_HPP

/*
 * Workaround for a but in boost 1.58: It does not support
 * empty structs in BOOST_FUSION_ADOPT_STRUCT
 */
#undef BOOST_PP_VARIADICS
#define BOOST_PP_VARIADICS 0

#include <vault-unification.hpp>

#include <atomic>
#include <string>
#include <vector>
#include <iomanip>

#include <boost/config/warning_disable.hpp>
#include <boost/bind.hpp>

#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>
#include <boost/spirit/include/phoenix_object.hpp>
#include <boost/spirit/include/phoenix_fusion.hpp>
#include <boost/spirit/include/phoenix.hpp>
#include <boost/spirit/repository/include/qi_iter_pos.hpp>

#include <boost/fusion/include/adapt_struct.hpp>

#include <boost/fusion/include/io.hpp>

#define BOOST_SPIRIT_USE_PHOENIX_V3

namespace vault {
namespace unify {    

class FileDebugInfo;


namespace PrologParser {

#define USE_NIL 1

#if USE_NIL
struct unifynil {};
#endif

struct AtomInput {
    AtomInput() {}
    AtomInput( const std::string& strName, int l ) 
        : name( strName )
        , line( l ) {}
    std::string name;
    int line;
};

struct AnyTermInput;
struct ConsTermInput;
struct InfixTermsInput;
struct PrefixTermInput;
struct MapPairInput;
struct ArithTermInput;
struct CompareTermInput;
struct RangeTermInput;

struct MapTermInput {
    MapTermInput() {};
    std::vector<MapPairInput> pairs;
};


struct ArrayTermInput {
    ArrayTermInput() {};
    std::vector<AnyTermInput> members;
};

typedef boost::variant<
#if USE_NIL
        unifynil,
#endif
        boost::recursive_wrapper<ConsTermInput>,
        boost::recursive_wrapper<MapTermInput>,
        boost::recursive_wrapper<ArrayTermInput>,
        boost::recursive_wrapper<PrefixTermInput>,
        boost::recursive_wrapper<InfixTermsInput>,
        boost::recursive_wrapper<ArithTermInput>,
        boost::recursive_wrapper<CompareTermInput>,
        boost::recursive_wrapper<RangeTermInput>
        > AnyTermRecursiveType;

struct AnyTermInput {
    AnyTermInput()
#if USE_NIL
    : term( unifynil() )
#endif
    {};
    AnyTermInput( const AnyTermInput& other ) : term( other.term ) {}
    AnyTermInput& operator=( const AnyTermInput& other ) { term = other.term; return *this; }
    AnyTermInput( const ConsTermInput& cti ) : term( cti ) {}
    AnyTermInput( const MapTermInput& mti ) : term( mti ) {}
    AnyTermInput( const ArrayTermInput& ati ) : term( ati ) {}
    AnyTermInput( const PrefixTermInput& pti ) : term( pti ) {}
    AnyTermInput( const InfixTermsInput& iti ) : term( iti ) {}
    // Arithmetic ('*','/','+','-') and comparison ('==','!=','<=','>=',
    // '<','>') precedence levels, inserted between m_rulePrefixTerm and
    // m_ruleInfixTerm ('=') -- ROADMAP Phase 2 "Arithmetic and comparison
    // builtins" (see SPEC.md). Each level's own struct doubles as an
    // AnyTermInput variant alternative for the same reason PrefixTermInput/
    // InfixTermsInput above do: the ENCLOSING level's field type is
    // AnyTermInput, so Spirit needs this converting ctor to build one from
    // whatever concrete attribute type the next level down actually
    // produces.
    AnyTermInput( const ArithTermInput& ati ) : term( ati ) {}
    AnyTermInput( const CompareTermInput& cti ) : term( cti ) {}
    // ROADMAP ("for"/"foreach" + ranges, language owner request 2026-08-21):
    // `<a>..<b>`, inserted between m_ruleAdditive and m_ruleComparison --
    // see RangeTermInput below and AnyTermFactory::operator()(const
    // RangeTermInput&), src/vault-unify-parser.cpp, for the desugaring.
    AnyTermInput( const RangeTermInput& rti ) : term( rti ) {}

    AnyTermRecursiveType term;
};

struct ConsTermInput {
    ConsTermInput() {}
    // ctor for cons term with atom only.
    ConsTermInput( const AtomInput& ai ) : atom( ai ) {}
    AtomInput atom;
    std::vector<AnyTermInput> values;
};

struct InfixTermRightHandSide {
    InfixTermRightHandSide() {}
    InfixTermRightHandSide( const AnyTermInput& ati ) : first( 0 ), second( ati ) {}

    char first;
    AnyTermInput second;
};

struct InfixTermsInput {
    InfixTermsInput() {}
    InfixTermsInput( const AnyTermInput& ati ) : atilhs( ati ) {}

    AnyTermInput atilhs;
    boost::optional<InfixTermRightHandSide> rhs;
};

struct PrefixTermInput {
    PrefixTermInput() {}
    PrefixTermInput( const InfixTermsInput& ati ) : rhs( ati ) {}
    PrefixTermInput( char op ) : first( op ) {}

    boost::optional<char> first;
    InfixTermsInput rhs;
};


/**
 * One (operator, operand) step of a left-associative multiplicative
 * ('*','/') or additive ('+','-') chain -- e.g. `a * b / c` is
 * ArithTermInput{ lhs=a, rhsList=[ {op='*',second=b}, {op='/',second=c} ] }.
 * Both m_ruleMultiplicative and m_ruleAdditive produce this same struct
 * (only the set of operator characters the grammar rule matches differs);
 * AnyTermFactory::operator()(const ArithTermInput&) folds rhsList
 * left-to-right regardless of which level produced it, since precedence
 * is already resolved by the grammar's nesting.
 */
struct ArithTermRhsInput {
    ArithTermRhsInput() {}
    char op;
    AnyTermInput second;
};

struct ArithTermInput {
    ArithTermInput() {}
    AnyTermInput lhs;
    std::vector<ArithTermRhsInput> rhsList;
};


/**
 * Comparison level ('==','!=','<=','>=','<','>'): single-shot (at most one
 * operator), non-associative -- `a < b < c` is not meaningful here and is
 * not supported, matching the task's design (SPEC.md).
 */
struct CompareTermRhsInput {
    CompareTermRhsInput() {}
    std::string op;
    AnyTermInput second;
};

struct CompareTermInput {
    CompareTermInput() {}
    AnyTermInput lhs;
    boost::optional<CompareTermRhsInput> rhs;
};


/**
 * `<a>..<b>` (ROADMAP: language owner request "for/foreach + ranges",
 * 2026-08-21): inserted between m_ruleAdditive and m_ruleComparison, so a
 * range's bounds are additive-level expressions (may themselves be
 * arithmetic, e.g. `1+1..5`) and a range can appear anywhere an ordinary
 * AnyTerm can (not just inside foreach's header) -- see SPEC.md and
 * AnyTermFactory::operator()(const RangeTermInput&) (vault-unify-parser.cpp)
 * for the desugaring (eager ArrayTerm literal for literal integer bounds,
 * else a `__builtin_range` pre-goal). `..` is a two-character `qi::lit`
 * token contributing no attribute of its own (mirroring `->`'s `'\0'`-op
 * case, InfixTermRightHandSide above), hence the same
 * AnyTermInput-converting-constructor trick to accept whatever
 * m_ruleAdditive's plain ArithTermInput attribute converts through.
 */
struct RangeTermRhsInput {
    RangeTermRhsInput() {}
    RangeTermRhsInput( const AnyTermInput& ati ) : second( ati ) {}
    AnyTermInput second;
};

struct RangeTermInput {
    RangeTermInput() {}
    AnyTermInput lhs;
    boost::optional<RangeTermRhsInput> rhs;
};


struct MapPairInput {
    MapPairInput() {}
    MapPairInput( const MapPairInput& other ) : pairKey( other.pairKey ), pairValue( other.pairValue ) {}
    MapPairInput( const AtomInput& ai ) : pairKey( ai ) {}
    AtomInput pairKey;
    AnyTermInput pairValue;
};


class IfStatementInput;
class SingleGoalInput;
class AnyStatementInput;
class ForeachStatementInput;
class ForStatementInput;

typedef boost::variant<
#if USE_NIL
        unifynil,
#endif
        boost::recursive_wrapper<IfStatementInput>,
        boost::recursive_wrapper<ForeachStatementInput>,
        boost::recursive_wrapper<ForStatementInput>,
        boost::recursive_wrapper<SingleGoalInput>,
        boost::recursive_wrapper<AnyStatementInput>
        > AnyStatementRecursiveType;


struct AnyStatementInput {
    AnyStatementInput()
#if USE_NIL
    : statement( unifynil() )
#endif
    {};
    AnyStatementInput( const AnyStatementInput& other ) : statement( other.statement ) {}
    AnyStatementInput( const IfStatementInput& isi ) : statement( isi ) {}
    AnyStatementInput( const ForeachStatementInput& fsi ) : statement( fsi ) {}
    AnyStatementInput( const ForStatementInput& fsi ) : statement( fsi ) {}
    AnyStatementInput( const SingleGoalInput& sgi ) : statement( sgi ) {}

    AnyStatementRecursiveType statement;
};


struct GoalInput {
    std::vector<AnyStatementInput> consTerms;
};


struct SingleGoalInput {
    SingleGoalInput() {}
    SingleGoalInput( const InfixTermsInput& ati ) : rhs( ati ) {}
    SingleGoalInput( char op ) : first( op ) {}
    boost::optional<char> first;
    InfixTermsInput rhs;
};


struct IfStatementInput {
    IfStatementInput() {}
    IfStatementInput( const SingleGoalInput& sgi ) : lhs( sgi ) {}
    SingleGoalInput lhs;
    GoalInput rhs;
};


/**
 * `foreach ( $x : arrExpr ) { body }` (ROADMAP: language owner request
 * "classic for/foreach loops", 2026-08-21). `loopVar` is parsed via
 * m_ruleConsTerm (the same production every other bare `$name` variable
 * reference in this grammar goes through) rather than a dedicated rule --
 * conventionally, but NOT grammatically enforced, a `$`-prefixed variable
 * (SPEC.md documents this). `arrExpr` is anything m_ruleAnyTerm accepts,
 * including a range (RangeTermInput above). See
 * AnyTermFactory::operator()(const ForeachStatementInput&)
 * (vault-unify-parser.cpp) for the desugaring.
 */
struct ForeachStatementInput {
    ForeachStatementInput() {}
    ConsTermInput loopVar;
    AnyTermInput arrExpr;
    GoalInput body;
};


/**
 * `for ( $i = init; cond; $i = step ) { body }` (ROADMAP: language owner
 * request "classic for/foreach loops", 2026-08-21). All three header pieces
 * reuse m_ruleSingleGoal (the same production a bare goal statement or an
 * `if` condition uses) -- `initAssign`/`stepAssign` are conventionally, but
 * not grammatically enforced, a `$var = expr` assignment (SPEC.md documents
 * this). See AnyTermFactory::operator()(const ForStatementInput&)
 * (vault-unify-parser.cpp) for the desugaring.
 */
struct ForStatementInput {
    ForStatementInput() {}
    SingleGoalInput initAssign;
    SingleGoalInput condGoal;
    SingleGoalInput stepAssign;
    GoalInput body;
};


struct ClauseInput {
    ClauseInput() {}
    ClauseInput( const ConsTermInput& cti ) : leftHandTerm( cti ) {}
    ConsTermInput leftHandTerm;
    GoalInput rightHandGoal;
};


struct QueryInput {
    QueryInput() {}
//    QueryInput( const ConsTermInput& cti ) : leftHandTerm( cti ) {}
    GoalInput queryGoal;
};


/**
 * `import "relative/path.ufy";` (ROADMAP Phase 2 "File imports / include",
 * SPEC.md section 15). `m_ruleImport` is tried BEFORE `m_ruleQuery`/
 * `m_ruleClause` in `m_ruleEvent`, mirroring the exact-keyword-plus-required-
 * shape reservation `query` already uses (SPEC.md section 2): after
 * `qi::lit("import")`, the very next token must be a quoted string, so
 * `import(...)` (a clause head literally named "import") fails this rule at
 * that point and backtracks to `m_ruleClause` unaffected -- same trick as
 * `query`'s own reservation trace.
 */
struct ImportInput {
    ImportInput() {}
    ImportInput( const std::string& p ) : path( p ) {}
    std::string path;
};


struct EventInput {
    EventInput() {}
    EventInput( const QueryInput& qi ) : query( qi ) {}
    EventInput( const ClauseInput& qi ) : clause( qi ) {}
    EventInput( const ImportInput& ii ) : import( ii ) {}
    QueryInput query;
    ClauseInput clause;
    ImportInput import;
};

}; // namespace PrologParser
}; // namespace unify
}; // namespace vault

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::unifynil,
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::AtomInput,
    (std::string, name)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::ConsTermInput,
    (vault::unify::PrologParser::AtomInput, atom)
    (std::vector<vault::unify::PrologParser::AnyTermInput>, values)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::InfixTermRightHandSide,
    (char,first)
    (vault::unify::PrologParser::AnyTermInput,second)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::InfixTermsInput,
    (vault::unify::PrologParser::AnyTermInput, atilhs)
    (boost::optional<vault::unify::PrologParser::InfixTermRightHandSide>, rhs)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::PrefixTermInput,
    (boost::optional<char>, first)
    (vault::unify::PrologParser::InfixTermsInput, rhs)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::ArithTermRhsInput,
    (char, op)
    (vault::unify::PrologParser::AnyTermInput, second)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::ArithTermInput,
    (vault::unify::PrologParser::AnyTermInput, lhs)
    (std::vector<vault::unify::PrologParser::ArithTermRhsInput>, rhsList)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::CompareTermRhsInput,
    (std::string, op)
    (vault::unify::PrologParser::AnyTermInput, second)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::CompareTermInput,
    (vault::unify::PrologParser::AnyTermInput, lhs)
    (boost::optional<vault::unify::PrologParser::CompareTermRhsInput>, rhs)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::RangeTermRhsInput,
    (vault::unify::PrologParser::AnyTermInput, second)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::RangeTermInput,
    (vault::unify::PrologParser::AnyTermInput, lhs)
    (boost::optional<vault::unify::PrologParser::RangeTermRhsInput>, rhs)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::MapTermInput,
    (std::vector<vault::unify::PrologParser::MapPairInput>, pairs)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::ArrayTermInput,
    (std::vector<vault::unify::PrologParser::AnyTermInput>, members)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::AnyTermInput,
    (vault::unify::PrologParser::AnyTermRecursiveType, term)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::MapPairInput,
    (vault::unify::PrologParser::AtomInput, pairKey)
    (vault::unify::PrologParser::AnyTermInput, pairValue)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::SingleGoalInput,
    (boost::optional<char>, first)
    (vault::unify::PrologParser::InfixTermsInput, rhs)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::IfStatementInput,
    (vault::unify::PrologParser::SingleGoalInput, lhs)
    (vault::unify::PrologParser::GoalInput,rhs)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::ForeachStatementInput,
    (vault::unify::PrologParser::ConsTermInput, loopVar)
    (vault::unify::PrologParser::AnyTermInput, arrExpr)
    (vault::unify::PrologParser::GoalInput, body)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::ForStatementInput,
    (vault::unify::PrologParser::SingleGoalInput, initAssign)
    (vault::unify::PrologParser::SingleGoalInput, condGoal)
    (vault::unify::PrologParser::SingleGoalInput, stepAssign)
    (vault::unify::PrologParser::GoalInput, body)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::AnyStatementInput,
    (vault::unify::PrologParser::AnyStatementRecursiveType, statement)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::GoalInput,
    (std::vector<vault::unify::PrologParser::AnyStatementInput>, consTerms)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::ClauseInput,
    (vault::unify::PrologParser::ConsTermInput, leftHandTerm)
    (vault::unify::PrologParser::GoalInput, rightHandGoal)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::QueryInput,
    (vault::unify::PrologParser::GoalInput, queryGoal)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::ImportInput,
    (std::string, path)
)

BOOST_FUSION_ADAPT_STRUCT(
    vault::unify::PrologParser::EventInput,
    (vault::unify::PrologParser::QueryInput, query)
    (vault::unify::PrologParser::ClauseInput, clause)
    (vault::unify::PrologParser::ImportInput, import)
)

namespace vault {
namespace unify {
namespace PrologParser {

namespace qi = boost::spirit::qi;
namespace phoenix = boost::phoenix;


class Context;


struct ClauseContext {
    ClauseContext( Context& context ) : m_context( context ), m_anonVarIndex(1000) {}
    void reset() { m_mapSymbols.clear(); }
    std::map<std::string,vault::unify::VarTerm*> m_mapSymbols;
    std::string nextAnonVarName() {
        return std::string("$__")
            +boost::lexical_cast<std::string>( m_anonVarIndex++ );
    }
    Context& m_context;
    int m_anonVarIndex;
    std::string nextAnonClauseName( const std::string& tag ) {
        return std::string( "__" ) +tag+ std::string( "__" )
            +boost::lexical_cast<std::string>( m_anonClauseIndex++ );
    }

    /**
     * Engine item E14: atomic, and process-wide.
     *
     * A static member, so ONE counter is shared by every ClauseContext,
     * every Context and every World in the process -- which means two
     * RuntimeContexts parsing at once (exactly what a session that no
     * longer waits for engine idle permits) were racing to name their
     * desugared clauses. A collision here does not merely produce an
     * odd name: two different control constructs would be given the
     * same __fe__N and silently share a predicate.
     *
     * Making it atomic fixes the race, not the sharing. Per-World
     * numbering is the right end state and is a behaviour change (the
     * generated names move), so it belongs with E12's staged parse
     * rather than here.
     */
    static std::atomic<int> m_anonClauseIndex;
};

class Context {
public:
    Context( WorldPtr spWorld ) : m_spWorld( spWorld ), m_pFileDebugInfo( NULL ) { }

    /**
     * Given an abstract syntax tree, isUnificationDonegenerate a list of terms.
     * The overall list of terms later may be converted to a goal.
     */
    int createGoal(
        ClauseContext& clauseContext,
        const vault::unify::PrologParser::GoalInput& goalInput,
        std::list<vault::unify::AbstractTerm*>& out_lsTerms
        );
    int createClause(
        ClauseContext& clauseContext,
        const vault::unify::PrologParser::ClauseInput& clauseInput,
        vault::unify::Clause*& out_pClause
        );

    WorldPtr getWorld() const { return m_spWorld; }

    void setFileDebugInfo( const FileDebugInfo* pFileDebugInfo ) {
        m_pFileDebugInfo = pFileDebugInfo;
    }

    const FileDebugInfo* getFileDebugInfo() const {
        return m_pFileDebugInfo;
    }

private:
    WorldPtr m_spWorld;
    const FileDebugInfo* m_pFileDebugInfo;
};


template <typename Iterator>
    struct ufy_skipper 
        : public qi::grammar<Iterator> 
{
    ufy_skipper( vault::unify::PrologParser::Context& pctx )
            : ufy_skipper::base_type(start)
            , m_pctx( pctx )
    {
        start = qi::ascii::space 
            | ("/*" >> *(qi::char_ - "*/") >> "*/")
            | "//" >> *(qi::char_ - qi::eol) >> ( qi::eol );
    }

    qi::rule<Iterator> start;
    vault::unify::PrologParser::Context& m_pctx;
};

// lazy function for error reporting
struct ReportError {
    // the result type must be explicit for Phoenix
    template<typename, typename, typename, typename>
    struct result { typedef void type; };

    // contract the string to the surrounding new-line characters
    template<typename Iter>
    void operator()(Iter first_iter, Iter last_iter,
                Iter error_iter, const qi::info& what) const {
    std::string first(first_iter, error_iter);
    std::string last(error_iter, last_iter);
    auto first_pos = first.rfind('\n');
    auto last_pos = last.find('\n');
    auto error_line = ((first_pos == std::string::npos) ? first
                    : std::string(first, first_pos + 1))
                    + std::string(last, 0, last_pos);
    auto error_pos = (error_iter - first_iter) + 1;
    if (first_pos != std::string::npos) {
        error_pos -= (first_pos + 1);
    }
    std::cerr
        << "Parsing error in " << what << std::endl
        << error_line << std::endl
        << std::setw(error_pos) << '^'
        << std::endl;
    }
};


template<typename It>
struct annotation_f {
    template<typename,typename> struct result { typedef void type; };
    //typedef void result;

    annotation_f(It _first) : first(_first) {}
    It const first;

    template<typename Val, typename First>
    void operator()(Val& v, First f ) const {
        do_annotate(v, f, first);
    }
  private:
    void static do_annotate( AtomInput& ai, It f, It const /*first*/ ) {
        ai.line   = get_line(f);
    }
    static void do_annotate(...) {}
};


template <typename Iterator, typename Skipper> 
    struct ClauseParser 
        : public qi::grammar<Iterator, EventInput(), Skipper /*qi::ascii::space_type*/>
{
public:
    ClauseParser( Iterator first, Context& pctx )
            : ClauseParser::base_type( m_ruleEvent )
            , m_pctx( pctx )
            // , annotate( first )
    {
        using boost::phoenix::at_c;
        using boost::phoenix::push_back;

        m_unescapedChar.add
                ("\\a", '\a')("\\b", '\b')("\\f", '\f')("\\n", '\n')
                ("\\r", '\r')("\\t", '\t')("\\v", '\v')
                ("\\\\", '\\')("\\\'", '\'')("\\\"", '\"')
            ;
 
        m_unescapedString %= 
                qi::lit( '"' ) >> qi::no_skip[ *(m_unescapedChar | "\\x" >> qi::hex | qi::char_ - '"' - '\\') ] >>  qi::lit( '"' )
            ;

        m_ruleId %= qi::char_( "$a-zA-Z_" ) >> *qi::char_( "a-zA-Z_0-9" );

        m_ruleNumber %= + qi::char_( "0-9" );

        m_ruleAtom %=
                ( m_ruleId >> qi::attr( 1 ) /* boost::spirit::repository::qi::iter_pos.position()*/ )
            |   ( m_ruleNumber >> qi::attr( 1 ) /* boost::spirit::repository::qi::iter_pos.position()*/ )
            |   ( m_unescapedString >> qi::attr( 1 ) /* boost::spirit::repository::qi::iter_pos.position()*/ )
            // or another type of constant like a number.
            ;

        m_ruleMapPair %=
                ( m_ruleAtom >> ':' >> m_ruleAnyTerm )
            ;

        m_ruleMapTerm %=
                ( '{' 
                    >> ( m_ruleMapPair % ',' )
                    >>'}' 
                )
            ;

        m_ruleArrayTerm %=
                ( '['
                    >> ( m_ruleAnyTerm % ',' )
                    >> ']'
                )
            ;

        m_ruleConsTerm %=
                (m_ruleAtom 
                    >> -( 
                        '(' 
                        >> ( m_ruleAnyTerm  % ',' )
                        >> ')' )
                    )
            ;

        m_ruleAnyConsTerm %=
                m_ruleConsTerm
            |   m_ruleMapTerm
            |   m_ruleArrayTerm
            ;

        m_ruleArrayDeref %=
                (m_ruleAnyConsTerm >> ( -( qi::char_( "[" ) >> m_ruleAnyConsTerm ) ) )
            ;

        m_ruleAssignmentPart %=
                ( m_ruleArrayDeref >> ( -( qi::lit( "->" ) >> m_ruleArrayDeref ) ) )
            ;

        m_rulePrefixTerm %=
                //m_ruleAssignmentPart
                -qi::char_( '-' ) >> m_ruleAssignmentPart
            ;

        // Arithmetic/comparison precedence levels (ROADMAP Phase 2,
        // SPEC.md section 4): inserted between m_rulePrefixTerm and the
        // existing '=' level (m_ruleInfixTerm). Binary '-' is included
        // alongside '+ * /' -- see SPEC.md for the trace showing this does
        // not conflict with the hyphens used elsewhere in this grammar
        // (they all belong to the quoted-string rule m_unescapedString or
        // to the unrelated "->" token, never to a bareword).
        // NOTE: deliberately NOT qi::char_("*/") / qi::char_("+-") -- a
        // char-set string spec treats "ch1-ch2" as an inclusive range, and
        // while a trailing/leading lone '-' with no partner char cannot
        // form one, spelling the alternatives out avoids any doubt (in
        // particular, must never accidentally include ',', the argument
        // separator, in the additive operator set).
        m_ruleMultiplicative %=
                ( m_rulePrefixTerm >> *( ( qi::char_( '*' ) | qi::char_( '/' ) ) >> m_rulePrefixTerm ) )
            ;

        m_ruleAdditive %=
                ( m_ruleMultiplicative >> *( ( qi::char_( '+' ) | qi::char_( '-' ) ) >> m_ruleMultiplicative ) )
            ;

        // ROADMAP ("for"/"foreach" + ranges, language owner request
        // 2026-08-21): `<a>..<b>`, inserted between m_ruleAdditive and
        // m_ruleComparison -- see RangeTermInput (this header) and
        // AnyTermFactory::operator()(const RangeTermInput&) (vault-unify-
        // parser.cpp). ".." is a plain two-character qi::lit token; nothing
        // else in this grammar ever uses a bare '.', so there is no
        // longest-match ambiguity to resolve (unlike the comparison
        // operators just below).
        m_ruleRange %=
                ( m_ruleAdditive >> -( qi::lit( ".." ) >> m_ruleAdditive ) )
            ;

        // Longest-match ordering matters: "=="/"!="/"<="/">=" must be tried
        // before "<"/">" so e.g. `a <= b` cannot half-match as `a < ...`.
        m_ruleComparison %=
                ( m_ruleRange >> -(
                        ( qi::string( "==" ) | qi::string( "!=" )
                        | qi::string( "<=" ) | qi::string( ">=" )
                        | qi::string( "<" )  | qi::string( ">" ) )
                    >> m_ruleRange ) )
            ;

        m_ruleInfixTerm %=
                ( m_ruleComparison >> ( -( qi::char_( "=" ) >> m_ruleComparison ) ) )
            ;

        m_ruleAnyTerm %=
                m_ruleInfixTerm
            ;

        m_ruleSingleGoal %=
                -qi::char_( '!' ) >> m_ruleInfixTerm;
            ;
     
        m_ruleIfStatement %=
                qi::lit( "if") >> qi::lit( "(" ) >> m_ruleSingleGoal >> qi::lit( ")" )
                    >> qi::lit( "{" ) >> m_ruleGoal >> qi::lit( "}" )
            ;

        // ROADMAP ("for"/"foreach" loops, language owner request
        // 2026-08-21): reserved statement keywords, mirroring `if`'s own
        // reservation (SPEC.md section 2) -- structural/positional, not by
        // exact name+arity like `cut`/`query`. `m_ruleForeachStatement`/
        // `m_ruleForStatement` are tried BEFORE `m_ruleSingleGoal` in
        // `m_ruleAnyStatement` below; a goal that merely happens to be a
        // call to a predicate named `for`/`foreach` but does NOT match the
        // full loop-header-plus-`{ }`-body shape simply fails to match here
        // and backtracks to `m_ruleSingleGoal`, parsing as an ordinary call
        // -- exactly the same PEG backtracking `if`/`query` already rely on
        // (see SPEC.md section 2's `query` reservation trace). A clause
        // HEAD literally named `for(...)`/`foreach(...)` is unaffected
        // either way: `m_ruleAnyStatement` is never reached from
        // `m_ruleClause`, which parses a clause head via m_ruleConsTerm
        // directly.
        m_ruleForeachStatement %=
                qi::lit( "foreach" ) >> qi::lit( "(" )
                    >> m_ruleConsTerm >> qi::lit( ":" ) >> m_ruleAnyTerm
                >> qi::lit( ")" )
                    >> qi::lit( "{" ) >> m_ruleGoal >> qi::lit( "}" )
            ;

        m_ruleForStatement %=
                qi::lit( "for" ) >> qi::lit( "(" )
                    >> m_ruleSingleGoal >> qi::lit( ";" )
                    >> m_ruleSingleGoal >> qi::lit( ";" )
                    >> m_ruleSingleGoal
                >> qi::lit( ")" )
                    >> qi::lit( "{" ) >> m_ruleGoal >> qi::lit( "}" )
            ;

        m_ruleAnyStatement %=
                m_ruleForeachStatement
            |   m_ruleForStatement
            |   m_ruleIfStatement
            |   m_ruleSingleGoal >> ';'
            ;

        m_ruleGoal %=
                qi::eps >> +( m_ruleAnyStatement /* >> ';' */ )
            ;

        m_ruleClause %=
                ( m_ruleConsTerm >> qi::lit( "{" ) >> m_ruleGoal >> '}' )
            |   ( m_ruleConsTerm >> ';' )
            ;

        // NOTE: the bare atom "query" (no parens, i.e. a zero-argument
        // clause head) is a reserved word at the start of a top-level
        // form: `query { ... }` always parses as the query block below,
        // never as a rule named "query" with an empty body. A clause
        // headed by "query" is still expressible as long as it takes at
        // least one argument, e.g. `query( a ) { ... }` or `query( a );`,
        // since m_ruleQuery requires '{' to immediately follow the
        // "query" keyword (no '(' in between) and therefore fails and
        // backtracks to m_ruleClause for those forms.
        m_ruleQuery %=
                qi::lit( "query" ) >> '{' >> m_ruleGoal >> '}'
            ;

        // `import "relative/path.ufy";` (ROADMAP Phase 2 "File imports",
        // SPEC.md section 15). Tried before m_ruleQuery/m_ruleClause below,
        // the same reservation shape as m_ruleQuery itself (see ImportInput's
        // comment, this header): the keyword must be followed immediately by
        // a quoted string, so a clause head literally named "import" (e.g.
        // `import(x);` or `import { ... }`) fails here and backtracks to
        // m_ruleClause unaffected.
        m_ruleImport %=
                qi::lit( "import" ) >> m_unescapedString >> ';'
            ;

        m_ruleEvent %=
                (m_ruleImport)
            |   (m_ruleQuery)
            |   (m_ruleClause)
            ;
            
        m_ruleId.name( "Identifier" );
        m_ruleNumber.name( "Number" );
        m_ruleAtom.name( "Atom" );
        m_ruleAtomTerm.name( "AtomTerm" );
        m_ruleConsTerm.name( "ConsTerm" );
        m_ruleInfixTerm.name( "InfixTerm" );
        m_rulePrefixTerm.name( "PrefixTerm" );
        m_ruleMultiplicative.name( "Multiplicative" );
        m_ruleAdditive.name( "Additive" );
        m_ruleRange.name( "Range" );
        m_ruleComparison.name( "Comparison" );
        m_ruleAssignmentPart.name( "AssignmentPart" );
        m_ruleMapPair.name( "MapPair" );
        m_ruleMapTerm.name( "MapTerm" );
        m_ruleAnyConsTerm.name( "AnyConsTerm" );
        m_ruleAnyTerm.name( "AnyTerm" );
        m_ruleSingleGoal.name( "SingleGoal" );
        m_ruleForeachStatement.name( "ForeachStatement" );
        m_ruleForStatement.name( "ForStatement" );
        m_ruleGoal.name( "Goal" );
        m_ruleClause.name( "Clause" );
        m_ruleQuery.name( "Query" );
        m_ruleImport.name( "Import" );
        m_ruleEvent.name( "Event" );

        {
            /*qi::_1_type _1;
            qi::_2_type _2;
            qi::_3_type _3;
            qi::_4_type _4;
            qi::_val_type _val;*/
            typedef boost::phoenix::function<annotation_f<Iterator> > annotation_t;
            // annotation_t f = annotation_t( first )( _val, _1 );
            qi::on_success( m_ruleAtom, annotation_t( first )( boost::spirit::_val, boost::spirit::_1 ) );
        }
#if 0
        const phoenix::function<ReportError> report_error = ReportError();
        qi::on_error<qi::fail>( m_ruleId, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleNumber, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleAtom, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleAtomTerm, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleConsTerm, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleInfixTerm, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleAssignmentPart, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleMapPair, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleMapTerm, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleAnyConsTerm, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleAnyTerm, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleGoal, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleClause, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleQuery, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
        qi::on_error<qi::fail>( m_ruleEvent, report_error(boost::spirit::_1, boost::spirit::_2, boost::spirit::_3, boost::spirit::_4) );
#endif


#if 0
        qi::debug( m_ruleId );
        qi::debug( m_ruleNumber );
        qi::debug( m_ruleAtom );
        qi::debug( m_ruleAtomTerm );
        qi::debug( m_ruleConsTerm );
        qi::debug( m_ruleInfixTerm );
        qi::debug( m_ruleMapPair );
        qi::debug( m_ruleMapTerm );
        qi::debug( m_ruleAnyConsTerm );
        qi::debug( m_ruleAnyTerm );
        qi::debug( m_ruleGoal );
        qi::debug( m_ruleClause );
        qi::debug( m_ruleQuery );
        qi::debug( m_ruleEvent );
#endif
    }
        
    virtual ~ClauseParser() {}
   
    Context& m_pctx;
   
    qi::rule<Iterator, std::string(), Skipper> m_unescapedString;
    qi::symbols<char const, char const> m_unescapedChar;

    

    // qi::rule<Iterator, std::string(), Skipper, qi::locals<char> > m_ruleQuotedString;
    qi::rule<Iterator, std::string(), Skipper> m_ruleId;
    qi::rule<Iterator, std::string(), Skipper> m_ruleNumber;
    qi::rule<Iterator, AtomInput(), Skipper> m_ruleAtom;
    qi::rule<Iterator, ConsTermInput(), Skipper> m_ruleAtomTerm;
    qi::rule<Iterator, AnyTermInput(), Skipper> m_ruleAnyAtomTerm;
    qi::rule<Iterator, ConsTermInput(), Skipper> m_ruleConsTerm;
    qi::rule<Iterator, InfixTermsInput(), Skipper> m_ruleInfixTerm;
    qi::rule<Iterator, PrefixTermInput(), Skipper> m_rulePrefixTerm;
    qi::rule<Iterator, ArithTermInput(), Skipper> m_ruleMultiplicative;
    qi::rule<Iterator, ArithTermInput(), Skipper> m_ruleAdditive;
    qi::rule<Iterator, RangeTermInput(), Skipper> m_ruleRange;
    qi::rule<Iterator, CompareTermInput(), Skipper> m_ruleComparison;
    qi::rule<Iterator, InfixTermsInput(), Skipper> m_ruleAssignmentPart;
    qi::rule<Iterator, InfixTermsInput(), Skipper> m_ruleArrayDeref;
    qi::rule<Iterator, MapPairInput(), Skipper> m_ruleMapPair;
    qi::rule<Iterator, MapTermInput(), Skipper> m_ruleMapTerm;
    qi::rule<Iterator, ArrayTermInput(), Skipper> m_ruleArrayTerm;
    qi::rule<Iterator, AnyTermInput(), Skipper> m_ruleAnyConsTerm;
    qi::rule<Iterator, AnyTermInput(), Skipper> m_ruleAnyTerm;
    qi::rule<Iterator, SingleGoalInput(), Skipper> m_ruleSingleGoal;
    qi::rule<Iterator, IfStatementInput(), Skipper> m_ruleIfStatement;
    qi::rule<Iterator, ForeachStatementInput(), Skipper> m_ruleForeachStatement;
    qi::rule<Iterator, ForStatementInput(), Skipper> m_ruleForStatement;
    qi::rule<Iterator, AnyStatementInput(), Skipper> m_ruleAnyStatement;
    qi::rule<Iterator, GoalInput(), Skipper> m_ruleGoal;
    qi::rule<Iterator, ClauseInput(), Skipper> m_ruleClause;
    qi::rule<Iterator, QueryInput(), Skipper> m_ruleQuery;
    qi::rule<Iterator, ImportInput(), Skipper> m_ruleImport;
    qi::rule<Iterator, EventInput(), Skipper> m_ruleEvent;
private:
};

}; // namespace PrologParser
}; // namespace unify
}; // namespace vault

#endif
