#pragma once

#include <string>
#include <utility>

namespace kvforge {

enum class StatusCode {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kIOError = 3,
    kInvalidArgument = 4
};

class Status {
public:
    Status() : code_(StatusCode::kOk) {}
    Status(StatusCode code, std::string msg)
        : code_(code), message_(std::move(msg)) {}

    static Status Ok() { return Status(); }
    static Status NotFound(const std::string& msg = "") {
        return Status(StatusCode::kNotFound, msg);
    }
    static Status Corruption(const std::string& msg) {
        return Status(StatusCode::kCorruption, msg);
    }
    static Status IOError(const std::string& msg) {
        return Status(StatusCode::kIOError, msg);
    }
    static Status InvalidArgument(const std::string& msg) {
        return Status(StatusCode::kInvalidArgument, msg);
    }

    bool ok() const { return code_ == StatusCode::kOk; }
    bool IsNotFound() const { return code_ == StatusCode::kNotFound; }
    bool IsCorruption() const { return code_ == StatusCode::kCorruption; }
    bool IsIOError() const { return code_ == StatusCode::kIOError; }
    StatusCode code() const { return code_; }
    const std::string& message() const { return message_; }

    std::string ToString() const {
        const char* prefix = "OK";
        switch (code_) {
            case StatusCode::kOk: prefix = "OK"; break;
            case StatusCode::kNotFound: prefix = "NotFound"; break;
            case StatusCode::kCorruption: prefix = "Corruption"; break;
            case StatusCode::kIOError: prefix = "IOError"; break;
            case StatusCode::kInvalidArgument: prefix = "InvalidArgument"; break;
        }
        if (message_.empty()) {
            return prefix;
        }
        return std::string(prefix) + ": " + message_;
    }

private:
    StatusCode code_;
    std::string message_;
};

}  // namespace kvforge
