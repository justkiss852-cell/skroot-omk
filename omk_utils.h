// omk_utils.h
// 模块内部使用的小工具集：文件读写、目录创建、命令执行、pid 管理、JSON 转义。
// 只依赖 bionic 的 libc / stdio，不依赖 SKRoot SDK，方便单独复用与调试。
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace omk {

// 统一目录形式：保证以 '/' 结尾，方便拼接
inline std::string normalize_dir(const char* dir) {
    std::string s = (dir && *dir) ? dir : "";
    if (s.empty()) return s;
    if (s.back() != '/') s.push_back('/');
    return s;
}

inline bool file_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

inline bool is_dir(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

inline bool ensure_dir(const std::string& path) {
    if (path.empty()) return false;
    if (is_dir(path)) return true;
    std::string cur;
    cur.reserve(path.size());
    size_t i = 0;
    if (path[0] == '/') { cur = "/"; i = 1; }
    while (i <= path.size()) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && cur != "/" && !file_exists(cur)) {
                if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
            }
            if (i < path.size()) cur.push_back('/');
        } else {
            cur.push_back(path[i]);
        }
        ++i;
    }
    return is_dir(path);
}

inline bool read_text_file(const std::string& path, std::string& out, size_t max_size = 1u << 20) {
    out.clear();
    FILE* fp = ::fopen(path.c_str(), "rb");
    if (!fp) return false;
    char buf[8192];
    size_t n = 0;
    while ((n = ::fread(buf, 1, sizeof(buf), fp)) > 0) {
        out.append(buf, n);
        if (out.size() >= max_size) break;
    }
    ::fclose(fp);
    return true;
}

inline std::string read_text_file(const std::string& path) {
    std::string text;
    read_text_file(path, text);
    return text;
}

// 只读取文件末尾 max_bytes 字节（看日志用）
inline std::string tail_text_file(const std::string& path, size_t max_bytes = 16 * 1024) {
    FILE* fp = ::fopen(path.c_str(), "rb");
    if (!fp) return std::string();
    ::fseek(fp, 0, SEEK_END);
    long size = ::ftell(fp);
    if (size <= 0) { ::fclose(fp); return std::string(); }
    long start = (static_cast<size_t>(size) > max_bytes) ? (size - static_cast<long>(max_bytes)) : 0;
    ::fseek(fp, start, SEEK_SET);
    std::string out;
    out.resize(static_cast<size_t>(size - start));
    size_t got = ::fread(&out[0], 1, out.size(), fp);
    out.resize(got);
    ::fclose(fp);
    return out;
}

inline bool write_text_file(const std::string& path, const std::string& data) {
    FILE* fp = ::fopen(path.c_str(), "wb");
    if (!fp) return false;
    bool ok = (data.empty()) || (::fwrite(data.data(), 1, data.size(), fp) == data.size());
    ::fflush(fp);
    ::fclose(fp);
    return ok;
}

// 执行命令并捕获输出（同步、会阻塞，只能在后台进程里用）
inline std::string run_cmd_capture(const std::string& cmd) {
    std::string out;
    FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return out;
    char buf[4096];
    size_t n = 0;
    while ((n = ::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    ::pclose(fp);
    return out;
}

inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

inline int read_pid_file(const std::string& path) {
    std::string text = trim(read_text_file(path));
    if (text.empty()) return -1;
    char* end = nullptr;
    long pid = ::strtol(text.c_str(), &end, 10);
    if (pid <= 1) return -1;
    return static_cast<int>(pid);
}

inline bool pid_alive(int pid) {
    if (pid <= 1) return false;
    return ::kill(pid, 0) == 0;
}

// 判断 /proc/<pid>/cmdline 里是否包含 keyword，避免误杀复用了 pid 的其它进程
inline bool pid_matches(int pid, const std::string& keyword) {
    if (!pid_alive(pid)) return false;
    std::string cmdline;
    char path[64];
    ::snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    if (!read_text_file(path, cmdline, 4096)) return false;
    for (size_t i = 0; i < cmdline.size(); ++i) {
        if (cmdline[i] == '\0') cmdline[i] = ' ';
    }
    return cmdline.find(keyword) != std::string::npos;
}

inline bool kill_pid(int pid, int sig = SIGTERM) {
    if (pid <= 1) return false;
    return ::kill(pid, sig) == 0;
}

// 简单 JSON 字符串转义（处理路径 / 日志这类文本足够）
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    ::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

inline std::string json_string(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

} // namespace omk
