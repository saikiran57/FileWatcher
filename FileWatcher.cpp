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

IFileWatcher::~IFileWatcher() {}

FileWatcher::~FileWatcher() {
  stop();
}

void FileWatcher::setRootPath(std::string &rootPath) noexcept {
  m_rootPath = rootPath;
  m_entry.assign(m_rootPath);
}

bool FileWatcher::isRootPathExists() const noexcept { return m_entry.exists(); }

void FileWatcher::start(const FuncCallback &cb) noexcept {
  if (m_watching.exchange(true)) {
    return; // Already watching
  }
  m_stoped = false;
  // Launch watcher thread for file system monitoring
  m_workerThreads.emplace_back(&FileWatcher::watcherThread, this);
  // Launch callback worker thread for async callback processing
  m_workerThreads.emplace_back(&FileWatcher::callbackWorkerThread, this, cb);
}

void FileWatcher::stop() noexcept {
  if (!m_watching) {
    return; // Not running
  }
  m_stoped = true;
  // Wake callback worker so it can drain any pending events and exit
  m_callbackCondVar.notify_all();
  for (auto &thread : m_workerThreads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  m_workerThreads.clear();
  m_watching = false;
}

void FileWatcher::watcherThread() noexcept {
  while (!m_stoped) {
    {
      std::lock_guard<std::mutex> lock(m_fileMapMutex);
      std::erase_if(m_fileMap, [this](const auto &item) {
        const auto &[path, value] = item;
        if (!std::filesystem::exists(path)) {
          queueFileChangeEvent(path, FileStatus::ERASED);
          return true;
        }
        return false;
      });

      for (auto &entry :
           std::filesystem::recursive_directory_iterator(m_entry.path())) {
        const auto &filePath = entry.path().string();
        if (!m_fileMap.contains(filePath)) {
          queueFileChangeEvent(filePath, FileStatus::CREATED);
          m_fileMap[filePath] = entry.last_write_time();
        } else {
          if (m_fileMap[filePath] < entry.last_write_time()) {
            queueFileChangeEvent(filePath, FileStatus::MODIFIED);
            m_fileMap[filePath] = entry.last_write_time();
          }
        }
      }
    }
    std::this_thread::sleep_for(m_delay);
  }
}

void FileWatcher::queueFileChangeEvent(const std::string &path, FileStatus status) noexcept {
  {
    std::lock_guard<std::mutex> lock(m_callbackQueueMutex);
    m_callbackQueue.emplace(path, status);
  }
  m_callbackCondVar.notify_one();
}

void FileWatcher::callbackWorkerThread(const FuncCallback &cb) noexcept {
  for (;;) {
    std::unique_lock<std::mutex> lock(m_callbackQueueMutex);

    // Wait for events or shutdown signal
    m_callbackCondVar.wait(lock, [this]() {
      return !m_callbackQueue.empty() || m_stoped;
    });

    // If we're stopping and there are no pending events, exit
    if (m_stoped && m_callbackQueue.empty()) {
      break;
    }

    // Process all queued events
    while (!m_callbackQueue.empty()) {
      FileChangeEvent event = std::move(m_callbackQueue.front());
      m_callbackQueue.pop();
      lock.unlock();

      // Execute callback outside of lock to avoid blocking queue processing
      cb(event.path, event.status);

      lock.lock();
    }
  }
}
