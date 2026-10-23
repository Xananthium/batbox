#include <batbox/agents/AgentJournal.hpp>
#include <batbox/core/Uuid.hpp>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

namespace batbox::agents {

AgentJournal::AgentJournal(std::filesystem::path path, Json state)
    : path_(std::move(path)), state_(std::move(state)) {}

Json AgentJournal::read() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void AgentJournal::update(const std::function<void(Json&)>& edit) {
    std::lock_guard lock(mutex_);
    Json next = state_;
    edit(next);
    std::filesystem::create_directories(path_.parent_path());
    const auto tmp = path_.string() + "." + Uuid::v4().to_string() + ".tmp";
    int fd = ::open(tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("Cannot open worker checkpoint: " + std::string(std::strerror(errno)));
    try {
        const auto text = next.dump(2);
        std::size_t written = 0;
        while (written < text.size()) {
            const auto n = ::write(fd, text.data() + written, text.size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("Cannot write worker checkpoint");
            written += static_cast<std::size_t>(n);
        }
        if (::fsync(fd) != 0) throw std::runtime_error("Cannot sync worker checkpoint");
        ::close(fd); fd = -1;
        std::filesystem::rename(tmp, path_);
        const int dir = ::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0) throw std::runtime_error("Cannot open worker checkpoint directory");
        const int synced = ::fsync(dir);
        ::close(dir);
        if (synced != 0) throw std::runtime_error("Cannot sync worker checkpoint directory");
        state_ = std::move(next);
    } catch (...) {
        if (fd >= 0) ::close(fd);
        ::unlink(tmp.c_str());
        throw;
    }
}

} // namespace batbox::agents
