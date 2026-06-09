#pragma once

#include <cstdarg>

namespace gvfg::internal
{
    typedef enum
    {
        GVFG_LOG_TRACE = 0,
        GVFG_LOG_DEBUG = 1,
        GVFG_LOG_INFO = 2,
        GVFG_LOG_WARN = 3,
        GVFG_LOG_ERROR = 4
    } gvfg_log_level_t;

    void log_message(gvfg_log_level_t level, const char *message_utf8);
    void log_message_w(gvfg_log_level_t level, const wchar_t *message_wide);
    void log_printf(gvfg_log_level_t level, const char *fmt, ...);
}

inline void gvfg_log_trace(const char *message_utf8) { gvfg::internal::log_message(gvfg::internal::GVFG_LOG_TRACE, message_utf8); }
inline void gvfg_log_debug(const char *message_utf8) { gvfg::internal::log_message(gvfg::internal::GVFG_LOG_DEBUG, message_utf8); }
inline void gvfg_log_info(const char *message_utf8) { gvfg::internal::log_message(gvfg::internal::GVFG_LOG_INFO, message_utf8); }
inline void gvfg_log_warn(const char *message_utf8) { gvfg::internal::log_message(gvfg::internal::GVFG_LOG_WARN, message_utf8); }
inline void gvfg_log_error(const char *message_utf8) { gvfg::internal::log_message(gvfg::internal::GVFG_LOG_ERROR, message_utf8); }
inline void gvfg_log_debug_w(const wchar_t *message_wide) { gvfg::internal::log_message_w(gvfg::internal::GVFG_LOG_DEBUG, message_wide); }
