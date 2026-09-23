#include "marketscan/live/recorder.hpp"

#include <stdexcept>

#include <nlohmann/json.hpp>

#include "marketscan/util/version.hpp"

namespace marketscan::live {
namespace {

/// 2 since Session F2, when recordings started carrying the venue side.
/// Version 1 files are still readable: they simply have no submissions and no
/// responses, which is the signal a replay uses to fall back to the simulator.
constexpr int kRecordingVersion = 2;
constexpr int kOldestReadableVersion = 1;
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
    head["run_token"] = header.run_token;

    out_ << head.dump() << '\n';
    out_.flush();
}

StreamRecorder::~StreamRecorder() {
    close();
}

void StreamRecorder::write_line(const std::string& text) {
    // Caller holds the lock.
    if (!out_.is_open()) {
        return;
    }
    out_ << text << '\n';
    // Flushed every line on purpose: the recordings worth having are the ones
    // whose session ended badly.
    out_.flush();
    ++sequence_;
    bytes_ += text.size() + 1;
}

void StreamRecorder::record(std::string_view raw, Timestamp recv) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!out_.is_open()) {
        return;
    }
    nlohmann::json line;
    line["seq"] = sequence_;
    line["recv"] = recv;
    line["raw"] = raw;
    write_line(line.dump());
}

void StreamRecorder::record_response(std::string_view raw, Timestamp recv) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!out_.is_open()) {
        return;
    }
    nlohmann::json line;
    line["seq"] = sequence_;
    line["recv"] = recv;
    line["response"] = raw;
    write_line(line.dump());
}

void StreamRecorder::record_submission(std::string_view client_order_id,
                                       OrderId engine_id,
                                       std::string_view symbol,
                                       Side side,
                                       MS_OrderType type,
                                       MS_TimeInForce tif,
                                       Quantity quantity,
                                       Price limit_price,
                                       std::uint64_t strategy_client_id,
                                       bool accepted,
                                       std::string_view reason,
                                       Timestamp at) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!out_.is_open()) {
        return;
    }
    nlohmann::json submit;
    submit["client"] = client_order_id;
    submit["engine_id"] = engine_id;
    submit["symbol"] = symbol;
    submit["side"] = side == MS_SIDE_SELL ? "sell" : "buy";
    submit["type"] = type == MS_ORD_LIMIT ? "limit" : "market";
    submit["tif"] = tif == MS_TIF_IOC ? "ioc" : (tif == MS_TIF_GTC ? "gtc" : "day");
    submit["qty"] = quantity;
    submit["limit"] = limit_price;
    submit["strategy_client_id"] = strategy_client_id;
    submit["accepted"] = accepted;
    if (!reason.empty()) {
        submit["reason"] = reason;
    }

    nlohmann::json line;
    line["seq"] = sequence_;
    line["recv"] = at;
    line["submit"] = std::move(submit);
    write_line(line.dump());
}

void StreamRecorder::record_reconnect(Timestamp at,
                                      std::uint32_t attempt,
                                      std::string_view reason) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!out_.is_open()) {
        return;
    }
    nlohmann::json line;
    line["seq"] = sequence_;
    line["recv"] = at;
    line["reconnect"] = true;
    line["attempt"] = attempt;
    line["reason"] = reason;
    write_line(line.dump());
}

void StreamRecorder::close() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

namespace {

[[nodiscard]] std::string text_field(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

[[nodiscard]] double number_field(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) {
        return 0.0;
    }
    return it->get<double>();
}

}  // namespace

RecordingContents read_recording_contents(const std::filesystem::path& path) {
    RecordingContents out;

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
    out.header.version = head.value(kMagicField, 0);
    if (out.header.version < kOldestReadableVersion || out.header.version > kRecordingVersion) {
        throw std::runtime_error("recording version " + std::to_string(out.header.version) +
                                 " but this build reads versions " +
                                 std::to_string(kOldestReadableVersion) + " to " +
                                 std::to_string(kRecordingVersion) + ": " + path.string());
    }
    out.header.host = head.value("host", std::string{});
    out.header.path = head.value("path", std::string{});
    out.header.feed = head.value("feed", std::string{});
    out.header.started = head.value("started", Timestamp{0});
    out.header.engine_version = head.value("engine_version", std::string{});
    out.header.run_token = head.value("run_token", std::string{});
    if (const auto it = head.find("symbols"); it != head.end() && it->is_array()) {
        for (const auto& s : *it) {
            if (s.is_string()) {
                out.header.symbols.push_back(s.get<std::string>());
            }
        }
    }

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
                                     " is not JSON. Refusing to skip it: a replay that drops a "
                                     "line disagrees silently with the session it is reproducing.");
        }

        const std::uint64_t seq = doc.value("seq", std::uint64_t{0});
        const Timestamp recv = doc.value("recv", Timestamp{0});

        // An order this process sent.
        if (const auto submit = doc.find("submit"); submit != doc.end() && submit->is_object()) {
            RecordedSubmission record;
            record.sequence = seq;
            record.received = recv;
            record.client_order_id = text_field(*submit, "client");
            record.engine_id = submit->value("engine_id", OrderId{0});
            record.symbol = text_field(*submit, "symbol");
            record.side = text_field(*submit, "side") == "sell" ? MS_SIDE_SELL : MS_SIDE_BUY;
            record.type = text_field(*submit, "type") == "limit" ? MS_ORD_LIMIT : MS_ORD_MARKET;
            const std::string tif = text_field(*submit, "tif");
            record.tif = tif == "ioc" ? MS_TIF_IOC : (tif == "gtc" ? MS_TIF_GTC : MS_TIF_DAY);
            record.quantity = number_field(*submit, "qty");
            record.limit_price = number_field(*submit, "limit");
            record.strategy_client_id = submit->value("strategy_client_id", std::uint64_t{0});
            record.accepted = submit->value("accepted", false);
            record.reason = text_field(*submit, "reason");
            out.submissions.push_back(std::move(record));
            continue;
        }

        // Something the venue said about an order.
        if (const auto response = doc.find("response");
            response != doc.end() && response->is_string()) {
            out.responses.push_back(RecordedResponse{seq, recv, response->get<std::string>()});
            continue;
        }

        RecordedMessage message;
        message.sequence = seq;
        message.received = recv;
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
        out.messages.push_back(std::move(message));
    }

    return out;
}

std::vector<RecordedMessage> read_recording(const std::filesystem::path& path,
                                            RecordingHeader& header) {
    // A thin wrapper, so that every caller which only wants market data keeps
    // working and there is still one parser for the file.
    RecordingContents contents = read_recording_contents(path);
    header = std::move(contents.header);
    return std::move(contents.messages);
}

}  // namespace marketscan::live
