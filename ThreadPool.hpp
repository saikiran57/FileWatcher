#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class BoundedThreadPool {
public:
  BoundedThreadPool(size_t workerCount, size_t queueCapacity);
  ~BoundedThreadPool();

  // submit a task; blocks if queue full. Returns false if pool is stopping.
  bool submit(std::function<void()> task);

  // stop accepting new tasks; if drain==true, process queued tasks first
  void shutdown(bool drain = true) noexcept;

private:
  void workerLoop();

  std::vector<std::thread> m_workers;
  std::queue<std::function<void()>> m_queue;
  size_t m_queueCapacity;
  std::mutex m_mu;
  std::condition_variable m_cvNotEmpty;
  std::condition_variable m_cvNotFull;
  bool m_stopping = false;
};
