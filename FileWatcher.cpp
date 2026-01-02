#include "FileWatcher.hpp"
#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <thread>

std::string toString(const std::filesystem::file_time_type &ftime) {
  return std::format("{:%c}", ftime);
}

FileWatcher::FileWatcher(std::string rootPath,
                         std::chrono::duration<int, std::milli> delay)
    : m_rootPath(std::move(rootPath)), m_delay(delay) {
  m_entry.assign(m_rootPath);
  for (auto entry :
       std::filesystem::recursive_directory_iterator(m_entry.path())) {
    m_fileMap[entry.path().string()] = entry.last_write_time();
  }
}

void FileWatcher::setRootPath(std::string &rootPath) noexcept {
  m_rootPath = rootPath;
  m_entry.assign(m_rootPath);
}

bool FileWatcher::isRootPathExists() const noexcept { return m_entry.exists(); }

void FileWatcher::start(const FuncCallback &cb) noexcept {

  while (!m_stoped) {

    std::erase_if(m_fileMap, [&cb](const auto &item) {
      const auto &[path, value] = item;
      if (!std::filesystem::exists(path)) {
        cb(path, FileStatus::ERASED);
        return true;
      }
      return false;
    });

    for (auto &entry :
         std::filesystem::recursive_directory_iterator(m_entry.path())) {
      const auto &filePath = entry.path().string();
      if (!m_fileMap.contains(filePath)) {
        cb(filePath, FileStatus::CREATED);
        m_fileMap[filePath] = entry.last_write_time();
      } else {
        if (m_fileMap[filePath] < entry.last_write_time()) {
          cb(filePath, FileStatus::MODIFIED);
          m_fileMap[filePath] = entry.last_write_time();
        }
      }
    }
    std::this_thread::sleep_for(m_delay);
  }
}
