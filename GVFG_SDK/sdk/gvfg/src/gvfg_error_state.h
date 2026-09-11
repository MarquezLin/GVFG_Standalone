#pragma once

#include <mutex>
#include <string>
#include <utility>

namespace gvfg::internal
{
    class ChannelErrorState final
    {
    public:
        void set(std::string message)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            message_ = std::move(message);
        }

        void clear()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            message_.clear();
        }

        std::string message() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return message_;
        }

    private:
        mutable std::mutex mutex_;
        std::string message_;
    };
}
