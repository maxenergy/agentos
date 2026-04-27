#include "utils/curl_secret.hpp"

#include "utils/secure_random.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace agentos {

namespace {

constexpr std::size_t kRandomNameBytes = 16;

std::string HexEncode(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        output.push_back(kHexDigits[(byte >> 4) & 0x0F]);
        output.push_back(kHexDigits[byte & 0x0F]);
    }
    return output;
}

std::filesystem::path EnsureTempDirectory(const std::filesystem::path& workspace_path) {
    std::filesystem::path tmp_dir;
    if (workspace_path.empty()) {
        tmp_dir = std::filesystem::temp_directory_path() / "agentos" / ".tmp";
    } else {
        tmp_dir = workspace_path / "runtime" / ".tmp";
    }

    std::error_code ec;
    std::filesystem::create_directories(tmp_dir, ec);
    if (ec) {
        throw std::runtime_error(
            "WriteCurlSecretFiles: failed to create temp directory " + tmp_dir.string() +
            ": " + ec.message());
    }
    return tmp_dir;
}

void WriteFileContents(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("WriteCurlSecretFiles: could not open file for write: " + path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("WriteCurlSecretFiles: could not write file: " + path.string());
    }
}

void RemoveBestEffort(const std::filesystem::path& path) noexcept {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
}

}  // namespace

CurlSecretFiles::CurlSecretFiles(CurlSecretFiles&& other) noexcept
    : body_file(std::move(other.body_file)),
      headers_file(std::move(other.headers_file)) {
    other.body_file.clear();
    other.headers_file.clear();
}

CurlSecretFiles& CurlSecretFiles::operator=(CurlSecretFiles&& other) noexcept {
    if (this != &other) {
        RemoveBestEffort(body_file);
        RemoveBestEffort(headers_file);
        body_file = std::move(other.body_file);
        headers_file = std::move(other.headers_file);
        other.body_file.clear();
        other.headers_file.clear();
    }
    return *this;
}

CurlSecretFiles::~CurlSecretFiles() {
    RemoveBestEffort(body_file);
    RemoveBestEffort(headers_file);
}

CurlSecretFiles WriteCurlSecretFiles(
    const std::filesystem::path& workspace_path,
    const std::string& body_contents,
    const std::vector<std::string>& header_lines) {
    CurlSecretFiles files;

    if (body_contents.empty() && header_lines.empty()) {
        return files;
    }

    const auto tmp_dir = EnsureTempDirectory(workspace_path);

    if (!body_contents.empty()) {
        const auto name = "curl-body-" + HexEncode(SecureRandomBytes(kRandomNameBytes));
        files.body_file = tmp_dir / name;
        WriteFileContents(files.body_file, body_contents);
    }

    if (!header_lines.empty()) {
        std::ostringstream joined;
        for (std::size_t i = 0; i < header_lines.size(); ++i) {
            joined << header_lines[i];
            // curl reads headers from `-H @file` line by line; both LF and CRLF
            // are accepted, but CRLF is the on-the-wire convention.
            joined << "\r\n";
        }
        const auto name = "curl-headers-" + HexEncode(SecureRandomBytes(kRandomNameBytes));
        files.headers_file = tmp_dir / name;
        WriteFileContents(files.headers_file, joined.str());
    }

    return files;
}

}  // namespace agentos
