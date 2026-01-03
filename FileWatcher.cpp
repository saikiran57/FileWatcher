#include "FileWatcher.hpp"
#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <thread>
#include <regex>
#include <fstream>
#ifdef __linux__
#include <sys/inotify.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

std::string toString(const std::filesystem::file_time_type &ftime) {
  return std::format("{:%c}", ftime);
}

FileWatcher::FileWatcher(std::string rootPath,
                         std::chrono::duration<int, std::milli> delay)
    : m_rootPath(std::move(rootPath)), m_delay(delay) {
  m_entry.assign(m_rootPath);
  // do not populate m_fileMap here; initial scan and CREATED events
  // will be performed in start() so callers can observe existing files.
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
  // store callback
  m_callback = cb;
  // create bounded thread pool for callback execution
  size_t workers = std::max<size_t>(1, std::thread::hardware_concurrency());
  size_t qcap = 1024;
  m_threadPool = std::make_unique<BoundedThreadPool>(workers, qcap);
#ifdef __linux__
  // create shutdown pipe for graceful wakeup
  if (pipe(m_shutdownPipe) == 0) {
    fcntl(m_shutdownPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(m_shutdownPipe[1], F_SETFL, O_NONBLOCK);
  }
#endif
  // load ignore patterns (optional .filewatcherignore in root)
  loadIgnorePatterns();
  // perform initial scan and populate internal map for existing files
  // NOTE: do NOT emit events for files that already exist at startup.
  try {
    for (auto &entry : std::filesystem::recursive_directory_iterator(m_entry.path())) {
      const auto &filePath = entry.path().string();
      if (!entry.is_directory()) {
        if (isIgnored(filePath)) continue;
        std::lock_guard<std::mutex> lock(m_fileMapMutex);
        m_fileMap[filePath] = entry.last_write_time();
      } else {
        // ensure directories are watched when inotify starts
        std::lock_guard<std::mutex> lock(m_fileMapMutex);
        m_fileMap[entry.path().string()] = std::filesystem::file_time_type::min();
      }
    }
  } catch (...) {
  }
  // Launch watcher thread for file system monitoring
  m_workerThreads.emplace_back(&FileWatcher::watcherThread, this);
}

void FileWatcher::loadIgnorePatterns() noexcept {
  m_ignoreRegexes.clear();
  try {
    std::filesystem::path ignPath = std::filesystem::path(m_rootPath) / ".filewatcherignore";
    if (!std::filesystem::exists(ignPath)) return;
    std::ifstream in(ignPath);
    std::string line;
    while (std::getline(in, line)) {
      // trim whitespace
      auto start = line.find_first_not_of(" \t\r\n");
      if (start == std::string::npos) continue;
      auto end = line.find_last_not_of(" \t\r\n");
      std::string pat = line.substr(start, end - start + 1);
      if (pat.empty() || pat[0] == '#') continue;
      // convert glob to regex: escape, replace * with .* and ? with .
      std::string re;
      re.reserve(pat.size() * 2);
      for (size_t i = 0; i < pat.size(); ++i) {
        char c = pat[i];
        if (c == '*') re += ".*";
        else if (c == '?') re += '.';
        else if (c == '.') re += "\\.";
        else if (c == '/') re += "/";
        else if (c == '\\') re += "\\\\";
        else if (c == '+' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '^' || c == '$' || c == '|') {
          re += '\\'; re += c;
        } else re += c;
      }
      try {
        m_ignoreRegexes.emplace_back(re, std::regex::ECMAScript | std::regex::icase);
      } catch (...) {}
    }
  } catch (...) {
  }
}

bool FileWatcher::isIgnored(const std::string &path) const noexcept {
  if (m_ignoreRegexes.empty()) return false;
  // match against path relative to root
  std::string rel;
  try {
    auto rp = std::filesystem::relative(path, m_rootPath);
    rel = rp.generic_string();
  } catch (...) {
    rel = path;
  }
  for (const auto &re : m_ignoreRegexes) {
    try {
      if (std::regex_search(rel, re)) return true;
    } catch (...) {}
  }
  return false;
}

void FileWatcher::stop() noexcept {
  if (!m_watching) {
    return; // Not running
  }
  std::cerr << "[stop] Setting m_stoped=true\n";
  m_stoped = true;
  // signal shutdown pipe to wake the watcher thread
#ifdef __linux__
  if (m_shutdownPipe[1] >= 0) {
    std::cerr << "[stop] Writing to shutdown pipe[1]=" << m_shutdownPipe[1] << "\n";
    char byte = 1;
    ssize_t n = write(m_shutdownPipe[1], &byte, 1);
    std::cerr << "[stop] Write result: " << n << "\n";
  }
#endif
  // shutdown thread pool and drain queued callbacks
  if (m_threadPool) {
    std::cerr << "[stop] Shutting down thread pool\n";
    m_threadPool->shutdown(true);
    m_threadPool.reset();
  }
  // Close inotify fd (if used) to unblock watcher thread
#ifdef __linux__
  if (m_inotifyFd >= 0) {
    std::cerr << "[stop] Closing inotify fd\n";
    close(m_inotifyFd);
    m_inotifyFd = -1;
  }
#endif
  std::cerr << "[stop] Joining threads...\n";
  for (auto &thread : m_workerThreads) {
    if (thread.joinable()) {
      std::cerr << "[stop] Joining thread\n";
      thread.join();
      std::cerr << "[stop] Thread joined\n";
    }
  }
  m_workerThreads.clear();
  m_watching = false;
  std::cerr << "[stop] Completed\n";
}

void FileWatcher::watcherThread() noexcept {
#ifdef __linux__
  // initialize inotify
  m_inotifyFd = inotify_init1(IN_CLOEXEC);
  if (m_inotifyFd >= 0) {
    // set non-blocking mode
    int flags = fcntl(m_inotifyFd, F_GETFL);
    fcntl(m_inotifyFd, F_SETFL, flags | O_NONBLOCK);

    // add watches for existing directories and populate file map
    try {
      for (auto &entry : std::filesystem::recursive_directory_iterator(m_entry.path())) {
        if (entry.is_directory()) {
          int wd = inotify_add_watch(m_inotifyFd, entry.path().c_str(),
                                     IN_CREATE | IN_MODIFY | IN_DELETE |
                                         IN_MOVED_FROM | IN_MOVED_TO |
                                         IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_CLOSE_WRITE);
          if (wd >= 0) {
            std::lock_guard<std::mutex> lock(m_fileMapMutex);
            m_wdToPath[wd] = entry.path().string();
            m_pathToWd[entry.path().string()] = wd;
          }
        } else {
          std::lock_guard<std::mutex> lock(m_fileMapMutex);
          m_fileMap[entry.path().string()] = entry.last_write_time();
        }
      }
    } catch (...) {
    }

    // ensure root dir is watched
    try {
      std::string root = m_entry.path().string();
      if (m_pathToWd.find(root) == m_pathToWd.end()) {
        int wd = inotify_add_watch(m_inotifyFd, root.c_str(),
                                   IN_CREATE | IN_MODIFY | IN_DELETE |
                                       IN_MOVED_FROM | IN_MOVED_TO |
                                       IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_CLOSE_WRITE);
        if (wd >= 0) {
          std::lock_guard<std::mutex> lock(m_fileMapMutex);
          m_wdToPath[wd] = root;
          m_pathToWd[root] = wd;
        }
      }
    } catch(...) {}

    // read events using select() to allow interrupt via shutdown pipe
    constexpr size_t BUF_LEN = 16 * (sizeof(struct inotify_event) + 256);
    std::vector<char> buffer(BUF_LEN);

    while (!m_stoped) {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(m_inotifyFd, &readfds);
      if (m_shutdownPipe[0] >= 0) {
        FD_SET(m_shutdownPipe[0], &readfds);
      }
      int maxfd = std::max(m_inotifyFd, m_shutdownPipe[0]);

      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 100000; // 100ms timeout for periodic wakeup

      int ret = select(maxfd + 1, &readfds, nullptr, nullptr, &tv);
      if (ret < 0 && errno != EINTR) break;

      // if shutdown pipe readable, exit
      if (m_shutdownPipe[0] >= 0 && FD_ISSET(m_shutdownPipe[0], &readfds)) {
        break;
      }

      // process inotify events if ready
      if (FD_ISSET(m_inotifyFd, &readfds)) {
        ssize_t length = read(m_inotifyFd, buffer.data(), buffer.size());
        if (length > 0) {
          ssize_t i = 0;
          while (i < length) {
            struct inotify_event *event = reinterpret_cast<struct inotify_event *>(&buffer[i]);
            std::string dir;
            {
              std::lock_guard<std::mutex> lock(m_fileMapMutex);
              auto it = m_wdToPath.find(event->wd);
              if (it != m_wdToPath.end()) dir = it->second;
            }

            std::string name = (event->len > 0) ? std::string(event->name) : std::string();
            std::string fullPath = dir;
            if (!name.empty()) {
              if (!fullPath.empty() && fullPath.back() != '/') fullPath += '/';
              fullPath += name;
            }
            // skip ignored paths
            if (isIgnored(fullPath)) {
              i += sizeof(struct inotify_event) + event->len;
              continue;
            }

            if (event->mask & IN_CREATE) {
              // add to map if file exists
              try {
                if (std::filesystem::exists(fullPath)) {
                  std::lock_guard<std::mutex> lock(m_fileMapMutex);
                  m_fileMap[fullPath] = std::filesystem::last_write_time(fullPath);
                }
              } catch(...) {}
              // record recent directory-level creation to group subsequent child events
              if (event->mask & IN_ISDIR) {
                try {
                  std::lock_guard<std::mutex> lock(m_fileMapMutex);
                  m_recentDirEvents[fullPath] = std::chrono::steady_clock::now();
                } catch(...) {}
              }
              queueFileChangeEvent(fullPath, FileStatus::CREATED);
              if (event->mask & IN_ISDIR) {
                // new directory: add watch
                int wd = inotify_add_watch(m_inotifyFd, fullPath.c_str(),
                                           IN_CREATE | IN_MODIFY | IN_DELETE |
                                               IN_MOVED_FROM | IN_MOVED_TO |
                                               IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
                if (wd >= 0) {
                  std::lock_guard<std::mutex> lock(m_fileMapMutex);
                  m_wdToPath[wd] = fullPath;
                  m_pathToWd[fullPath] = wd;
                }
              }
            } else if (event->mask & IN_MODIFY || event->mask & IN_ATTRIB || event->mask & IN_CLOSE_WRITE) {
              // update timestamp in map
              try {
                if (std::filesystem::exists(fullPath)) {
                  std::lock_guard<std::mutex> lock(m_fileMapMutex);
                  m_fileMap[fullPath] = std::filesystem::last_write_time(fullPath);
                }
              } catch(...) {}
              queueFileChangeEvent(fullPath, FileStatus::MODIFIED);
            } else if (event->mask & IN_DELETE || event->mask & IN_MOVED_FROM) {
              // remove from map
              {
                std::lock_guard<std::mutex> lock(m_fileMapMutex);
                m_fileMap.erase(fullPath);
              }
              // if a directory was removed/moved, record it so child events can be suppressed
              if (event->mask & IN_ISDIR) {
                try {
                  std::lock_guard<std::mutex> lock(m_fileMapMutex);
                  m_recentDirEvents[fullPath] = std::chrono::steady_clock::now();
                } catch(...) {}
              }
              queueFileChangeEvent(fullPath, FileStatus::ERASED);
            } else if (event->mask & IN_MOVED_TO) {
              try {
                if (std::filesystem::exists(fullPath)) {
                  std::lock_guard<std::mutex> lock(m_fileMapMutex);
                  m_fileMap[fullPath] = std::filesystem::last_write_time(fullPath);
                }
              } catch(...) {}
              queueFileChangeEvent(fullPath, FileStatus::CREATED);
            }

            if (event->mask & IN_DELETE_SELF || event->mask & IN_MOVE_SELF) {
              // directory itself removed; remove mapping
              std::lock_guard<std::mutex> lock(m_fileMapMutex);
              auto it = m_pathToWd.find(dir);
              if (it != m_pathToWd.end()) {
                inotify_rm_watch(m_inotifyFd, it->second);
                m_wdToPath.erase(it->second);
                m_pathToWd.erase(it);
              }
            }

            i += sizeof(struct inotify_event) + event->len;
          }
        }
      }
    }

    if (m_inotifyFd >= 0) {
      close(m_inotifyFd);
      m_inotifyFd = -1;
    }
    if (m_shutdownPipe[0] >= 0) {
      close(m_shutdownPipe[0]);
      m_shutdownPipe[0] = -1;
    }
    if (m_shutdownPipe[1] >= 0) {
      close(m_shutdownPipe[1]);
      m_shutdownPipe[1] = -1;
    }

    return;
  }
#endif
  while (!m_stoped) {
    if (!m_stoped) {
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
    // sleep in small increments to quickly detect shutdown signal
    for (int i = 0; i < 10 && !m_stoped; ++i) {
      std::this_thread::sleep_for(m_delay / 10);
    }
  }
}

void FileWatcher::queueFileChangeEvent(const std::string &path, FileStatus status) noexcept {
  if (!m_threadPool || !m_callback) return;
  auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(m_fileMapMutex);
    // prune stale recent-directory entries
    for (auto it = m_recentDirEvents.begin(); it != m_recentDirEvents.end();) {
      if (now - it->second > m_dirGroupWindow) it = m_recentDirEvents.erase(it);
      else ++it;
    }
    // If this path is under a recently-created/removed directory, suppress
    // individual child events — the directory-level event will represent the change.
    std::filesystem::path p(path);
    for (auto parent = p.parent_path(); !parent.empty(); parent = parent.parent_path()) {
      auto pit = m_recentDirEvents.find(parent.string());
      if (pit != m_recentDirEvents.end()) {
        if (now - pit->second <= m_dirGroupWindow) {
          return; // drop child event in favor of grouped dir event
        }
      }
      if (parent.string() == m_rootPath) break;
    }
    auto it = m_lastEvent.find(path);
    if (it != m_lastEvent.end()) {
      // Suppress repeated identical events within debounce window
      if (it->second.first == status && (now - it->second.second) < m_eventDebounce) {
        return; // drop duplicate
      }
      // If a CREATED was just emitted, suppress a MODIFIED within debounce window
      if (status == FileStatus::MODIFIED && it->second.first == FileStatus::CREATED) {
        if (now - it->second.second < m_eventDebounce) {
          return; // drop noisy modified
        }
      }
    }
    // record this event
    m_lastEvent[path] = {status, now};
  }
  std::string p = path;
  FileStatus s = status;
  m_threadPool->submit([this, p = std::move(p), s]() {
    try {
      m_callback(p, s);
    } catch (...) {}
  });
}
