// module_oh_my_keymint.cpp
// ---------------------------------------------------------------------------
// SKRoot(Pro) 模块：把 Oh My Keymint (OMK) 运行在 SKRoot 环境里。
//
// OMK 本身是一个 KernelSU/Magisk 模块，由 module.prop + post-fs-data.sh +
// service.sh 拉起两个常驻进程：
//   keymint          —— 自定义 KeyMint 实现（keystore2 通过 unix socket 访问）
//   inject           —— 把 payload 注入 keystore2 进程
// SKRoot 模块没有 Magisk 那套脚本生命周期，所以这里只做三件事：
//   1. skroot_module_main 里 fork 一个后台子进程，等系统起来后执行
//      resources/omk/skroot_start.sh（等价于 OMK 的 post-fs-data.sh + service.sh）；
//   2. 提供 WebUI，用于查看状态、开关、修改 injector.toml、看日志、重启；
//   3. 卸载时把守护进程停掉并清理 /data/adb/omk 与 /data/misc/keystore/omk。
// ---------------------------------------------------------------------------

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "kernel_module_kit_umbrella.h"
#include "omk_utils.h"

#define MOD_VER "1.3.0-sk1"
#define OMK_BIN_VER "1.3.0-7742865"

namespace {

using namespace omk;

constexpr const char* kStateDir        = "/data/adb/omk";                       // OMK 状态目录
constexpr const char* kDataDir         = "/data/misc/keystore/omk";             // keystore 侧数据目录
constexpr const char* kDataLogDir      = "/data/misc/keystore/omk/logs";
constexpr const char* kModuleLogDir    = "/data/adb/omk/logs";
constexpr const char* kModuleLog       = "/data/adb/omk/logs/skroot_module.log";
constexpr const char* kStartLog        = "/data/adb/omk/logs/skroot_start.log";
constexpr const char* kKeymintPidFile  = "/data/adb/omk/keymint-daemon.pid";
constexpr const char* kInjectorPidFile = "/data/adb/omk/injector-daemon.pid";

constexpr const char* kStartScript     = "omk/skroot_start.sh";   // 相对 module_private_dir
constexpr const char* kKeymintBin      = "omk/keymint";
constexpr const char* kInjectBin       = "omk/inject";
constexpr const char* kKeyboxRes       = "omk/keybox.xml";
constexpr const char* kInjectorCfgRes  = "omk/injector.toml";
constexpr const char* kKeyboxTarget    = "/data/misc/keystore/omk/keybox.xml";
constexpr const char* kInjectorTarget  = "/data/misc/keystore/omk/injector.toml";

constexpr const char* kToggleEnabled   = "omk_enabled";
constexpr const char* kToggleAutoBoot  = "omk_autoboot";

static std::string g_module_dir;   // 带结尾 '/'

// ---------------------------------------------------------------- 日志
void omk_log(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    ::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    ::printf("[oh_my_keymint] %s\n", msg);
    ::fflush(stdout);

    // 同时写文件：SKRoot 模块在开机早期 stdout 不一定能看到
    ensure_dir(kModuleLogDir);
    FILE* fp = ::fopen(kModuleLog, "a");
    if (fp) {
        char ts[32];
        time_t now = ::time(nullptr);
        struct tm tmv {};
        ::localtime_r(&now, &tmv);
        ::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        ::fprintf(fp, "[%s] [pid %d] %s\n", ts, (int)::getpid(), msg);
        ::fclose(fp);
    }
}

// ---------------------------------------------------------------- 配置开关
bool get_toggle(const char* key, bool default_value) {
    bool value = default_value;
    if (!is_ok(kernel_module::read_bool_disk_storage(key, value))) return default_value;
    return value;
}

bool set_toggle(const char* key, bool value) {
    return is_ok(kernel_module::write_bool_disk_storage(key, value));
}

bool get_enabled()  { return get_toggle(kToggleEnabled, true); }
bool set_enabled(bool v) { return set_toggle(kToggleEnabled, v); }

// ---------------------------------------------------------------- 路径
std::string dir_of(const char* rel) { return g_module_dir + rel; }

// ---------------------------------------------------------------- 进程
void prepare_state_dirs() {
    ensure_dir(kStateDir);
    ensure_dir(kModuleLogDir);
    ensure_dir(kDataDir);
    ensure_dir(kDataLogDir);
}

int keymint_daemon_pid()  { return read_pid_file(kKeymintPidFile); }
int injector_daemon_pid() { return read_pid_file(kInjectorPidFile); }

bool daemons_running() {
    int k = keymint_daemon_pid();
    int i = injector_daemon_pid();
    return (k > 1 && pid_alive(k)) || (i > 1 && pid_alive(i));
}

void kill_and_wait(int pid, const std::string& keyword) {
    if (pid <= 1) return;
    if (!pid_alive(pid)) return;
    if (!pid_matches(pid, keyword)) return;   // pid 被复用了，别乱杀
    kill_pid(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        if (!pid_alive(pid)) return;
        ::usleep(100 * 1000);
    }
    kill_pid(pid, SIGKILL);
    ::usleep(200 * 1000);
}

void stop_daemons() {
    omk_log("stop OMK daemons");
    kill_and_wait(keymint_daemon_pid(),  "daemon");
    kill_and_wait(injector_daemon_pid(), "daemon-injector");
    ::unlink(kKeymintPidFile);
    ::unlink(kInjectorPidFile);

    // 兜底：pid 文件可能在异常退出时残留，按命令行特征再扫一遍
    const std::string ps = run_cmd_capture("ps -A -o PID,ARGS 2>/dev/null");
    const char* patterns[] = { "omk/daemon-injector", "omk/daemon" };
    std::string line;
    for (size_t i = 0; i <= ps.size(); ++i) {
        if (i == ps.size() || ps[i] == 0x0a) {
            for (const char* p : patterns) {
                if (!line.empty() && line.find(p) != std::string::npos) {
                    int pid = ::atoi(line.c_str());
                    if (pid > 1) kill_pid(pid, SIGKILL);
                }
            }
            line.clear();
        } else {
            line.push_back(ps[i]);
        }
    }
}

// 在后台进程里执行启动脚本（阻塞直到脚本返回；脚本内部再把 daemon 放后台）
bool run_start_script_blocking(bool wait_boot) {
    const std::string script = dir_of(kStartScript);
    if (!file_exists(script)) {
        omk_log("start script not found: %s", script.c_str());
        return false;
    }

    // 等待系统启动完成，避免 keystore2 还没起来就注入
    if (wait_boot) {
        for (int i = 0; i < 180; ++i) {
            std::string v = trim(run_cmd_capture("getprop sys.boot_completed 2>/dev/null"));
            if (v == "1") break;
            ::sleep(1);
        }
        ::sleep(5);
    }

    char log_dir[64];
    ::snprintf(log_dir, sizeof(log_dir), "%s", kModuleLogDir);
    ensure_dir(log_dir);
    int fd = ::open(kStartLog, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) ::close(fd);
    }

    omk_log("exec %s", script.c_str());
    ::execl("/system/bin/sh", "sh", script.c_str(), (char*)nullptr);
    return false;   // exec 失败才会走到这里
}

// 子进程：真正执行启动脚本
void child_run_start_script(bool wait_boot) {
    ::setsid();
    if (!run_start_script_blocking(wait_boot)) {
        omk_log("exec start script failed, errno=%d", errno);
        ::_exit(1);
    }
    ::_exit(0);
}

// fork 一个后台子进程执行启动脚本，父进程立刻返回（入口函数不能阻塞）
bool launch_start_script(bool wait_boot) {
    pid_t pid = ::fork();
    if (pid < 0) {
        omk_log("fork failed, errno=%d", errno);
        return false;
    }
    if (pid == 0) child_run_start_script(wait_boot);
    return true;
}

std::string keystore_pid_text() {
    return trim(run_cmd_capture("pidof keystore2 2>/dev/null"));
}

bool keystore_injected(const std::string& pid_text) {
    if (pid_text.empty()) return false;
    int pid = ::atoi(pid_text.c_str());
    if (pid <= 1) return false;
    char path[64];
    ::snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    std::string maps;
    if (!read_text_file(path, maps, 32u << 20)) return false;
    return maps.find("/data/adb/omk/") != std::string::npos ||
           maps.find("/data/misc/keystore/omk/") != std::string::npos;
}

// ---------------------------------------------------------------- 状态 JSON
std::string build_status_json() {
    const int kpid = keymint_daemon_pid();
    const int ipid = injector_daemon_pid();
    const std::string kspid = keystore_pid_text();

    std::string out = "{";
    out += "\"version\":" + json_string(MOD_VER) + ",";
    out += "\"omkVersion\":" + json_string(OMK_BIN_VER) + ",";
    out += "\"enabled\":" + std::string(get_enabled() ? "true" : "false") + ",";
    out += "\"keymintPid\":" + std::to_string(pid_alive(kpid) ? kpid : 0) + ",";
    out += "\"injectorPid\":" + std::to_string(pid_alive(ipid) ? ipid : 0) + ",";
    out += "\"keymintRunning\":" + std::string(pid_alive(kpid) ? "true" : "false") + ",";
    out += "\"injectorRunning\":" + std::string(pid_alive(ipid) ? "true" : "false") + ",";
    out += "\"keystorePid\":" + json_string(kspid) + ",";
    out += "\"injected\":" + std::string(keystore_injected(kspid) ? "true" : "false") + ",";
    out += "\"moduleDir\":" + json_string(g_module_dir);
    out += "}";
    return out;
}

// ---------------------------------------------------------------- 安装 / 卸载回调
std::string module_on_install(const char* root_key, const char* module_private_dir) {
    (void)root_key;
    g_module_dir = normalize_dir(module_private_dir);
    omk_log("on install, module dir: %s", g_module_dir.c_str());

    const std::string sdk = trim(run_cmd_capture("getprop ro.build.version.sdk 2>/dev/null"));
    if (!sdk.empty() && ::atoi(sdk.c_str()) < 33) {
        return std::string("Oh My Keymint 需要 Android 13 (API 33) 及以上，当前 API: ") + sdk;
    }

    prepare_state_dirs();
    // 首次安装就把配置放到 keystore 侧目录，方便安装后立刻生效
    if (!file_exists(kKeyboxTarget) && file_exists(dir_of(kKeyboxRes))) {
        std::string text;
        if (read_text_file(dir_of(kKeyboxRes), text)) write_text_file(kKeyboxTarget, text);
    }
    if (!file_exists(kInjectorTarget) && file_exists(dir_of(kInjectorCfgRes))) {
        std::string text;
        if (read_text_file(dir_of(kInjectorCfgRes), text)) write_text_file(kInjectorTarget, text);
    }
    ::chmod(kKeyboxTarget, 0600);
    ::chmod(kInjectorTarget, 0600);
    return std::string();   // 返回空串 = 允许安装
}

void module_on_uninstall(const char* root_key, const char* module_private_dir) {
    (void)root_key;
    g_module_dir = normalize_dir(module_private_dir);
    omk_log("on uninstall, clean up");
    stop_daemons();
    run_cmd_capture("rm -rf /data/adb/omk /data/misc/keystore/omk 2>/dev/null");
    ::sync();
}

// ---------------------------------------------------------------- WebUI
class OmkWebHandler : public kernel_module::WebUIHttpHandler {
public:
    void onPrepareCreate(const char* root_key, const char* module_private_dir, uint32_t port) override {
        (void)root_key;
        g_module_dir = normalize_dir(module_private_dir);
        omk_log("webui created, port=%u", (unsigned)port);
    }

    bool handlePost(CivetServer* server, struct mg_connection* conn, const std::string& path,
                    const std::string& body) override {
        (void)server;
        std::string resp;

        if (path == "/getVersion") {
            resp = MOD_VER;
        } else if (path == "/getStatus") {
            resp = build_status_json();
        } else if (path == "/setEnabled") {
            bool on = (body == "1");
            set_enabled(on);
            resp = on ? "OK" : "OFF";
            omk_log("webui set enabled=%d", (int)on);
        } else if (path == "/restart") {
            stop_daemons();
            launch_start_script(false);
            resp = "RESTARTING";
        } else if (path == "/stop") {
            stop_daemons();
            resp = "STOPPED";
        } else if (path == "/getLog") {
            resp = tail_text_file(kStartLog, 24 * 1024);
            std::string mod = tail_text_file(kModuleLog, 8 * 1024);
            if (!mod.empty()) resp += "\n===== skroot_module.log =====\n" + mod;
            if (resp.empty()) resp = "(暂无日志)";
        } else if (path == "/getInjectorToml") {
            resp = read_text_file(file_exists(kInjectorTarget) ? kInjectorTarget : dir_of(kInjectorCfgRes));
        } else if (path == "/setInjectorToml") {
            ensure_dir(kDataDir);
            if (body.empty()) {
                resp = "EMPTY";
            } else {
                bool ok = write_text_file(kInjectorTarget, body) ||
                          write_text_file(dir_of(kInjectorCfgRes), body);
                ::chmod(kInjectorTarget.c_str(), 0600);
                resp = ok ? "OK" : "FAILED";
                omk_log("webui set injector.toml: %s", ok ? "ok" : "failed");
            }
        } else if (path == "/getKeyboxXml") {
            resp = read_text_file(file_exists(kKeyboxTarget) ? kKeyboxTarget : dir_of(kKeyboxRes));
        } else if (path == "/setKeyboxXml") {
            ensure_dir(kDataDir);
            if (body.empty()) {
                resp = "EMPTY";
            } else {
                bool ok = write_text_file(kKeyboxTarget, body) ||
                          write_text_file(dir_of(kKeyboxRes), body);
                ::chmod(kKeyboxTarget.c_str(), 0600);
                resp = ok ? "OK" : "FAILED";
                omk_log("webui set keybox.xml: %s", ok ? "ok" : "failed");
            }
        } else if (path == "/getPaths") {
            resp = "state=" + std::string(kStateDir) +
                   "\ndata=" + std::string(kDataDir) +
                   "\nmodule=" + g_module_dir +
                   "\nsdk=" + trim(run_cmd_capture("getprop ro.build.version.sdk 2>/dev/null"));
        } else {
            resp = "UNKNOWN_PATH";
        }

        kernel_module::webui::send_text(conn, 200, resp);
        return true;
    }
};

} // namespace

// ---------------------------------------------------------------- 模块入口
int skroot_module_main(const char* root_key, const char* module_private_dir) {
    (void)root_key;
    g_module_dir = normalize_dir(module_private_dir);

    omk_log("module start, dir=%s", g_module_dir.c_str());
    prepare_state_dirs();

    // 资源自检：keymint / inject 必须随 ZIP 一起打包并在模块目录里
    if (!file_exists(dir_of(kKeymintBin)) || !file_exists(dir_of(kInjectBin))) {
        omk_log("missing OMK binaries: %s / %s", dir_of(kKeymintBin).c_str(), dir_of(kInjectBin).c_str());
        return 2;
    }

    if (!get_enabled()) {
        omk_log("module disabled by config, skip auto start");
        return 0;
    }

    if (daemons_running()) {
        omk_log("OMK daemons already running, skip");
        return 0;
    }

    // 入口函数必须尽快返回：真正的启动流程放到 fork 出去的子进程里
    if (!launch_start_script(true)) {
        omk_log("launch failed");
        return 1;
    }
    return 0;
}

// SKRoot 模块名片
SKROOT_MODULE_NAME("Oh My Keymint")
SKROOT_MODULE_VERSION(MOD_VER)
SKROOT_MODULE_DESC("Custom keystore implementation for Android Keystore Spoofer (KeyMint 伪装)")
SKROOT_MODULE_AUTHOR("qwq233 (OMK) / SKRoot port")
SKROOT_MODULE_ID32("omkskrootmodule0001abcdefghijklm")
SKROOT_MODULE_WEB_UI(OmkWebHandler)
SKROOT_MODULE_ON_INSTALL(module_on_install)
SKROOT_MODULE_ON_UNINSTALL(module_on_uninstall)
