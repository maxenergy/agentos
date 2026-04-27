#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace agentos {

// Fills `bytes` with cryptographically-secure random bytes from the platform CSPRNG.
// Backends:
//   Windows: BCryptGenRandom (BCRYPT_USE_SYSTEM_PREFERRED_RNG)
//   Linux:   getrandom(2)
//   macOS:   SecRandomCopyBytes
// Throws std::runtime_error on platform-CSPRNG failure (no silent fallback).
void SecureRandomBytes(std::uint8_t* buffer, std::size_t length);

std::vector<std::uint8_t> SecureRandomBytes(std::size_t length);

}  // namespace agentos
