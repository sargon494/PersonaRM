#pragma once

#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>

namespace personarm {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error
};

class Logger {
public:
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void SetLogLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_min_level = level;
    }

    bool SetLogFile(const std::string& filepath) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file_stream.is_open()) {
            m_file_stream.close();
        }
        m_file_stream.open(filepath, std::ios::out | std::ios::app);
        return m_file_stream.is_open();
    }

    void Log(LogLevel level, const std::string& tag, const std::string& message) {
        if (level < m_min_level) return;

        std::lock_guard<std::mutex> lock(m_mutex);

        // Timestamp
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3) << ms.count()
           << " [" << LevelToString(level) << "] "
           << "[" << tag << "] "
           << message;

        std::string formatted = ss.str();

        // Console output (with color if supported)
        if (level == LogLevel::Error) {
            std::cerr << formatted << "\n";
        } else {
            std::cout << formatted << "\n";
        }

        // File output
        if (m_file_stream.is_open()) {
            m_file_stream << formatted << "\n";
            m_file_stream.flush();
        }
    }

private:
    Logger() : m_min_level(LogLevel::Info) {}
    ~Logger() {
        if (m_file_stream.is_open()) {
            m_file_stream.close();
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static const char* LevelToString(LogLevel level) {
        switch (level) {
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO ";
            case LogLevel::Warn:  return "WARN ";
            case LogLevel::Error: return "ERROR";
            default:              return "UNKNOWN";
        }
    }

    LogLevel m_min_level;
    std::ofstream m_file_stream;
    std::mutex m_mutex;
};

// Logging macros for clean ergonomics
#define PRM_LOG_DEBUG(tag, msg) personarm::Logger::Instance().Log(personarm::LogLevel::Debug, tag, msg)
#define PRM_LOG_INFO(tag, msg)  personarm::Logger::Instance().Log(personarm::LogLevel::Info,  tag, msg)
#define PRM_LOG_WARN(tag, msg)  personarm::Logger::Instance().Log(personarm::LogLevel::Warn,  tag, msg)
#define PRM_LOG_ERROR(tag, msg) personarm::Logger::Instance().Log(personarm::LogLevel::Error, tag, msg)

} // namespace personarm

