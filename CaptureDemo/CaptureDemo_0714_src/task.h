#ifndef TASK_H
#define TASK_H
#include <functional>
#include <memory>
#include <list>
#include <thread>
#include <mutex>
#include <QSemaphore>

class Task
{
public:
    Task(const std::function<void()> &f) {
        f_ = std::make_shared<std::function<void()>>(f);
    }

    void run() {
        if(f_) {
            (*f_)();
        }
    }
public:
    std::shared_ptr<std::function<void()>> f_;
};


class Worker {
public:
    Worker() {}
    void start() {
        run_ = true;
        if(!work_thread_) {
            work_thread_ = std::make_shared<std::thread>([=]() {
                while(1) {
                    tasks_sem_.acquire(1);
                    if(!run_) {
                        break;
                    }
                    tasks_mutex_.lock();
                    if(tasks_.size() > 0) {
                        Task t = tasks_.back();
                        tasks_.pop_back();
                        tasks_mutex_.unlock();
                        t.run();
                    } else {
                        tasks_mutex_.unlock();
                    }
                }
            });
        }
    }

    void stop() {
        run_ = false;
        tasks_sem_.release(1);
        if(work_thread_ && work_thread_->joinable()) {
            work_thread_->join();
            work_thread_.reset();
        }
    }
    virtual ~Worker(){}
public:
    void pushTask(const Task &task) {
        tasks_mutex_.lock();
        tasks_.push_back(task);
        tasks_mutex_.unlock();
        tasks_sem_.release(1);
    }
private:
    std::shared_ptr<std::thread> work_thread_ = nullptr;
    QSemaphore tasks_sem_;
    std::mutex tasks_mutex_;
    std::list<Task> tasks_;
    bool run_ = false;
};


#endif // TASK_H
