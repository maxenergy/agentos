#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace agentos {

// Pair of short-lived files that hold a curl request body and/or a set of
// `-H` header lines. Storing these payloads in temp files (and passing
// `--data @file` / `-H @file` to curl) keeps tokens, refresh tokens, and
// `Authorization: Bearer ...` headers out of the process command line so
// they cannot be observed via `/proc/<pid>/cmdline` or Windows process
// listings.
//
// The destructor unconditionally attempts to remove both files using
// `std::filesystem::remove` with a `std::error_code`, so cleanup is
// best-effort and never throws — even from exception unwinding.
struct CurlSecretFiles {
    // Empty path means "no body"/"no extra headers" — caller must skip the
    // corresponding `--data @...` / `-H @...` argv pair in that case.
    std::filesystem::path body_file;
    std::filesystem::path headers_file;

    CurlSecretFiles() = default;

    // Non-copyable: the destructor owns deletion. Move is allowed so the
    // helper can be returned by value from `WriteCurlSecretFiles`.
    CurlSecretFiles(const CurlSecretFiles&) = delete;
    CurlSecretFiles& operator=(const CurlSecretFiles&) = delete;
    CurlSecretFiles(CurlSecretFiles&& other) noexcept;
    CurlSecretFiles& operator=(CurlSecretFiles&& other) noexcept;

    ~CurlSecretFiles();
};

// Writes `body_contents` and `header_lines` to short-lived files under
// `<workspace_path>/runtime/.tmp/`. File names are derived from 16 random
// bytes drawn from the platform CSPRNG (via `SecureRandomBytes`) and
// hex-encoded.
//
// If `body_contents` is empty no body file is written; if `header_lines`
// is empty no headers file is written. The header file is built by joining
// the lines with "\r\n" — curl interprets `-H @file` line-by-line.
//
// Throws `std::runtime_error` if the temp directory cannot be created or
// the files cannot be written. Files written before the failure are
// cleaned up by the destructor of the returned object.
CurlSecretFiles WriteCurlSecretFiles(
    const std::filesystem::path& workspace_path,
    const std::string& body_contents,
    const std::vector<std::string>& header_lines);

}  // namespace agentos
