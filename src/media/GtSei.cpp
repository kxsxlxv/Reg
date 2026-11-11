#include "media/GtSei.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reg::media {
namespace {

struct Json {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    Type type{Type::Null};
    bool boolean{};
    std::string scalar;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    const Json* member(std::string_view name) const noexcept {
        if (type != Type::Object) return nullptr;
        const auto it = object.find(std::string(name));
        return it == object.end() ? nullptr : &it->second;
    }
};

bool validUtf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first < 0x80U) { ++i; continue; }
        int length = 0;
        std::uint32_t value = 0;
        if (first >= 0xC2U && first <= 0xDFU) { length = 2; value = first & 0x1FU; }
        else if (first >= 0xE0U && first <= 0xEFU) { length = 3; value = first & 0x0FU; }
        else if (first >= 0xF0U && first <= 0xF4U) { length = 4; value = first & 0x07U; }
        else return false;
        if (i + static_cast<std::size_t>(length) > text.size()) return false;
        for (int k = 1; k < length; ++k) {
            const auto ch = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if ((ch & 0xC0U) != 0x80U) return false;
            value = (value << 6U) | (ch & 0x3FU);
        }
        if ((length == 2 && value < 0x80U) ||
            (length == 3 && value < 0x800U) ||
            (length == 4 && value < 0x10000U) ||
            value > 0x10FFFFU ||
            (value >= 0xD800U && value <= 0xDFFFU)) return false;
        i += static_cast<std::size_t>(length);
    }
    return true;
}

void appendUtf8(std::string& text, std::uint32_t cp) {
    if (cp <= 0x7FU) text.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FFU) {
        text.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        text.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0xFFFFU) {
        text.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        text.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        text.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        text.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}
    bool parse(Json& result) {
        skipSpace();
        if (!value(result, 0)) return false;
        skipSpace();
        return pos_ == text_.size();
    }

private:
    std::string_view text_;
    std::size_t pos_{};

    void skipSpace() noexcept {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                text_[pos_] == '\r' || text_[pos_] == '\n')) ++pos_;
    }
    bool take(char ch) noexcept {
        if (pos_ >= text_.size() || text_[pos_] != ch) return false;
        ++pos_;
        return true;
    }
    bool literal(std::string_view word) noexcept {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }
    bool hex4(std::uint32_t& out) noexcept {
        if (text_.size() - pos_ < 4U) return false;
        out = 0;
        for (int n = 0; n < 4; ++n) {
            const char ch = text_[pos_++];
            std::uint32_t digit = 0;
            if (ch >= '0' && ch <= '9') digit = static_cast<std::uint32_t>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') digit = static_cast<std::uint32_t>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') digit = static_cast<std::uint32_t>(ch - 'A' + 10);
            else return false;
            out = (out << 4U) | digit;
        }
        return true;
    }
    bool string(std::string& out) {
        if (!take('"')) return false;
        while (pos_ < text_.size()) {
            const auto ch = static_cast<unsigned char>(text_[pos_++]);
            if (ch == '"') return validUtf8(out);
            if (ch < 0x20U) return false;
            if (ch != '\\') { out.push_back(static_cast<char>(ch)); continue; }
            if (pos_ == text_.size()) return false;
            switch (text_[pos_++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800U && cp <= 0xDBFFU) {
                    if (!take('\\') || !take('u')) return false;
                    std::uint32_t low = 0;
                    if (!hex4(low) || low < 0xDC00U || low > 0xDFFFU) return false;
                    cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low - 0xDC00U);
                } else if (cp >= 0xDC00U && cp <= 0xDFFFU) return false;
                appendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool number(std::string& out) {
        const std::size_t begin = pos_;
        static_cast<void>(take('-'));
        if (take('0')) {
            // A leading zero must not be followed by more integer digits.
            if (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') return false;
        } else {
            if (pos_ >= text_.size() || text_[pos_] < '1' || text_[pos_] > '9') return false;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (take('.')) {
            const std::size_t digits = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (digits == pos_) return false;
        }
        if (take('e') || take('E')) {
            if (!take('+')) static_cast<void>(take('-'));
            const std::size_t digits = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (digits == pos_) return false;
        }
        out.assign(text_.substr(begin, pos_ - begin));
        return true;
    }
    bool value(Json& out, unsigned depth) {
        if (depth > 24U) return false;
        skipSpace();
        if (pos_ == text_.size()) return false;
        if (text_[pos_] == '"') {
            out.type = Json::Type::String;
            return string(out.scalar);
        }
        if (take('{')) {
            out.type = Json::Type::Object;
            skipSpace();
            if (take('}')) return true;
            do {
                skipSpace();
                std::string key;
                if (!string(key)) return false;
                skipSpace();
                if (!take(':')) return false;
                Json next;
                if (!value(next, depth + 1U)) return false;
                if (!out.object.emplace(std::move(key), std::move(next)).second) return false;
                skipSpace();
                if (take('}')) return true;
            } while (take(','));
            return false;
        }
        if (take('[')) {
            out.type = Json::Type::Array;
            skipSpace();
            if (take(']')) return true;
            do {
                Json next;
                if (!value(next, depth + 1U)) return false;
                out.array.push_back(std::move(next));
                skipSpace();
                if (take(']')) return true;
            } while (take(','));
            return false;
        }
        if (literal("null")) { out.type = Json::Type::Null; return true; }
        if (literal("true")) { out.type = Json::Type::Boolean; out.boolean = true; return true; }
        if (literal("false")) { out.type = Json::Type::Boolean; out.boolean = false; return true; }
        out.type = Json::Type::Number;
        return number(out.scalar);
    }
};

bool asUint64(const Json* node, std::uint64_t& result) noexcept {
    if (node == nullptr || node->type != Json::Type::Number ||
        node->scalar.empty() ||
        node->scalar.find_first_not_of("0123456789") != std::string::npos) return false;
    const char* begin = node->scalar.data();
    const char* end = begin + node->scalar.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool asDimension(const Json* node, std::uint32_t& result) noexcept {
    std::uint64_t value = 0;
    if (!asUint64(node, value) || value == 0U ||
        value > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) return false;
    result = static_cast<std::uint32_t>(value);
    return true;
}

bool asDouble(const Json& node, double& result) noexcept {
    if (node.type != Json::Type::Number) return false;
    const char* begin = node.scalar.data();
    const char* end = begin + node.scalar.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(result);
}

bool asBox(const Json* node, std::array<double, 4>& xyxy) noexcept {
    if (node == nullptr || node->type != Json::Type::Array ||
        node->array.size() != 4U) return false;
    for (std::size_t index = 0; index < 4U; ++index) {
        if (!asDouble(node->array[index], xyxy[index])) return false;
    }
    return xyxy[2] > xyxy[0] && xyxy[3] > xyxy[1];
}

GtMetadata failed() { return GtMetadata{.status = GtStatus::ParseError}; }

} // namespace

const char* gtStatusName(GtStatus status) noexcept {
    switch (status) {
    case GtStatus::Missing: return "missing";
    case GtStatus::Ready: return "ready";
    case GtStatus::NotReady: return "not_ready";
    case GtStatus::Busy: return "busy";
    case GtStatus::AmbiguousViewport: return "ambiguous_viewport";
    case GtStatus::Unavailable: return "unavailable";
    case GtStatus::ParseError: return "parse_error";
    case GtStatus::FrameMismatch: return "frame_mismatch";
    }
    return "unknown";
}

GtMetadata parseGtJson(std::span<const std::uint8_t> json) noexcept {
    if (json.empty() || json.size() > kMaxGtJsonBytes) return failed();
    try {
        Json root;
        const auto bytes = std::string_view(
            reinterpret_cast<const char*>(json.data()), json.size());
        if (!Parser(bytes).parse(root) || root.type != Json::Type::Object) return failed();

        const Json* type = root.member("type");
        const Json* version = root.member("version");
        std::uint64_t versionNumber = 0;
        if (type == nullptr || type->type != Json::Type::String ||
            type->scalar != "KGT1" ||
            !asUint64(version, versionNumber) || versionNumber != 1U) return failed();

        const Json* status = root.member("status");
        if (status == nullptr || status->type != Json::Type::String) return failed();

        GtMetadata gt;
        if (status->scalar == "not_ready") gt.status = GtStatus::NotReady;
        else if (status->scalar == "busy") gt.status = GtStatus::Busy;
        else if (status->scalar == "ambiguous_viewport") gt.status = GtStatus::AmbiguousViewport;
        else if (status->scalar != "ready") gt.status = GtStatus::Unavailable;
        else gt.status = GtStatus::Ready;
        if (gt.status != GtStatus::Ready) return gt;

        const Json* videoSize = root.member("video_size");
        const Json* objects = root.member("objects");
        if (!asUint64(root.member("stream_epoch"), gt.source.key.streamEpoch) ||
            !asUint64(root.member("frame_id"), gt.source.key.frameId) ||
            !asUint64(root.member("source_time_ns"), gt.source.sourceTimeNs) ||
            videoSize == nullptr || videoSize->type != Json::Type::Array ||
            videoSize->array.size() != 2U ||
            !asDimension(&videoSize->array[0], gt.videoWidth) ||
            !asDimension(&videoSize->array[1], gt.videoHeight) ||
            objects == nullptr || objects->type != Json::Type::Array) return failed();

        if (objects->array.size() > 256U) return failed();
        gt.objects.reserve(objects->array.size());
        for (const auto& item : objects->array) {
            const Json* id = item.member("id");
            const Json* visible = item.member("visible");
            if (id == nullptr || id->type != Json::Type::String ||
                id->scalar.size() > 128U ||
                visible == nullptr || visible->type != Json::Type::Boolean) return failed();
            GtObject object;
            object.id = id->scalar;
            object.visible = visible->boolean;
            // An invisible object may have null/absent boxes.
            if (object.visible) {
                object.validBox = asBox(item.member("xyxy_video"), object.xyxyVideo);
            }
            gt.objects.push_back(std::move(object));
        }
        return gt;
    } catch (...) {
        // Never let a malformed SEI or allocation failure stop video decoding.
        return failed();
    }
}

GtMetadata extractGtSei(const AVFrame* frame) noexcept {
    if (frame == nullptr) return {};
    try {
        GtMetadata gt;
        bool found = false;
        for (int i = 0; i < frame->nb_side_data; ++i) {
            const AVFrameSideData* side = frame->side_data[i];
            if (side == nullptr || side->type != AV_FRAME_DATA_SEI_UNREGISTERED ||
                side->size < kGtSeiUuid.size()) continue;
            if (!std::equal(kGtSeiUuid.begin(), kGtSeiUuid.end(), side->data)) continue;
            if (found) return failed();
            found = true;
            gt = parseGtJson(std::span<const std::uint8_t>(
                side->data + kGtSeiUuid.size(), side->size - kGtSeiUuid.size()));
        }
        return gt;
    } catch (...) {
        return failed();
    }
}

void validateGtForFrame(
    GtMetadata& gt,
    const std::optional<SourceFrameIdentity>& rgf1,
    int decodedWidth,
    int decodedHeight) noexcept {
    if (gt.status != GtStatus::Ready) return;
    if (!rgf1 ||
        gt.source.key != rgf1->key ||
        gt.source.sourceTimeNs != rgf1->sourceTimeNs ||
        decodedWidth <= 0 || decodedHeight <= 0 ||
        gt.videoWidth != static_cast<std::uint32_t>(decodedWidth) ||
        gt.videoHeight != static_cast<std::uint32_t>(decodedHeight)) {
        gt.objects.clear();
        gt.status = GtStatus::FrameMismatch;
    }
}

} // namespace reg::media
