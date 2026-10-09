#include "robot_pm/decision_drive.hpp"

#include "robot_pm/audit.hpp"
#include "robot_pm/crypto.hpp"
#include "robot_pm/durable.hpp"
#include "robot_pm/feishu_access.hpp"
#include "robot_pm/frame.hpp"

#include <fstream>
#include <sstream>
#include <utility>

namespace robot_pm {
namespace {

using json = nlohmann::json;

constexpr const char* kEncryptKey = "encrypt-key";
constexpr const char* kOldDecision = "d0";
constexpr const char* kNewDecision = "d1";

[[nodiscard]] json state_n_body() {
    return json{{"generation", 1}, {"status", "todo"},     {"feedback", kFeedbackOld}, {"decision", kOldDecision},
                {"table", "todo"}, {"calendar", ""},       {"card", kFeedbackOld}};
}

[[nodiscard]] json state_n1_body() {
    return json{{"generation", 2},
                {"status", "doing"},
                {"feedback", kFeedbackNew},
                {"decision", kNewDecision},
                {"table", "doing"},
                {"calendar", "evt-1"},
                {"card", kFeedbackNew}};
}

[[nodiscard]] std::filesystem::path log_path(const std::filesystem::path& dir) { return dir / "decisions.jsonl"; }

[[nodiscard]] std::filesystem::path journal_path(const std::filesystem::path& dir) { return dir / "journal"; }

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] bool already_committed(const std::filesystem::path& dir, std::string_view decision) {
    for (const json& record : read_durable_bytes(read_file(log_path(dir)))) {
        if (record.value("decision", "") == decision) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] DecisionView view_of(const json& body) {
    DecisionView view;
    view.present = true;
    view.generation = body.value("generation", 0);
    view.feedback = body.value("feedback", "");
    view.status = body.value("status", "");
    view.decision = body.value("decision", "");
    view.table = body.value("table", "");
    view.calendar = body.value("calendar", "");
    view.card = body.value("card", "");
    return view;
}

[[nodiscard]] std::optional<DecisionView> read_committed(const std::filesystem::path& dir, bool stop_early) {
    const std::string bytes = read_file(log_path(dir));
    if (bytes.size() > 1 && stop_early) {
        return std::nullopt;
    }
    const std::vector<json> records = read_durable_bytes(bytes);
    if (records.empty()) {
        return std::nullopt;
    }
    return view_of(records.back());
}

[[nodiscard]] json inner_event() {
    return json{{"header", {{"event_type", "im.message.receive_v1"}, {"token", "verify-token"}}},
                {"event",
                 {{"message",
                   {{"chat_type", "p2p"},
                    {"message_type", "text"},
                    {"content", "{\"text\":\"改一下\"}"}}}}}};
}

[[nodiscard]] std::string hook_request(std::string& signature_out) {
    const std::string plain = inner_event().dump();
    const std::string cipher = encrypt_feishu_payload(kEncryptKey, plain);
    const std::string raw_body = json{{"encrypt", cipher}}.dump();
    signature_out = feishu_event_signature("1710000000", "nonce-1", kEncryptKey, raw_body);
    std::string http = "POST /hook HTTP/1.1\r\n";
    http += "X-Lark-Request-Timestamp: 1710000000\r\n";
    http += "X-Lark-Request-Nonce: nonce-1\r\n";
    http += "X-Lark-Signature: " + signature_out + "\r\n";
    http += "Content-Length: " + std::to_string(raw_body.size()) + "\r\n\r\n";
    http += raw_body;
    return http;
}

[[nodiscard]] bool same_signature(const HttpFrame& frame) {
    if (frame.timestamp.empty() || frame.nonce.empty() || frame.signature.empty()) {
        return false;
    }
    const std::string expected =
            feishu_event_signature(frame.timestamp, frame.nonce, kEncryptKey, frame.body);
    return expected == frame.signature;
}

}  // namespace

const char* decision_cut_name(DecisionCut cut) {
    switch (cut) {
        case DecisionCut::kWebhookBeforeSignature:
            return "webhook-before-signature";
        case DecisionCut::kFramingNotFinished:
            return "framing-not-finished";
        case DecisionCut::kWebhookAfterSignatureBeforeBody:
            return "webhook-after-signature-before-body";
        case DecisionCut::kWebhookMiddleOfBody:
            return "webhook-middle-of-body";
        case DecisionCut::kFramingDoneBadSignature:
            return "framing-done-bad-signature";
        case DecisionCut::kSignatureMatches:
            return "signature-matches";
        case DecisionCut::kModelRequestFraming:
            return "model-request-framing";
        case DecisionCut::kModelResponseFraming:
            return "model-response-framing";
        case DecisionCut::kModelStreamBeforeEnd:
            return "model-stream-before-end";
        case DecisionCut::kLocalWrite:
            return "local-write";
        case DecisionCut::kDiskFull:
            return "disk-full";
        case DecisionCut::kWriteFailed:
            return "write-failed";
        case DecisionCut::kLocalStoreSync:
            return "local-store-sync";
        case DecisionCut::kRead:
            return "read";
        case DecisionCut::kReadStopped:
            return "read-stopped";
        case DecisionCut::kFeedbackBeforeFirstByte:
            return "feedback-before-first-byte";
        case DecisionCut::kFeedbackMiddle:
            return "feedback-middle";
        case DecisionCut::kAfterSendBeforeLocalCommit:
            return "after-send-before-local-commit";
        case DecisionCut::kAfterLocalCommitBeforeAck:
            return "after-local-commit-before-ack";
    }
    return "unknown";
}

void plant_state_n(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    static_cast<void>(replace_file(log_path(dir), seal_commit(state_n_body())));
    std::error_code error;
    std::filesystem::remove(journal_path(dir), error);
}

DriveResult run_decision(const std::filesystem::path& dir, std::optional<DecisionCut> cut) {
    DriveResult result;
    const auto stop = [&](DecisionCut here) { return cut.has_value() && *cut == here; };

    if (already_committed(dir, kNewDecision)) {
        return result;
    }

    std::string signature;
    const std::string http = hook_request(signature);
    const std::size_t header_end = http.find("\r\n\r\n");
    const std::string headers = http.substr(0, header_end + 4);
    const std::string raw_body = http.substr(header_end + 4);

    if (stop(DecisionCut::kWebhookBeforeSignature)) {
        const std::size_t mark = http.find("X-Lark-Signature: ");
        const std::string partial = http.substr(0, mark + std::string("X-Lark-Signature: ").size() + 4);
        const HttpFrame frame = read_http_frame(partial, true);
        result.violated = frame.kind == FrameKind::kComplete;
        return result;
    }
    if (stop(DecisionCut::kFramingNotFinished)) {
        const std::string lying = "POST /hook HTTP/1.1\r\nContent-Length: " + std::to_string(raw_body.size() + 8) +
                                  "\r\n\r\n" + raw_body;
        const HttpFrame frame = read_http_frame(lying, true);
        result.violated = frame.kind == FrameKind::kComplete;
        return result;
    }
    if (stop(DecisionCut::kWebhookAfterSignatureBeforeBody)) {
        const HttpFrame frame = read_http_frame(headers, true);
        result.violated = frame.kind == FrameKind::kComplete;
        return result;
    }
    if (stop(DecisionCut::kWebhookMiddleOfBody)) {
        const std::string midway = headers + raw_body.substr(0, raw_body.size() / 2);
        const HttpFrame frame = read_http_frame(midway, true);
        result.violated = frame.kind == FrameKind::kComplete;
        return result;
    }

    const HttpFrame full = read_http_frame(http, true);
    if (full.kind != FrameKind::kComplete) {
        result.violated = true;
        return result;
    }
    if (stop(DecisionCut::kFramingDoneBadSignature)) {
        std::string bad = http;
        const std::size_t pos = bad.find(signature);
        if (pos != std::string::npos) {
            bad[pos] = bad[pos] == 'a' ? 'b' : 'a';
        }
        const HttpFrame framed = read_http_frame(bad, true);
        result.violated = framed.kind != FrameKind::kComplete || same_signature(framed);
        return result;
    }
    if (!same_signature(full)) {
        result.violated = true;
        return result;
    }
    if (stop(DecisionCut::kSignatureMatches)) {
        return result;
    }

    const json wrapper = json::parse(full.body, nullptr, false);
    if (wrapper.is_discarded() || !wrapper.contains("encrypt") || !wrapper.at("encrypt").is_string()) {
        result.violated = true;
        return result;
    }
    const std::expected<std::string, Error> plain =
            decrypt_feishu_payload(kEncryptKey, wrapper.at("encrypt").get_ref<const std::string&>());
    if (!plain) {
        result.violated = true;
        return result;
    }

    const std::string model_req = "POST /v1/chat HTTP/1.1\r\nContent-Length: " + std::to_string(plain->size()) +
                                  "\r\n\r\n" + *plain;
    if (stop(DecisionCut::kModelRequestFraming)) {
        const std::string cut_req = model_req.substr(0, model_req.size() - plain->size() / 2);
        const HttpFrame frame = read_http_frame(cut_req, true);
        result.violated = frame.kind == FrameKind::kComplete;
        return result;
    }
    if (read_http_frame(model_req, true).kind != FrameKind::kComplete) {
        result.violated = true;
        return result;
    }

    const std::string data_line = std::string("data: ") + state_n1_body().dump() + "\n\n";
    const std::string sse = data_line + "data: [DONE]\n";
    if (stop(DecisionCut::kModelResponseFraming)) {
        const std::string partial = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(sse.size()) + "\r\n\r\n" +
                                    data_line;
        const HttpFrame frame = read_http_frame(partial, true);
        result.violated = frame.kind == FrameKind::kComplete || model_reply_text(data_line).has_value();
        return result;
    }
    if (stop(DecisionCut::kModelStreamBeforeEnd)) {
        const std::string only = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(data_line.size()) +
                                 "\r\n\r\n" + data_line;
        const HttpFrame framed = read_http_frame(only, true);
        const std::optional<std::string> reply = model_reply_text(framed.body);
        result.violated = framed.kind != FrameKind::kComplete || reply.has_value();
        return result;
    }
    const std::string model_resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(sse.size()) + "\r\n\r\n" +
                                   sse;
    const HttpFrame model_full = read_http_frame(model_resp, true);
    const std::optional<std::string> reply = model_full.kind == FrameKind::kComplete ? model_reply_text(model_full.body)
                                                                                    : std::nullopt;
    if (!reply) {
        result.violated = true;
        return result;
    }
    const json parsed = json::parse(*reply, nullptr, false);
    if (parsed.is_discarded() || parsed.value("feedback", "") != kFeedbackNew ||
        parsed.value("card", "") != kFeedbackNew) {
        result.violated = true;
        return result;
    }
    const std::string pending = seal_commit(state_n1_body());

    if (stop(DecisionCut::kLocalWrite) || stop(DecisionCut::kDiskFull) || stop(DecisionCut::kWriteFailed)) {
        return result;
    }
    if (stop(DecisionCut::kLocalStoreSync)) {
        const std::string torn = pending.substr(0, pending.size() - 1);
        static_cast<void>(replace_file(journal_path(dir), torn));
        return result;
    }

    const std::optional<DecisionView> read_view = read_committed(dir, false);
    if (stop(DecisionCut::kRead)) {
        if (read_view) {
            result.read_returned = true;
            result.read_view = *read_view;
        }
        return result;
    }
    if (stop(DecisionCut::kReadStopped)) {
        const std::optional<DecisionView> stopped = read_committed(dir, true);
        result.read_returned = stopped.has_value();
        result.violated = stopped.has_value();
        return result;
    }

    std::string outbox;
    if (stop(DecisionCut::kFeedbackBeforeFirstByte)) {
        return result;
    }
    if (stop(DecisionCut::kFeedbackMiddle)) {
        outbox = std::string(kFeedbackNew).substr(0, 3);
        static_cast<void>(outbox);
        return result;
    }
    if (stop(DecisionCut::kAfterSendBeforeLocalCommit)) {
        outbox = kFeedbackNew;
        static_cast<void>(outbox);
        return result;
    }

    std::string kept;
    for (const json& record : read_durable_bytes(read_file(log_path(dir)))) {
        kept += seal_commit(record);
    }
    kept += pending;
    if (!replace_file(log_path(dir), kept)) {
        result.violated = true;
        return result;
    }
    if (stop(DecisionCut::kAfterLocalCommitBeforeAck)) {
        return result;
    }
    return result;
}

DecisionView reopen_decision(const std::filesystem::path& dir, DecisionPerson& person) {
    static_cast<void>(recover_durable_file(log_path(dir)));
    const std::vector<json> records = read_durable_bytes(read_file(log_path(dir)));
    if (records.empty()) {
        return {};
    }
    const DecisionView view = view_of(records.back());
    if (view.decision == kNewDecision) {
        if (person.seen.insert(view.decision).second) {
            person.table += 1;
            person.calendar += 1;
            person.card += 1;
        }
    }
    if (view.feedback == kFeedbackOld || view.feedback == kFeedbackNew) {
        person.feedback = view.feedback;
    }
    return view;
}

}  // namespace robot_pm
