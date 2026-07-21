#ifndef _SEMAPHORE_H_
#define _SEMAPHORE_H_
#include <memory>
class Semaphore
{
  public:
    Semaphore(unsigned long count = 0) : count_(count) {}

    Semaphore(const Semaphore &) = delete;

    Semaphore &operator=(const Semaphore &) = delete;

    void signal()
    {
        {
            std::unique_lock<std::mutex> lock(mtx_);
            ++count_;
        }
        cv_.notify_one();
    }

    void wait()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [=]() {
            return count_ > 0;
        });
        --count_;
    }

  private:
    std::mutex mtx_;
    std::condition_variable cv_;
    unsigned long count_;
};
#endif