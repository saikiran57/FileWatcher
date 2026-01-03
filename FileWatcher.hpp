#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include "ThreadPool.hpp"
#include <regex>
#include <memory>
#ifdef __linux__
#include <sys/inotify.h>
#endif

enum class FileStatus { CREATED, MODIFIED, ERASED };

using FuncCallback = std::function<void(const std::string &, FileStatus)>;

struct FileChangeEvent {
  std::string path;
  FileStatus status;
};

inline std::string toString(FileStatus status) {
  switch (status) {
  case FileStatus::CREATED:
    return "Created";
    break;
  case FileStatus::MODIFIED:
    return "Modified";
    break;
  case FileStatus::ERASED:
    return "Erased";
    break;
  default:
    return "";
  }
}

class IFileWatcher {
public:
  virtual void setRootPath(std::string &rootPath) noexcept = 0;
  virtual bool isRootPathExists() const noexcept = 0;
  virtual void start(const FuncCallback &cb) noexcept = 0;
  virtual ~IFileWatcher() = 0;
};

class FileWatcher : public IFileWatcher {
public:
  FileWatcher() = default;
  FileWatcher(std::string rootPath,
              std::chrono::duration<int, std::milli> delay =
                  std::chrono::milliseconds(1000));
  ~FileWatcher() override;
  void setRootPath(std::string &rootPath) noexcept override;
  bool isRootPathExists() const noexcept override;
  void start(const FuncCallback &cb) noexcept override;
  void stop() noexcept;

private:
  void watcherThread() noexcept;
  void queueFileChangeEvent(const std::string &path, FileStatus status) noexcept;
  std::unordered_map<std::string, std::filesystem::file_time_type> m_fileMap;
    std::vector<std::regex> m_ignoreRegexes;

    void loadIgnorePatterns() noexcept;
    bool isIgnored(const std::string &path) const noexcept;
  std::string m_rootPath;
  std::filesystem::directory_entry m_entry;
  std::atomic_bool m_stoped = false;
  std::atomic_bool m_watching = false;
  std::mutex m_fileMapMutex;
  FuncCallback m_callback;
  std::unique_ptr<BoundedThreadPool> m_threadPool;
  std::unordered_map<std::string, std::pair<FileStatus, std::chrono::steady_clock::time_point>> m_lastEvent;
  std::chrono::milliseconds m_eventDebounce{500};
  // When many events happen under a directory (e.g. copy/delete tree),
  // record recent directory-level events and suppress individual child
  // events to emit a single grouped event for the directory.
  std::chrono::milliseconds m_dirGroupWindow{500};
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_recentDirEvents;
  std::chrono::duration<int, std::milli> m_delay;
  std::vector<std::thread> m_workerThreads;
#ifdef __linux__
  int m_inotifyFd = -1;
  int m_shutdownPipe[2] = {-1, -1}; // pipe for graceful shutdown signaling
  std::unordered_map<int, std::string> m_wdToPath;
  std::unordered_map<std::string, int> m_pathToWd;
#endif
};