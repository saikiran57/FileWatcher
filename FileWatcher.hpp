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
  void callbackWorkerThread(const FuncCallback &cb) noexcept;
  void queueFileChangeEvent(const std::string &path, FileStatus status) noexcept;
  std::unordered_map<std::string, std::filesystem::file_time_type> m_fileMap;
  std::queue<FileChangeEvent> m_callbackQueue;
  std::string m_rootPath;
  std::filesystem::directory_entry m_entry;
  std::atomic_bool m_stoped = false;
  std::atomic_bool m_watching = false;
  std::mutex m_fileMapMutex;
  std::mutex m_callbackQueueMutex;
  std::condition_variable m_callbackCondVar;
  std::chrono::duration<int, std::milli> m_delay;
  std::vector<std::thread> m_workerThreads;
};