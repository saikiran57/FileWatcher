#include "FileWatcher.hpp"
#include <csignal>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

static volatile std::sig_atomic_t g_stopRequested = 0;

static void handle_signal(int) {
  g_stopRequested = 1;
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    std::cout << "Usage: " << argv[0] << " <directory>\n";
    return 1;
  }

  std::string rootPath = argv[1];
  if (rootPath.empty()) {
    std::cout << "empty directory\n";
    return 1;
  }

  // Install signal handlers for graceful shutdown
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::cout << "File Watcher watching: " << rootPath << " (Ctrl-C to stop)\n";
  FileWatcher fw(rootPath);
  if (!fw.isRootPathExists()) {
    std::cout << "Path does not exist: " << rootPath << "\n";
    return 1;
  }

  fw.start([](const std::string &path, FileStatus status) {
    std::cout << "Path: " << path << " status: " << toString(status) << "\n";
  });

  // Wait for signal
  while (!g_stopRequested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  std::cout << "Shutdown requested, stopping watcher...\n";
  fw.stop();
  std::cout << "Stopped." << std::endl;
  return 0;
}