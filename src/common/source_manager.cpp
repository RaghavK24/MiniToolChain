// SPDX-License-Identifier: MIT
#include "minitool/common/source_manager.hpp"

#include <utility>

namespace minitool {

FileId SourceManager::addFile(std::string name, std::string text) {
    Entry entry;
    entry.name = std::move(name);
    entry.text = std::move(text);
    entry.line_starts.push_back(0);
    for (std::size_t i = 0; i < entry.text.size(); ++i) {
        if (entry.text[i] == '\n') {
            entry.line_starts.push_back(i + 1);
        }
    }
    files_.push_back(std::move(entry));
    return static_cast<FileId>(files_.size() - 1);
}

std::string_view SourceManager::name(FileId id) const {
    if (!contains(id)) {
        return {};
    }
    return files_[id].name;
}

std::string_view SourceManager::text(FileId id) const {
    if (!contains(id)) {
        return {};
    }
    return files_[id].text;
}

std::string_view SourceManager::line(FileId id, u32 line) const {
    if (!contains(id) || line == 0) {
        return {};
    }
    const Entry& entry = files_[id];
    const std::size_t index = line - 1U;
    if (index >= entry.line_starts.size()) {
        return {};
    }
    const std::size_t begin = entry.line_starts[index];
    std::size_t end =
        (index + 1U < entry.line_starts.size()) ? entry.line_starts[index + 1U] : entry.text.size();
    while (end > begin && (entry.text[end - 1U] == '\n' || entry.text[end - 1U] == '\r')) {
        --end;
    }
    return std::string_view{entry.text}.substr(begin, end - begin);
}

}  // namespace minitool
