#include "utils/secure_random.hpp"

#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "Bcrypt.lib")
#elif defined(__APPLE__)
#include <Security/SecRandom.h>
#else
#include <sys/random.h>
#include <cerrno>
#endif

namespace agentos {

void SecureRandomBytes(std::uint8_t* buffer, std::size_t length) {
    if (length == 0) {
        return;
    }
    if (buffer == nullptr) {
        throw std::runtime_error("SecureRandomBytes: null buffer");
    }

#if defined(_WIN32)
    const NTSTATUS status = ::BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(buffer),
        static_cast<ULONG>(length),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        throw std::runtime_error(
            "SecureRandomBytes: BCryptGenRandom failed with status " + std::to_string(status));
    }
#elif defined(__APPLE__)
    const int rc = ::SecRandomCopyBytes(kSecRandomDefault, length, buffer);
    if (rc != 0) {
        throw std::runtime_error(
            "SecureRandomBytes: SecRandomCopyBytes failed with rc " + std::to_string(rc));
    }
#else
    std::size_t filled = 0;
    while (filled < length) {
        const ssize_t got = ::getrandom(buffer + filled, length - filled, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                std::string("SecureRandomBytes: getrandom failed: ") + std::strerror(errno));
        }
        filled += static_cast<std::size_t>(got);
    }
#endif
}

std::vector<std::uint8_t> SecureRandomBytes(std::size_t length) {
    std::vector<std::uint8_t> bytes(length);
    SecureRandomBytes(bytes.data(), length);
    return bytes;
}

}  // namespace agentos
