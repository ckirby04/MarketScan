#include "chat.hpp"

#include <algorithm>

namespace marketscan::app {

namespace {

using nlohmann::json;

[[nodiscard]] std::string text_or_empty(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

ChatItem chat_item_from_json(const json& item) {
    ChatItem out;
    out.kind = item.at("kind").get<std::string>();
    out.text = text_or_empty(item, "text");
    out.job_id = text_or_empty(item, "job_id");
    out.message = text_or_empty(item, "message");
    if (out.kind == "questions") {
        out.answered = item.value("answered", false);
        for (const json& q : item.at("questions")) {
            ChatQuestion question;
            question.question = q.at("question").get<std::string>();
            question.allow_free_text = q.value("allow_free_text", true);
            for (const json& o : q.at("options")) {
                question.options.push_back(
                    ChatOption{o.at("label").get<std::string>(), text_or_empty(o, "hint")});
            }
            out.questions.push_back(std::move(question));
        }
    }
    return out;
}

std::vector<ConversationSummary> conversations_from_json(const json& list) {
    std::vector<ConversationSummary> out;
    for (const json& c : list) {
        out.push_back(ConversationSummary{
            c.at("id").get<std::string>(), text_or_empty(c, "title"), c.value("updated_at", 0.0)});
    }
    return out;
}

void load_conversation(ChatView& view, const json& conversation) {
    view.id = conversation.at("id").get<std::string>();
    view.title = text_or_empty(conversation, "title");
    view.ready = conversation.value("ready", false);
    view.analysed = conversation.value("analysed", std::vector<std::string>{});
    view.items.clear();
    for (const json& item : conversation.at("display")) {
        view.items.push_back(chat_item_from_json(item));
    }
}

void add_sent(ChatView& view, const std::string& text) {
    ChatItem item;
    item.kind = "user";
    item.text = text;
    item.sent = true;
    view.items.push_back(std::move(item));
}

void close_live(ChatView& view) {
    for (ChatItem& item : view.items) {
        item.live = false;
    }
}

void apply_event(ChatView& view, const json& event, TurnKind kind, const std::string& job_id) {
    const std::string type = text_or_empty(event, "event");
    if (type == "activity") {
        view.activity = text_or_empty(event, "text");
        return;
    }
    if (type == "text") {
        view.activity.clear();
        const std::string delta = text_or_empty(event, "delta");
        if (!view.items.empty()) {
            ChatItem& last = view.items.back();
            if (last.live && (last.kind == "assistant" || last.kind == "analysis")) {
                last.text += delta;
                return;
            }
        }
        ChatItem item;
        item.kind = kind == TurnKind::Analysis ? "analysis" : "assistant";
        item.text = delta;
        item.job_id = kind == TurnKind::Analysis ? job_id : std::string();
        item.live = true;
        view.items.push_back(std::move(item));
        return;
    }
    if (type == "item") {
        close_live(view);
        ChatItem item = chat_item_from_json(event.at("item"));
        if (item.kind == "user") {
            // The service's copy of the message on screen replaces the page's.
            view.items.erase(std::remove_if(view.items.begin(),
                                            view.items.end(),
                                            [](const ChatItem& shown) { return shown.sent; }),
                             view.items.end());
        }
        view.items.push_back(std::move(item));
        return;
    }
    if (type == "error") {
        close_live(view);
        ChatItem item;
        item.kind = "error";
        item.text = text_or_empty(event, "message");
        view.items.push_back(std::move(item));
        return;
    }
    if (type == "form") {
        // Never shown; read for whether Run can be offered.
        view.ready = event.value("ready", false);
        return;
    }
    // "end", and anything a newer service adds.
}

std::vector<std::string> run_ids(const ChatView& view) {
    std::vector<std::string> out;
    for (const ChatItem& item : view.items) {
        if (item.kind == "run" && !item.job_id.empty()) {
            out.push_back(item.job_id);
        }
    }
    return out;
}

bool analysed(const ChatView& view, const std::string& job_id) {
    return std::find(view.analysed.begin(), view.analysed.end(), job_id) != view.analysed.end();
}

}  // namespace marketscan::app
