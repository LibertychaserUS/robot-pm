#include "robot_pm/error.hpp"

namespace robot_pm {

std::string_view error_code_name(ErrorCode code) {
    switch (code) {
    case ErrorCode::kConfigMissing:
        return "kConfigMissing";
    case ErrorCode::kUnknownEvent:
        return "kUnknownEvent";
    case ErrorCode::kForbidden:
        return "kForbidden";
    case ErrorCode::kUnknownField:
        return "kUnknownField";
    case ErrorCode::kUnsupportedType:
        return "kUnsupportedType";
    case ErrorCode::kEditRejected:
        return "kEditRejected";
    case ErrorCode::kBridgeFailed:
        return "kBridgeFailed";
    }
    return "kBridgeFailed";
}

}  // namespace robot_pm
