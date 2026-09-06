#if !defined( _VAULT_UNIFY_SESSION_HPP )
#define _VAULT_UNIFY_SESSION_HPP

/**
 * @file vault-unify-session.hpp
 *
 * The boundary between any Unify front end and any Unify core.
 *
 * Specified in plans/todo/lens/SESSION-API.md; the gates it must satisfy
 * are in plans/todo/lens/ACCEPTANCE.md G0.
 *
 * THIS HEADER DELIBERATELY INCLUDES NO OTHER UNIFY HEADER AND NO BOOST
 * HEADER. That is not a stylistic preference -- it is gate G0.1, enforced
 * by a dedicated compile target (`unify-session-isolation`) that compiles a
 * translation unit including this file and nothing else of ours, and by
 * gate G0.2, a grep for engine type names and raw pointers in signatures
 * (test/session/check-header-hygiene.sh). If you find yourself wanting a
 * `Clause*` here, the thing you actually want is a value in section 3.
 *
 * The five invariants every element below serves (SESSION-API.md section 1):
 *
 *   1. Values only.        No engine type and no pointer into engine memory
 *                          ever crosses. Handles are opaque integers.
 *   2. Asynchronous.       Requests return an id; answers arrive as events.
 *                          Nothing here blocks, `describe()` excepted.
 *   3. Pull, not push.     Every unbounded producer is demand-driven.
 *                          Program output cannot be -- nobody demands a
 *                          `print` -- so it is budgeted instead (section 4.1).
 *   4. One ordered stream.  Monotonic `seq`, plus a per-query `querySeq` so
 *                          a front end can order one query's events without
 *                          depending on the global order surviving a
 *                          concurrent core.
 *   5. Generation-stamped. Every event carries the `worldGeneration` it was
 *                          produced under, so a view knows it is stale
 *                          without having to ask.
 *
 * Namespace note (2026-09-06): SESSION-API.md writes `namespace
 * unify::session`. The rest of this codebase lives in `vault::unify`, and a
 * second top-level `unify` namespace next to `vault::unify` would be a
 * genuine ambiguity trap inside `namespace vault`. So the boundary is
 * `vault::unify::session`, with `namespace unify_session` reserved for
 * nothing. SESSION-API.md carries a revision note recording this.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace vault {
namespace unify {
namespace session {

// ---------------------------------------------------------------------------
// 1. Handles
//
// Every handle is an opaque integer, session-scoped and monotonic. Nothing
// the front end holds can dangle, because nothing the front end holds is a
// pointer. This is invariant 1, and it is what makes a BEAM core or a socket
// a change of implementation rather than a change of every caller.
// ---------------------------------------------------------------------------

using RequestId = std::uint64_t;   //!< identifies one request and its answers
using QueryId   = std::uint64_t;   //!< identifies one running/retained query
using ModuleId  = std::uint64_t;   //!< identifies one loaded module
using Seq       = std::uint64_t;   //!< global event sequence, monotonic from 1
using Gen       = std::uint64_t;   //!< world generation (engine mutation count)

/** The value 0 is never a valid id; it means "none". */
constexpr RequestId kNoRequest = 0;
constexpr QueryId   kNoQuery   = 0;
constexpr ModuleId  kNoModule  = 0;

/**
 * The two demand-driven streams a query produces.
 *
 * They share one `demand` verb rather than having one each, because they
 * share one back-pressure problem: a producer that can outrun its consumer
 * without bound.
 */
enum class Stream {
    Solutions,
    Trace
};

// ---------------------------------------------------------------------------
// 2. The value model  (SESSION-API.md section 3)
//
// A self-contained tree. No interning, no back-references, no pointers into a
// world -- because on BEAM it is a copied message and over a socket it is
// bytes. It mirrors the engine's term kinds one-for-one, which is what makes
// the LocalSession conversion mechanical.
// ---------------------------------------------------------------------------

struct Value {
    enum class Kind {
        Atom,    //!< bare name; `name` holds it
        Int,     //!< `i`
        Float,   //!< `f`
        Str,     //!< `name` holds the text
        Var,     //!< unbound; `name` holds the *display* name, e.g. "$p"
        Cons,    //!< compound; `name` is the functor, `args` the arguments
        Array,   //!< `args`
        Map      //!< `pairs`
    };

    Kind        kind = Kind::Atom;
    std::string name;              //!< Atom | Str | Var | Cons functor
    std::int64_t i = 0;            //!< Int
    double       f = 0.0;          //!< Float

    std::vector<Value>                                  args;   //!< Cons | Array
    std::vector<std::pair<std::string, Value>>          pairs;  //!< Map

    /**
     * Load-bearing. A binding may be enormous; a 40-row screen cannot show it
     * and a socket should not carry it. The core truncates at the budget in
     * QueryOptions and marks the node; the front end renders an ellipsis and
     * calls inspect() to go deeper.
     *
     * A front end that ignores this field will silently show the user a
     * *wrong* term rather than a shortened one, so treat rendering it as
     * mandatory, not decorative.
     */
    bool truncated = false;
};

/**
 * How much of a value the core may serialise before it must truncate.
 *
 * Both limits are per-value, not per-event: a solution with twenty bindings
 * may legitimately carry twenty budgets' worth of tree.
 */
struct ValueBudget {
    std::uint32_t maxDepth = 8;      //!< nesting levels before truncation
    std::uint32_t maxNodes = 512;    //!< total nodes in one value
    std::uint32_t maxStringBytes = 4096;  //!< per Str/Atom leaf
};

/**
 * The address of a node inside a Value, as the index path from its root.
 *
 * An empty path is the root itself. For Map nodes the index is the index
 * into `pairs`, not a key lookup, so a path stays valid across a core that
 * does not preserve key order -- and stays a plain integer vector on the
 * wire.
 */
using ValuePath = std::vector<std::uint32_t>;

// ---------------------------------------------------------------------------
// 3. Program-level vocabulary
// ---------------------------------------------------------------------------

/**
 * A predicate's identity: what the browser lists and what `source`,
 * `undefine` and every diagnostic refer to.
 */
struct PredicateKey {
    std::string   name;
    std::uint32_t arity = 0;
    ModuleId      module = kNoModule;

    bool operator==( const PredicateKey& other ) const {
        return arity == other.arity
            && module == other.module
            && name == other.name;
    }
    bool operator!=( const PredicateKey& other ) const {
        return !( *this == other );
    }
    /** Ordered so a catalogue can be a std::map and a listing is stable. */
    bool operator<( const PredicateKey& other ) const {
        if ( module != other.module ) return module < other.module;
        if ( name   != other.name   ) return name   < other.name;
        return arity < other.arity;
    }
};

/**
 * Where a clause came from -- engine item E1.
 *
 * Today the engine infers "not user code" from a `__` name prefix and a
 * dynamic_cast (unify-repl.cpp). The catalogue, the source view, the image
 * writer and compaction all need this to be a fact rather than a heuristic,
 * which is why it is a field on the clause and not a guess made here.
 */
struct Origin {
    enum class Kind {
        Module,       //!< parsed from a module's source text
        Asserted,     //!< created at run time by assert/asserta/assertz
        Builtin,      //!< provided by the engine
        Synthesized,  //!< produced by desugaring: __fe__N, __for__N, __if__N
        Transcript    //!< typed by a human at a prompt
    };

    Kind        kind = Kind::Module;
    ModuleId    module = kNoModule;
    std::string file;             //!< empty for Asserted/Builtin/Transcript
    std::uint32_t line = 0;       //!< 1-based; 0 when unknown
    std::uint32_t column = 0;     //!< 1-based; 0 when unknown
    std::uint32_t endLine = 0;    //!< 1-based; 0 when unknown
};

/**
 * What `define` should do when the text defines a predicate that already
 * has clauses.
 *
 * The distinction between Append and ReplacePredicates is the difference
 * between a consult and an edit-and-save, and it is the whole reason
 * `define` can serve both a transcript line and a file: they are one
 * operation with different policies, not two operations.
 */
enum class OverwritePolicy {
    Append,             //!< add clauses after any existing ones (consult)
    ReplacePredicates,  //!< for each predicate the text defines, drop its
                        //!< existing clauses first (edit-and-save). Needs
                        //!< engine item E3.
    Fail                //!< refuse if any predicate the text defines exists
};

/** What a `listing` should include. Metadata only -- never source text. */
struct ListingFilter {
    std::optional<ModuleId> module;       //!< unset: every module
    std::string             namePrefix;   //!< empty: no name filter
    std::optional<std::uint32_t> arity;   //!< unset: any arity

    bool includeBuiltins = false;
    bool includeSynthesized = false;

    /**
     * Paging. The browser shows thousands of keys; `Listing.more` says
     * whether another page exists. 0 means "the core's default page size",
     * which `describe()` reports.
     */
    std::uint32_t limit = 0;
    std::uint32_t offset = 0;
};

/** One row of the catalogue -- engine item E2. */
struct CatalogueEntry {
    PredicateKey  key;
    Origin        origin;         //!< of the *first* clause; kind is the
                                  //!< predicate's kind
    std::uint32_t clauseCount = 0;
    Gen           generation = 0; //!< world generation of the last change
};

// ---------------------------------------------------------------------------
// 4. Query vocabulary
// ---------------------------------------------------------------------------

struct QueryOptions {
    /**
     * How many solutions to deliver without waiting for a `demand`.
     *
     * Non-zero means the first screenful arrives in one round trip, which is
     * the difference between a usable and an unusable remote session.
     */
    std::uint32_t initialDemand = 0;

    /** How many trace steps to deliver up front; only meaningful with trace. */
    std::uint32_t initialTraceDemand = 0;

    ValueBudget budget;

    /** Emit TraceEvents for this query. Requires Capabilities::debug. */
    bool trace = false;

    /**
     * Keep the query's solutions (and their term trees) after it finishes, so
     * `inspect` can expand a truncated value without re-running a goal that
     * may be nondeterministic or have side effects. Engine item E15.
     *
     * The cost is that the front end then owes a `release`. QueryStatus
     * reports `retained` so that debt is visible rather than assumed.
     */
    bool retain = true;
};

/** Options for writing an image. */
struct SaveOptions {
    bool includeAsserted = true;   //!< ground facts created at run time (E6)
    bool includeTranscript = false;//!< predicates defined at the prompt
    std::optional<ModuleId> onlyModule;
};

/** Severity of a Diagnostic. */
enum class Severity {
    Note,
    Warning,
    Error
};

/** One frame of a stack, reported on a breakpoint hit. */
struct StackFrame {
    PredicateKey key;
    std::uint32_t depth = 0;
    std::string   file;
    std::uint32_t line = 0;
};

// -- debug commands: one variant, one verb ---------------------------------

struct SetTrace        { QueryId query = kNoQuery; bool on = false; };
struct AddBreakpoint   { PredicateKey key; };
struct RemoveBreakpoint{ PredicateKey key; };
struct Step            { QueryId query = kNoQuery; };
struct StepOver        { QueryId query = kNoQuery; };
struct Continue        { QueryId query = kNoQuery; };
struct StackAt         { QueryId query = kNoQuery; };

using DebugCommand = std::variant<
    SetTrace,
    AddBreakpoint,
    RemoveBreakpoint,
    Step,
    StepOver,
    Continue,
    StackAt >;

// ---------------------------------------------------------------------------
// 5. Capabilities  (the one synchronous answer)
// ---------------------------------------------------------------------------

/**
 * What this core can do, answerable before any request is legal.
 *
 * A proxy answers this from the handshake it completed while connecting,
 * not from a round trip -- which is the only reason a synchronous call is
 * tolerable in an otherwise fully asynchronous interface.
 */
struct Capabilities {
    std::string coreName;      //!< e.g. "vault-unify-core"
    std::string coreVersion;
    std::string location;      //!< "local", or "remote:<host>" -- used to
                               //!< label image paths (SESSION-API.md 7)

    bool debug = false;            //!< xdebug backend present
    bool structuredSolutions = false; //!< E7 landed; false => Str leaves only
    bool realDemand = false;       //!< E11 landed; false => "buffered"
    bool realCancel = false;       //!< E8 landed; false => best-effort
    bool images = false;           //!< save/load/insert usable
    bool retention = false;        //!< E15 landed; false => inspect fails

    std::uint32_t defaultListingPageSize = 200;
    std::uint32_t resumeBufferEvents = 0;  //!< how far back subscribe() can go

    /** The core's builtin predicates, for the help extractor (G0 "H"). */
    std::vector<PredicateKey> builtins;

    /**
     * Diagnostics counters, exposed so the contract suite can assert on
     * leak-shaped properties (G0.10) rather than on timing.
     */
    std::uint64_t retainedQueries = 0;
    std::uint64_t liveRequests = 0;
};

// ---------------------------------------------------------------------------
// 6. Events  (SESSION-API.md section 4)
// ---------------------------------------------------------------------------

struct EventHeader {
    Seq seq = 0;                        //!< global, monotonic, gapless
    Gen worldGeneration = 0;

    /**
     * Set on every query-attributable event -- including Output and
     * Diagnostic.
     *
     * Today a single FIFO worker makes "the output that just arrived belongs
     * to the running query" accidentally true. On a concurrent core two
     * queries interleave their `print` output and it becomes unattributable,
     * and adding this field later would change the wire format of the two
     * highest-volume events. So it is here from the start.
     */
    std::optional<QueryId> query;

    /** Per-query subsequence, gapless, from 1. Meaningful iff `query` is set. */
    std::uint64_t querySeq = 0;
};

// -- request envelope ------------------------------------------------------

/** The core accepted a request. Always the first event for that request. */
struct Started { RequestId req = kNoRequest; };

/**
 * Infrastructure failure -- not a user error and not a query outcome.
 * `fatal` distinguishes "this request failed" from "this session is over".
 */
struct Failed {
    RequestId   req = kNoRequest;
    std::string reason;
    bool        fatal = false;
};

// -- program ---------------------------------------------------------------

struct Defined {
    RequestId                 req = kNoRequest;
    ModuleId                  module = kNoModule;
    std::vector<PredicateKey> added;
    std::vector<PredicateKey> replaced;
    std::vector<PredicateKey> removed;
    /**
     * Non-zero when the text had errors. The matching Diagnostics are
     * separate events: a bad `define` must produce both, never one alone.
     */
    std::uint32_t             errorCount = 0;
};

struct Listing {
    RequestId                   req = kNoRequest;
    std::vector<CatalogueEntry> entries;
    bool                        more = false;
};

struct SourceText {
    RequestId    req = kNoRequest;
    PredicateKey key;
    std::string  text;
    Origin       origin;
};

// -- queries ---------------------------------------------------------------

struct Solution {
    std::uint64_t index = 0;   //!< 0-based, in production order
    std::vector<std::pair<std::string, Value>> bindings;
};

struct QueryStatus {
    enum class State {
        Running,    //!< accepted; more may come on demand
        Exhausted,  //!< no more solutions exist
        Complete,   //!< the requested count was delivered
        Failed,     //!< the goal errored out
        Aborted,    //!< cancelled
        Blocked     //!< reserved for ROADMAP 5.3 async builtins; a v1 core
                    //!< never emits this
    };

    State         state = State::Running;
    std::uint64_t produced = 0;
    /** True while `inspect` is possible and a `release` is owed. */
    bool          retained = false;
    std::string   detail;
};

struct Expanded {
    RequestId req = kNoRequest;
    ValuePath path;
    Value     value;
};

// -- debug -----------------------------------------------------------------

struct TraceEvent {
    enum class Kind { Call, Exit, Redo, Fail, BreakpointHit };

    Kind          kind = Kind::Call;
    std::uint32_t depth = 0;
    PredicateKey  key;
    Value         goal;
    std::vector<StackFrame> stack;   //!< populated on BreakpointHit only
    bool          halted = false;    //!< execution is waiting for Step/Continue
};

// -- ambient ---------------------------------------------------------------

/**
 * Program output -- engine item E4.
 *
 * Exists because a remote engine's `print` sent to that process's stdout is
 * lost on another machine. Note that the default sink must still reproduce
 * today's byte-for-byte format or the engine goldens break.
 *
 * Output is the one unbounded producer that cannot be demanded (nobody
 * demands a `print`), so the core budgets it instead: it coalesces adjacent
 * writes and reports what it discarded. A front end renders `droppedBytes`
 * as an elision marker; ignoring it means silently lying about the program's
 * output.
 */
struct Output {
    std::string   stream;          //!< "stdout" or "stderr"
    std::string   text;
    std::uint64_t droppedBytes = 0;
};

/**
 * A user error: parse error, unknown predicate, UnifyError -- engine item
 * E10. Carries the offending source line, because the diagnostics panel
 * navigates to it and re-reading the file is not possible for a transcript
 * line or a remote core.
 */
struct Diagnostic {
    std::optional<RequestId> req;
    Severity      sev = Severity::Error;
    std::string   file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string   message;
    std::string   sourceLine;
};

/** One signal every view can invalidate on, instead of each panel polling. */
struct WorldChanged {
    Gen           generation = 0;
    std::uint32_t clauseCount = 0;
    std::uint32_t predicateCount = 0;
};

// -- the event ------------------------------------------------------------

using EventBody = std::variant<
    Started,
    Failed,
    Defined,
    Listing,
    SourceText,
    Solution,
    QueryStatus,
    Expanded,
    TraceEvent,
    Output,
    Diagnostic,
    WorldChanged >;

struct Event {
    EventHeader header;
    EventBody   body;
};

/**
 * Where events are delivered.
 *
 * Called on the session's own thread, never on the caller's. An
 * implementation of this interface must not block: it is expected to push
 * onto a queue the UI thread drains. This is the rule in ARCHITECTURE.md
 * section 3 -- the session thread never touches the model.
 */
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void onEvent( const Event& event ) = 0;
};

// ---------------------------------------------------------------------------
// 7. The interface
// ---------------------------------------------------------------------------

/**
 * Fifteen requests, twelve events, one value model.
 *
 * Every method except describe() returns immediately with an id; the answer
 * arrives on the event stream. Deliberately so even for LocalSession, which
 * could answer synchronously: the extra indirection is the price of the
 * abstraction being real rather than nominal, and it is what lets one
 * contract suite run against an in-process core, a fake, and a socket.
 */
class Session {
public:
    virtual ~Session() = default;

    // -- lifecycle ---------------------------------------------------------

    /** The sole synchronous call; see Capabilities. */
    virtual Capabilities describe() const = 0;

    /**
     * Release every retained query, stop delivering events, and detach.
     * Idempotent. After close(), every request answers Failed{fatal=true}.
     */
    virtual void close() = 0;

    /**
     * Begin delivery to `sink`.
     *
     * `resumeFrom` is part of the interface rather than a proxy-private
     * trick: pass the last `seq` seen to resume after a dropped connection.
     * If the core's bounded buffer no longer reaches back that far it
     * answers Failed{"resume point expired"}, after which the front end owes
     * itself a full refresh. Both paths are exercised by FakeSession.
     *
     * `resumeFrom == 0` means "from now on".
     */
    virtual void subscribe( EventSink& sink, Seq resumeFrom = 0 ) = 0;

    // -- program -----------------------------------------------------------

    /**
     * The single ingestion path. A transcript line, a file, an edited
     * predicate and an `insert` are one operation with different Origin and
     * OverwritePolicy; collapsing them is most of what keeps this API small.
     */
    virtual RequestId define( std::string text,
                              Origin origin,
                              OverwritePolicy policy ) = 0;

    /** Engine item E3. "Define nothing for this key" cannot express deletion. */
    virtual RequestId undefine( PredicateKey key, ModuleId scope ) = 0;

    /** Catalogue metadata only -- never source text. */
    virtual RequestId listing( ListingFilter filter ) = 0;

    /** One definition's text: a rarer, much larger payload than a listing. */
    virtual RequestId source( PredicateKey key ) = 0;

    // -- queries -----------------------------------------------------------

    virtual QueryId   solve( std::string goalText, QueryOptions options ) = 0;

    /** Ask for `n` more items of one stream. Invariant 3. */
    virtual RequestId demand( QueryId query, Stream stream, std::uint32_t n ) = 0;

    /**
     * Best-effort in v1: it always produces a terminal QueryStatus so the
     * front end can detach, but with a single-worker engine and no bounded
     * slices the work itself keeps running. The UI must say so rather than
     * imply otherwise -- plan section 6.
     */
    virtual RequestId cancel( QueryId query ) = 0;

    /**
     * Drop a retained query and its term trees. Without this, a session
     * leaks one retained query per query run.
     */
    virtual RequestId release( QueryId query ) = 0;

    /**
     * Expand a truncated node of a delivered solution. Re-running the query
     * to see a term is wrong: queries are nondeterministic and may have side
     * effects. Requires Capabilities::retention.
     */
    virtual RequestId inspect( QueryId query,
                               std::uint64_t solutionIndex,
                               ValuePath path,
                               ValueBudget budget ) = 0;

    // -- image -------------------------------------------------------------
    //
    // Paths resolve on the CORE's filesystem. With a remote core an image
    // saved from a front end lands on the engine's machine; the front end
    // labels the path using Capabilities::location. Streaming file contents
    // across this boundary instead would add a file-transfer concern to the
    // API and make `save` on a large world a front-end memory problem.
    // Moving images between machines is scp's job.

    virtual RequestId save  ( std::string path, SaveOptions options ) = 0;
    virtual RequestId load  ( std::string path ) = 0;
    virtual RequestId insert( std::string path, OverwritePolicy policy ) = 0;

    // -- debug -------------------------------------------------------------

    /**
     * One verb over a command variant rather than seven methods. A core
     * reporting `debug: false` must answer Failed here -- never crash.
     */
    virtual RequestId debug( DebugCommand command ) = 0;
};

using SessionPtr = std::shared_ptr<Session>;

} // namespace session
} // namespace unify
} // namespace vault

#endif // _VAULT_UNIFY_SESSION_HPP
