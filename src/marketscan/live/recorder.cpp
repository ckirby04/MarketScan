#include "marketscan/live/recorder.hpp"

#include <stdexcept>

#include <nlohmann/json.hpp>

#include "marketscan/util/version.hpp"

namespace marketscan::live {
namespace {

constexpr int kRecordingVersion = 1;
constexpr const char* kMagicField = "marketscan_recording";

}  // namespace

StreamRecorder::StreamRecorder(const std::filesystem::path& path, const RecordingHeader& header)
    : path_(path) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    // Binary, so that the '\n' this writes is one byte on every platform. A
    // recording read back with a different line ending than it was written
    // with is a recording whose byte offsets no longer mean anything.
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_.is_open()) {
        throw std::runtime_error("cannot open recording for writing: " + path.string());
    }

    nlohmann::json head;
    head[kMagicField] = kRecordingVersion;
    head["host"] = header.host;
    head["path"] = header.path;
    head["feed"] = header.feed;
    head["symbols"] = header.symbols;
    head["started"] = header.started;
    head["engine_version"] =
        header.engine_version.empty() ? std::string(version::kString) : header.engine_version;

    out_ << head.dump() << '\n';
    out_.flush();
}

StreamRecorder::~StreamRecorder() {
    close();
}

void StreamRecorder::record(std::string_view raw, Timestamp recv) {
    if (!out_.is_open()) {
        return;
    }
    nlohmann::json line;
    line["seq"] = sequence_;
    line["recv"] = recv;
    line["raw"] = raw;

    const std::string text = line.dump();
    out_ << text << '\n';
    // Flushed every message on purpose: the recordings worth having are the
    // ones whose session ended badly.
    out_.flush();

    ++sequence_;
    bytes_ += text.size() + 1;
}

void StreamRecorder::record_reconnect(Timestamp at,
                                      std::uint32_t attempt,
                                      std::string_view reason) {
    if (!out_.is_open()) {
        return;
    }
    nlohmann::json line;
    line["seq"] = sequence_;
    line["recv"] = at;
    line["reconnect"] = true;
    line["attempt"] = attempt;
    line["reason"] = reason;

    const std::string text = line.dump();
    out_ << text << '\n';
    out_.flush();

    ++sequence_;
    bytes_ += text.size() + 1;
}

void StreamRecorder::close() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

std::vector<RecordedMessage> read_recording(const std::filesystem::path& path,
                                            RecordingHeader& header) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("cannot open recording: " + path.string());
    }

    std::string line;
    if (!std::getline(in, line)) {
        throw std::runtime_error("recording is empty: " + path.string());
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    const nlohmann::json head = nlohmann::json::parse(line, nullptr, false);
    if (head.is_discarded() || !head.is_object() || !head.contains(kMagicField)) {
        throw std::runtime_error("not a MarketScan recording: " + path.string());
    }
    header.version = head.value(kMagicField, 0);
    if (header.version != kRecordingVersion) {
        throw std::runtime_error("recording version " + std::to_string(header.version) +
                                 " but this build reads version " +
                                 std::to_string(kRecordingVersion) + ": " + path.string());
    }
    header.host = head.value("host", std::string{});
    header.path = head.value("path", std::string{});
    header.feed = head.value("feed", std::string{});
    header.started = head.value("started", Timestamp{0});
    header.engine_version = head.value("engine_version", std::string{});
    header.symbols.clear();
    if (const auto it = head.find("symbols"); it != head.end() && it->is_array()) {
        for (const auto& s : *it) {
            if (s.is_string()) {
                header.symbols.push_back(s.get<std::string>());
            }
        }
    }

    std::vector<RecordedMessage> messages;
    std::uint64_t line_number = 1;
    while (std::getline(in, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        const nlohmann::json doc = nlohmann::json::parse(line, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) {
            throw std::runtime_error("recording " + path.string() + " line " +
                                     std::to_string(line_number) +
                                     " is not JSON. Refusing to "
                                     "skip it: a replay that drops a message disagrees silently "
                                     "with the session it is reproducing.");
        }

        RecordedMessage message;
        message.sequence = doc.value("seq", std::uint64_t{0});
        message.received = doc.value("recv", Timestamp{0});
        if (doc.value("reconnect", false)) {
            message.reconnect = true;
            message.attempt = doc.value("attempt", std::uint32_t{0});
            message.reason = doc.value("reason", std::string{});
        } else {
            const auto raw = doc.find("raw");
            if (raw == doc.end() || !raw->is_string()) {
                throw std::runtime_error("recording " + path.string() + " line " +
                                         std::to_string(line_number) + " has no raw message");
            }
            message.raw = raw->get<std::string>();
        }
        messages.push_back(std::move(message));
    }

    return messages;
}

}  // namespace marketscan::live
