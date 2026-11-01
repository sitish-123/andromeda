#include "core/shared_worker.h"

#include "core/deferred_call.h"

#include <pthread.h>

SharedWorker& SharedWorker::instance() {
  static SharedWorker worker;
  return worker;
}

SharedWorker::~SharedWorker() {
  {
    std::scoped_lock lock(m_mutex);
    m_stopping = true;
  }
  m_wake.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void SharedWorker::post(std::function<void()> work) {
  std::unique_lock lock(m_mutex);
  if (m_stopping) {
    return;
  }
  m_queue.push_back(std::move(work));
  if (!m_running) {
    // An idle-exited thread has left loop() and needs no lock, so joining under the lock is safe.
    if (m_thread.joinable()) {
      m_thread.join();
    }
    m_running = true;
    m_thread = std::thread([this] { loop(); });
    return;
  }
  lock.unlock();
  m_wake.notify_one();
}

bool SharedWorker::running() {
  std::scoped_lock lock(m_mutex);
  return m_running;
}

void SharedWorker::runOnMainLoop(std::function<void()> fn) { DeferredCall::callLater(std::move(fn)); }

void SharedWorker::loop() {
  pthread_setname_np(pthread_self(), "dsk-worker");
  std::unique_lock lock(m_mutex);
  while (true) {
    if (!m_wake.wait_for(lock, m_idleExit, [this] { return m_stopping || !m_queue.empty(); })) {
      m_running = false;
      return;
    }
    if (m_stopping) {
      m_running = false;
      return;
    }
    auto work = std::move(m_queue.front());
    m_queue.pop_front();
    lock.unlock();
    work();
    lock.lock();
  }
}
