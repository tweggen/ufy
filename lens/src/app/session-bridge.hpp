#if !defined( _LENS_APP_SESSION_BRIDGE_HPP )
#define _LENS_APP_SESSION_BRIDGE_HPP

/**
 * @file session-bridge.hpp
 *
 * Between the model's `CommandRequest`s and a real `Session`.
 *
 * The model returns requests rather than issuing them (UI.md section 6), so
 * something has to turn "the transcript wants to solve this text" into a
 * call on the boundary and the resulting events back into something the
 * model can fold. That is all this does, and it lives in `app/` because it
 * is the only place that is allowed to know both sides exist.
 *
 * It also owns the queue between the session thread and the UI thread. The
 * rule from ARCHITECTURE.md section 3 is that the session thread never
 * touches the model: events land in a mutex-guarded deque here, and the UI
 * thread drains it when it is ready. Nothing in `model/` ever sees a thread.
 */

#include "../model/model.hpp"

#include "vault-unify-local-session.hpp"
#include "vault-unify-session.hpp"

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lens {

class SessionBridge {
public:
    SessionBridge();
    ~SessionBridge();

    /** Start the engine and subscribe. Returns an error string, or empty. */
    std::string start();

    /** What the core says it can do, for the model to tell the truth with. */
    const us::Capabilities& capabilities() const { return m_caps; }

    /** Issue one request. Unknown kinds are reported, never ignored. */
    void issue( Model& model, const CommandRequest& request );

    /** Move every event received so far out of the queue. */
    std::vector<us::Event> drain();

    /**
     * Block until the engine is quiet and every event has been delivered.
     *
     * For `--script` only, where a golden screen has to be a function of the
     * script rather than of how fast the machine is. The interactive loop
     * never calls it -- a UI that waited for the engine would be the design
     * ARCHITECTURE.md section 3.1 rejects.
     */
    void settle();

private:
    class Sink;

    std::unique_ptr<vault::unify::session::LocalSession> m_session;
    std::unique_ptr<Sink> m_sink;
    us::Capabilities m_caps;

    mutable std::mutex m_mutex;
    std::deque<us::Event> m_events;
};

} // namespace lens

#endif // _LENS_APP_SESSION_BRIDGE_HPP
