#include "cad/kernel/Result.h"

namespace cad::kernel {

const char* toString(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::Ok:              return "Ok";
        case ErrorCode::InvalidInput:    return "InvalidInput";
        case ErrorCode::NotDone:         return "NotDone";
        case ErrorCode::InvalidResult:   return "InvalidResult";
        case ErrorCode::BooleanFailed:   return "BooleanFailed";
        case ErrorCode::Unsupported:     return "Unsupported";
        case ErrorCode::NamingLost:      return "NamingLost";
        case ErrorCode::KernelException: return "KernelException";
        case ErrorCode::Cancelled:       return "Cancelled";
        case ErrorCode::Internal:        return "Internal";
    }
    return "Unknown";
}

}  // namespace cad::kernel
