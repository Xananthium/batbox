#include <batbox/conversation/ToolOutputArchive.hpp>
#include <batbox/core/Json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace batbox::conversation {
namespace {

std::string sha256(std::string_view content) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_Digest(content.data(), content.size(), bytes.data(), &size,
                   EVP_sha256(), nullptr) != 1 || size != 32)
        throw std::runtime_error("Cannot hash tool output");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (unsigned int i = 0; i < size; ++i) {
        result += hex[bytes[i] >> 4];
        result += hex[bytes[i] & 15];
    }
    return result;
}

void sync_directory(const std::filesystem::path& directory) {
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("Cannot open tool archive directory");
    const int result = ::fsync(fd);
    const int saved_errno = errno;
    ::close(fd);
    if (result != 0)
        throw std::runtime_error("Cannot sync tool archive directory: " + std::string(std::strerror(saved_errno)));
}

void publish(const std::filesystem::path& path, std::string_view bytes) {
    // Matching content-addressed artifacts are immutable and reusable.
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (!ec && std::filesystem::exists(status)) {
        if (!std::filesystem::is_regular_file(status))
            throw std::runtime_error("Tool archive destination is not a regular file");
        if (std::filesystem::file_size(path) != bytes.size())
            throw std::runtime_error("Existing tool archive size mismatch");
        std::ifstream file(path, std::ios::binary);
        const std::string old((std::istreambuf_iterator<char>(file)), {});
        if (old != bytes) throw std::runtime_error("Existing tool archive content mismatch");
        std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_write);
        return;
    }
    if (ec && ec != std::errc::no_such_file_or_directory)
        throw std::runtime_error("Cannot inspect tool archive destination");
    std::string temporary = path.string() + ".tmp.XXXXXX";
    int fd = ::mkstemp(temporary.data()); // mode0600; same directory as final file
    if (fd < 0) throw std::runtime_error("Cannot create tool archive temporary file");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("Cannot write tool archive");
            offset += static_cast<std::size_t>(n);
        }
        if (::fsync(fd) != 0) throw std::runtime_error("Cannot sync tool archive");
        const int close_result = ::close(fd);
        fd = -1;
        if (close_result != 0) throw std::runtime_error("Cannot close tool archive");
        // A racing publisher can only publish the same content/hash. Rename
        // exposes a complete file, never a partially written page.
        if (::rename(temporary.c_str(), path.c_str()) != 0)
            throw std::runtime_error("Cannot publish tool archive");
    } catch (...) {
        if (fd >= 0) ::close(fd);
        ::unlink(temporary.c_str());
        throw;
    }
}

std::size_t utf8_boundary(std::string_view text, std::size_t offset) {
    while (offset > 0 && offset < text.size() &&
           (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) --offset;
    return offset;
}

std::size_t page_end(std::string_view content, std::size_t start) {
    auto end = std::min(content.size(), start + kToolOutputArchivePageBytes);
    if (end < content.size()) {
        const auto newline = content.rfind('\n', end - 1);
        if (newline != std::string_view::npos && newline >= start + kToolOutputArchivePageBytes / 2)
            end = newline + 1;
        else
            end = utf8_boundary(content, end);
    }
    if (end <= start) throw std::runtime_error("Cannot split tool archive page");
    return end;
}

} // namespace

Result<std::optional<std::string>> archive_tool_output(
        std::string_view content, const std::filesystem::path& directory) {
    if (content.size() < kToolOutputArchiveThreshold) return std::optional<std::string>{};
    try {
        const auto root = std::filesystem::absolute(directory).lexically_normal();
        const bool created = std::filesystem::create_directories(root);
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(root)))
            throw std::runtime_error("Tool archive root is not a directory");
        std::filesystem::permissions(root, std::filesystem::perms::owner_all);
        if (created) {
            sync_directory(root.parent_path());
        }
        const auto hash = sha256(content);
        const auto raw_path = root / (hash + ".txt");
        const auto manifest_path = root / (hash + ".pages.json");
        publish(raw_path, content);
        const auto lines = std::count(content.begin(), content.end(), '\n') + (content.back() != '\n');
        Json pages = Json::array();
        std::size_t start = 0, page = 0;
        while (start < content.size()) {
            const auto end = page_end(content, start);
            const auto path = root / (hash + ".page-" + std::to_string(++page) + ".txt");
            const auto bytes = content.substr(start, end - start);
            publish(path, bytes);
            pages.push_back({{"file_path", path.string()}, {"byte_offset", start},
                             {"bytes", bytes.size()}, {"sha256", sha256(bytes)}});
            start = end;
        }
        const Json manifest = {{"format", "batbox-tool-output-v1"}, {"file_path", raw_path.string()},
            {"sha256", hash}, {"bytes", content.size()}, {"lines", lines},
            {"page_bytes_max", kToolOutputArchivePageBytes},
            {"page_note", "Pages are exact consecutive byte ranges. Concatenation reproduces the original; long lines may span pages."},
            {"pages", pages}};
        publish(manifest_path, manifest.dump(2) + "\n");
        sync_directory(root);

        const auto head_end = utf8_boundary(content, 12 * 1024);
        const auto tail_start = utf8_boundary(content, content.size() - 4 * 1024);
        std::string view = "[Large tool output archived; bounded context view]\nFull result: " + raw_path.string() +
            "\nSHA256: " + hash + "\nOriginal bytes: " + std::to_string(content.size()) +
            "; original lines: " + std::to_string(lines) +
            "\nThe complete tool result remains in the session transcript and these durable artifacts. "
            "Read the full result with file_path, 1-based offset and a small limit (for example 200). "
            "For bounded pages, very long lines or files beyond Read's size limit, Read the page manifest: " +
            manifest_path.string() + ". Then Read a listed page with offset/limit; each page is at most 65536 bytes.\n" +
            "--- HEAD: bytes 0.." + std::to_string(head_end) + " ---\n" + std::string(content.substr(0, head_end)) +
            "\n--- OMITTED FROM ACTIVE VIEW: " + std::to_string(tail_start - head_end) +
            " bytes; retrieve from artifacts above ---\n--- TAIL: byte " + std::to_string(tail_start) +
            " onward ---\n" + std::string(content.substr(tail_start));
        return std::optional<std::string>(std::move(view));
    } catch (const std::exception& e) {
        return batbox::Err(std::string("tool output archive: ") + e.what());
    }
}

} // namespace batbox::conversation
