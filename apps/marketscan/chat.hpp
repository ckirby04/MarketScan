#ifndef MARKETSCAN_APP_CHAT_HPP
#define MARKETSCAN_APP_CHAT_HPP

/*
 * A conversation with the job service's assistant, as the Research view shows
 * it (docs/specs/SPEC_DESKTOP_CHAT.md section 4).
 *
 * Plain data and the rules for changing it, with no thread and no network, so
 * the recorded turns in data/samples/research/chat/ can drive it directly in a
 * test. ResearchSession's chat worker owns the network and applies these.
 *
 * The rules are the web app's (web/src/components/Chat.tsx), so both clients
 * show a turn the same way: text deltas grow the last item while it is live,
 * any other event closes it, the service's copy of a sent message replaces the
 * one shown at once, and when a turn ends the conversation is reloaded from the
 * service, so what is on screen afterwards is the service's record, not a
 * reconstruction of it.
 *
 * The form never appears: `form` events are read only for whether the
 * backtest is ready to run.
 */

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace marketscan::app {

struct ChatOption {
    std::string label;
    std::string hint;
};

struct ChatQuestion {
    std::string question;
    std::vector<ChatOption> options;
    bool allow_free_text = true;
};

/// One thing in the conversation, as the service's `display` lists it.
struct ChatItem {
    /// user | assistant | analysis | questions | ready | run | error
    std::string kind;
    /// user, assistant, analysis and error.
    std::string text;
    /// analysis and run.
    std::string job_id;
    /// questions: what the assistant said before asking.
    std::string message;
    std::vector<ChatQuestion> questions;
    bool answered = false;

    /// Text still streaming in.
    bool live = false;
    /// The user's message as typed, shown until the service's copy arrives.
    bool sent = false;
};

/// Reads one item of the service's `display`, or of an `item` event. Throws
/// nlohmann::json::exception on one not shaped like it.
[[nodiscard]] ChatItem chat_item_from_json(const nlohmann::json& item);

struct ConversationSummary {
    std::string id;
    std::string title;
    double updated_at = 0.0;
};

[[nodiscard]] std::vector<ConversationSummary> conversations_from_json(const nlohmann::json& list);

/// The open conversation.
struct ChatView {
    /// Empty for a new conversation not yet sent.
    std::string id;
    std::string title;
    std::vector<ChatItem> items;
    /// The assistant has checked the form and the backtest can be run.
    bool ready = false;
    /// Runs the assistant has already read and analysed.
    std::vector<std::string> analysed;

    /// A turn is streaming.
    bool busy = false;
    /// What a tool call is doing, while it does it.
    std::string activity;
    /// Why the last command did not work, in one sentence. Cleared by the next.
    std::string problem;
};

/// Replaces the view's record with the service's: items, title, readiness,
/// analysed runs. Keeps busy, activity and problem, which are the client's.
void load_conversation(ChatView& view, const nlohmann::json& conversation);

/// Shows a message the moment it is sent, before the service has it.
void add_sent(ChatView& view, const std::string& text);

/// Which item a turn's text belongs to.
enum class TurnKind { Assistant, Analysis };

/// Applies one event of a streamed turn. `job_id` is the run an analysis turn is
/// about. Unknown events are ignored, so a newer service does not break an older
/// client.
void apply_event(ChatView& view,
                 const nlohmann::json& event,
                 TurnKind kind,
                 const std::string& job_id = {});

/// Closes any live item, as the end of a turn does.
void close_live(ChatView& view);

/// The job ids of the view's runs, in order.
[[nodiscard]] std::vector<std::string> run_ids(const ChatView& view);

[[nodiscard]] bool analysed(const ChatView& view, const std::string& job_id);

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_CHAT_HPP
