#if !defined( _LENS_MODREG_COMMAND_HPP )
#define _LENS_MODREG_COMMAND_HPP

/**
 * @file command.hpp
 *
 * The command table -- UI.md section 6.
 *
 * One table underlies all four help surfaces and every binding. Menus, the
 * hint line, `M-x` and the generated help page are four RENDERINGS of this
 * one table, not four lists that have to be kept in agreement. That is the
 * mechanism by which UI.md section 5 stays true as the environment grows:
 * a module that registers a command gets a hint, a menu entry, a palette
 * entry and a help entry for free, and cannot get three of the four.
 *
 * "A command with no help text is a build-time failure." Taken literally
 * that is not expressible for data registered at run time, so it is
 * enforced in the two places where it can be:
 *
 *   - the constructor REQUIRES id, title and help, so a command with no
 *     help text cannot be written down; and
 *   - registration rejects an empty or whitespace-only help string, which
 *     the module test asserts (G1 "H").
 *
 * The second is what catches `Command( "x", "X", "" )`. Together they are
 * as close to a compile-time guarantee as a runtime registry gets, and the
 * gate says so rather than claiming more.
 */

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace lens {

class Model;

/**
 * A request the command wants issued against the core.
 *
 * Deliberately an id plus arguments rather than a session request object:
 * `model/` and the command table must not depend on the session boundary's
 * types, so that "what did this keystroke ask the engine to do?" stays an
 * assertable VALUE in a test with no engine present. The composition root
 * translates these into real requests.
 */
struct CommandRequest {
    std::string kind;   //!< "solve", "define", "listing", ...
    std::string text;   //!< goal or program text, where applicable
    std::uint64_t handle = 0;   //!< query id, where applicable
};

/**
 * One command.
 *
 * `run` returns requests rather than issuing them (UI.md section 6, and
 * ARCHITECTURE.md section 4's rule that a panel never calls the session
 * directly). That keeps `fold` free of I/O and makes every command testable
 * by inspecting what it asked for.
 */
class Command {
public:
    using Predicate = std::function<bool( const Model& )>;
    using Handler   = std::function<std::vector<CommandRequest>( Model& )>;

    /**
     * All three strings are constructor parameters and none has a default,
     * so a command without help text cannot be written. See the file
     * comment for why that is only half the mechanism.
     */
    Command( std::string id, std::string title, std::string help );

    const std::string& id()    const { return m_id; }
    const std::string& title() const { return m_title; }
    const std::string& help()  const { return m_help; }

    /** Optional link into the help hypertext. */
    const std::string& helpTopic() const { return m_helpTopic; }
    Command& helpTopic( std::string topic );

    /**
     * Which menu-bar heading this belongs under -- UI.md section 1's
     * File / Edit / World / Query / Image / Debug / Window / Help.
     *
     * Defaulted from the id's namespace rather than required, so a module
     * that registers `query.cancel` lands under Query without being asked.
     * A command whose namespace matches no heading falls to File, which is
     * where a general-purpose action belongs.
     */
    const std::string& category() const { return m_category; }
    Command& category( std::string category );

    /**
     * Whether the command applies right now. Commands are never hidden --
     * a disabled command still appears in `M-x` and in help, greyed, so
     * discovering that something exists does not require it to be usable
     * at that moment.
     */
    Command& enabledWhen( Predicate predicate );
    bool enabled( const Model& model ) const;

    Command& onRun( Handler handler );
    std::vector<CommandRequest> run( Model& model ) const;

private:
    std::string m_id;
    std::string m_title;
    std::string m_help;
    std::string m_helpTopic;
    std::string m_category;
    Predicate   m_enabled;
    Handler     m_run;
};


class CommandTable {
public:
    /**
     * Add a command.
     *
     * @return an empty string on success, or why it was rejected: a
     *     duplicate id, or a missing id, title or help text. Returned
     *     rather than thrown because the composition root registers dozens
     *     at startup and wants to report all the problems, not the first.
     */
    std::string add( const Command& command );

    const Command* find( const std::string& id ) const;

    /** Every command, in registration order. */
    const std::vector<Command>& all() const { return m_commands; }

    std::size_t size() const { return m_commands.size(); }

private:
    std::vector<Command> m_commands;
    std::map<std::string, std::size_t> m_byId;
};

} // namespace lens

#endif // _LENS_MODREG_COMMAND_HPP
