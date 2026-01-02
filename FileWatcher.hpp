#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>

enum class FileStatus { CREATED, MODIFIED, ERASED };

using FuncCallback = std::function<void(const std::string &, FileStatus)>;

inline std::string toString(FileStatus status)
{
  switch (status)
  {
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
};

class FileWatcher : public IFileWatcher {
public:
  FileWatcher() = default;
  FileWatcher(std::string rootPath,
              std::chrono::duration<int, std::milli> delay =
                  std::chrono::milliseconds(1000));
  void setRootPath(std::string &rootPath) noexcept override;
  bool isRootPathExists() const noexcept override;
  void start(const FuncCallback &cb) noexcept override;

private:
  std::unordered_map<std::string, std::filesystem::file_time_type> m_fileMap;
  std::string m_rootPath;
  std::filesystem::directory_entry m_entry;
  std::atomic_bool m_stoped = false;
  std::chrono::duration<int, std::milli> m_delay;
};