#include "FileWatcher.hpp"
#include <iostream>
#include <string>

int main() {
  std::cout << "File Watcher...\n";
  std::string rootPath = "/home/sai_dev/projects/cpp/FileWatcher";
  FileWatcher fw(rootPath);
  if (fw.isRootPathExists()) {
    fw.start([](const std::string &path, FileStatus status) {
      std::cout << "Path: " << path << " status:" << toString(status) << "\n";
    });
  }
}