#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <batbox/conversation/ToolOutputArchive.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/tools/ReadTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace batbox::conversation;

namespace {
struct Temporary {
    fs::path path;
    Temporary() {
        std::string pattern = (fs::temp_directory_path() / "batbox-tool-archive-XXXXXX").string();
        const auto p = ::mkdtemp(pattern.data());
        if (!p) throw std::runtime_error("mkdtemp failed");
        path = p;
    }
    ~Temporary() { std::error_code ec; fs::remove_all(path, ec); }
};
std::string read(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary); file << bytes;
}
batbox::Json manifest(const fs::path& directory) {
    for (const auto& item : fs::directory_iterator(directory))
        if (item.path().filename().string().ends_with(".pages.json"))
            return batbox::Json::parse(read(item.path()));
    throw std::runtime_error("Missing page manifest");
}
std::string listing() {
    if (const auto path = std::getenv("BATBOX_ARCHIVE_REAL_TOOL_FIXTURE")) return read(path);
    std::string text = "DIRECTORY_HEAD\n";
    for (int i = 0; i < 24000; ++i)
        text += "/project/build/vcpkg/installed/package-" + std::to_string(i) + "/include/header.hpp\n";
    return text + "DIRECTORY_TAIL\nExit code: 0\n";
}
}

TEST_CASE("ordinary tool bodies are unchanged and create no artifacts") {
    Temporary tmp;
    auto result = archive_tool_output(std::string(kToolOutputArchiveThreshold - 1, 'a'), tmp.path / "archive");
    REQUIRE(result.has_value());
    CHECK_FALSE(result.value().has_value());
    CHECK_FALSE(fs::exists(tmp.path / "archive"));
}

TEST_CASE("large listing archives exact bytes with private reusable pages and bounded view") {
    Temporary tmp;
    const auto original = listing();
    REQUIRE(original.size() >= kToolOutputArchiveThreshold);
    auto result = archive_tool_output(original, tmp.path / "archive");
    REQUIRE(result.has_value());
    REQUIRE(result.value().has_value());
    CHECK(result.value()->size() < 20 * 1024);
    CHECK(result.value()->find("SHA256:") != std::string::npos);
    CHECK(result.value()->find("offset") != std::string::npos);
    CHECK(result.value()->find("limit") != std::string::npos);
    CHECK(result.value()->find(original.substr(0, 100)) != std::string::npos);
    CHECK(result.value()->find(original.substr(original.size() - 100)) != std::string::npos);
    const auto meta = manifest(tmp.path / "archive");
    CHECK(meta["bytes"].get<std::size_t>() == original.size());
    if (const auto expected = std::getenv("BATBOX_ARCHIVE_EXPECTED_SHA256"))
        CHECK(meta["sha256"].get<std::string>() == expected);
    CHECK(bool(read(meta["file_path"].get<std::string>()) == original));
    std::string joined;
    for (const auto& page : meta["pages"]) {
        const auto bytes = read(page["file_path"].get<std::string>());
        CHECK(page["byte_offset"].get<std::size_t>() == joined.size());
        CHECK(bytes.size() <= kToolOutputArchivePageBytes);
        CHECK(bytes.size() == page["bytes"].get<std::size_t>());
        joined += bytes;
    }
    CHECK(bool(joined == original));
    batbox::tools::ReadTool reader;
    batbox::tools::ToolContext context;
    context.cwd = tmp.path;
    const auto& page = meta["pages"][meta["pages"].size() / 2];
    const auto read_page = reader.run({{"file_path", page["file_path"]}, {"offset", 1}, {"limit", 2}}, context);
    CHECK_FALSE(read_page.is_error);
    const auto page_text = read(page["file_path"].get<std::string>());
    CHECK(read_page.body.find(page_text.substr(0, std::min<std::size_t>(40, page_text.size()))) != std::string::npos);
    for (const auto& item : fs::directory_iterator(tmp.path / "archive")) {
        struct stat st{};
        REQUIRE(::stat(item.path().c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0600);
        CHECK(item.path().filename().string().find(".tmp.") == std::string::npos);
    }
    const auto timestamp = fs::last_write_time(meta["file_path"].get<std::string>());
    const auto again = archive_tool_output(original, tmp.path / "archive");
    REQUIRE(again.has_value());
    CHECK(bool(*again.value() == *result.value()));
    CHECK(fs::last_write_time(meta["file_path"].get<std::string>()) == timestamp);
    if (const auto report = std::getenv("BATBOX_ARCHIVE_TEST_REPORT")) {
        const batbox::Json details = {{"original_bytes", original.size()},
            {"original_sha256", meta["sha256"]}, {"original_lines", meta["lines"]},
            {"active_view_bytes", result.value()->size()}, {"pages", meta["pages"].size()},
            {"page_bytes_max", kToolOutputArchivePageBytes}, {"exact_bytes_reconstructed", joined == original},
            {"read_tool_page_succeeded", !read_page.is_error}, {"files_mode", "0600"}};
        write(report, details.dump(2) + "\n");
    }
}

TEST_CASE("corrupt existing artifact and unwritable destination refuse a replacement view") {
    Temporary tmp;
    const auto original = listing();
    REQUIRE(archive_tool_output(original, tmp.path / "archive").has_value());
    auto meta = manifest(tmp.path / "archive");
    auto corrupt = original;
    corrupt[corrupt.size() / 2] ^= 1;
    write(meta["file_path"].get<std::string>(), corrupt);
    auto result = archive_tool_output(original, tmp.path / "archive");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("mismatch") != std::string::npos);
    CHECK(bool(read(meta["file_path"].get<std::string>()) == corrupt));
    write(tmp.path / "blocked", "not a directory");
    CHECK_FALSE(archive_tool_output(original, tmp.path / "blocked").has_value());
}

TEST_CASE("long UTF8 lines split into valid bounded pages without losing bytes") {
    Temporary tmp;
    std::string original;
    for (int i = 0; i < 300000; ++i) original += "é";
    auto result = archive_tool_output(original, tmp.path / "archive");
    REQUIRE(result.has_value());
    REQUIRE(result.value().has_value());
    CHECK_NOTHROW(batbox::Json(*result.value()).dump());
    std::string joined;
    const auto meta = manifest(tmp.path / "archive");
    for (const auto& page : meta["pages"]) {
        const auto bytes = read(page["file_path"].get<std::string>());
        CHECK(bytes.size() <= kToolOutputArchivePageBytes);
        CHECK_NOTHROW(batbox::Json(bytes).dump());
        joined += bytes;
    }
    CHECK(bool(joined == original));
}
