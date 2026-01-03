#include "ThreadPool.hpp"

#include <chrono>

BoundedThreadPool::BoundedThreadPool(size_t workerCount, size_t queueCapacity)
    : m_queueCapacity(queueCapacity) {
  for (size_t i = 0; i < workerCount; ++i) {
    m_workers.emplace_back(&BoundedThreadPool::workerLoop, this);
  }
}

BoundedThreadPool::~BoundedThreadPool() {
  shutdown(true);
}

bool BoundedThreadPool::submit(std::function<void()> task) {
  std::unique_lock<std::mutex> lk(m_mu);
  // wait until there is space or stopping
  m_cvNotFull.wait(lk, [this]() { return m_stopping || m_queue.size() < m_queueCapacity; });
  if (m_stopping) return false;
  m_queue.push(std::move(task));
  lk.unlock();
  m_cvNotEmpty.notify_one();
  return true;
}

void BoundedThreadPool::shutdown(bool drain) noexcept {
  {
    std::unique_lock<std::mutex> lk(m_mu);
    if (m_stopping) return;
    m_stopping = true;
  }
  // wake workers and any submitters
  m_cvNotEmpty.notify_all();
  m_cvNotFull.notify_all();

  if (drain) {
    // join workers after queue drained
    for (auto &t : m_workers) {
      if (t.joinable()) t.join();
    }
  } else {
    // clear queue then join
    {
      std::unique_lock<std::mutex> lk(m_mu);
      while (!m_queue.empty()) m_queue.pop();
    }
    for (auto &t : m_workers) {
      if (t.joinable()) t.join();
    }
  }
}

void BoundedThreadPool::workerLoop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lk(m_mu);
      m_cvNotEmpty.wait(lk, [this]() { return m_stopping || !m_queue.empty(); });
      if (m_stopping && m_queue.empty()) return;
      task = std::move(m_queue.front());
      m_queue.pop();
      // notify submitters that space is available
      lk.unlock();
      m_cvNotFull.notify_one();
    }
    // execute task outside lock
    try {
      task();
    } catch (...) {
      // swallow exceptions from user tasks to keep worker alive
    }
  }
}
