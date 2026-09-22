#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace gvfg::internal
{
    class ChannelErrorState final
    {
    public:
        struct VendorDetail
        {
            int32_t code = 0;
            std::string api;
            std::string name;
        };

        void set(std::string message)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            message_ = std::move(message);
            vendor_detail_ = {};
        }

        void set_vendor(std::string message, int32_t code, std::string api, std::string name)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            message_ = std::move(message);
            vendor_detail_.code = code;
            vendor_detail_.api = std::move(api);
            vendor_detail_.name = std::move(name);
        }

        void clear()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            message_.clear();
            vendor_detail_ = {};
        }

        std::string message() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return message_;
        }

        VendorDetail vendor_detail() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return vendor_detail_;
        }

    private:
        mutable std::mutex mutex_;
        std::string message_;
        VendorDetail vendor_detail_;
    };
}
