#pragma once

#include <stdexcept>
#include <string>

namespace cb {

// Exit code contract (see README.md, "ClipBank 退出码"):
//   clipbank: 0 ok, 2 usage/config, 3 missing dependency,
//             4 input/decode, 5 internal/DB, 6 verify failed.
class AppError : public std::runtime_error {
public:
    explicit AppError(std::string message) : std::runtime_error(std::move(message)) {}
    virtual int exit_code() const noexcept = 0;
};

class UsageError : public AppError {
public:
    using AppError::AppError;
    int exit_code() const noexcept override { return 2; }
};

class DependencyError : public AppError {
public:
    using AppError::AppError;
    int exit_code() const noexcept override { return 3; }
};

class InputError : public AppError {
public:
    using AppError::AppError;
    int exit_code() const noexcept override { return 4; }
};

class DbError : public AppError {
public:
    using AppError::AppError;
    int exit_code() const noexcept override { return 5; }
};

class InternalError : public AppError {
public:
    using AppError::AppError;
    int exit_code() const noexcept override { return 5; }
};

class VerifyError : public AppError {
public:
    using AppError::AppError;
    int exit_code() const noexcept override { return 6; }
};

} // namespace cb
