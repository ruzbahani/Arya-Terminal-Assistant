#include "platform.hpp"
#include "embedded.h"

using namespace arya;

namespace {

const char* APP_NAME = "Arya Terminal Assistant";
const char* VERSION = "1.1.0";
const size_t MAX_OUTPUT = 4000000;

std::string g_token, g_home, g_tmp, g_data, g_shell, g_shell_name, g_askpass, g_start_dir, g_host1, g_host2;
bool g_window_mode = false;
std::atomic<bool> g_quit{false};

double now_s() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

std::string expand_home(const std::string& p) {
    if (p == "~") return g_home;
    if (starts_with(p, "~/") || (IS_WIN && starts_with(p, "~\\"))) return g_home + p.substr(1);
    return p;
}
std::string resolve(const std::string& cwd, const std::string& target) {
    std::string p = expand_home(target.empty() ? "~" : target);
    return normalize(is_abs(p) ? p : join(cwd, p));
}
std::string valid_dir(const std::string& p) { return is_dir(p) ? p : g_home; }

const char* DROP_ENV[] = {"PWD", "OLDPWD", "SHLVL", "_", "CLICOLOR", "ARYA_STATE"};

Env clean_env(const Env& in) {
    Env out;
    for (auto& kv : in) {
        const std::string& k = kv.first;
        if (k.empty() || k.find('=') != std::string::npos || k.find('\0') != std::string::npos || kv.second.find('\0') != std::string::npos) continue;
        bool drop = false;
        for (const char* d : DROP_ENV) if (k == d) drop = true;
        if (!drop) out.push_back(kv);
    }
    return out;
}

Env with_extras(Env env) {
    env_set(env, "GIT_PAGER", "cat");
    env_set(env, "GIT_TERMINAL_PROMPT", "0");
    env_set(env, "PYTHONUNBUFFERED", "1");
    env_set(env, "PYTHONIOENCODING", "utf-8");
    env_set(env, "TERM", "xterm-256color");
    env_set(env, "COLUMNS", "120");
    if (!IS_WIN) {
        env_set(env, "PAGER", "cat");
        env_set(env, "MANPAGER", "cat");
        env_set(env, "CLICOLOR_FORCE", "1");
    }
    return env;
}

Env login_env() {
    Env env = current_env();
#ifndef _WIN32
    Proc p;
    std::string err;
    if (spawn({g_shell, "-ilc", "printf __ARYA_ENV__; env -0"}, g_home, env, p, err)) {
        close(p.in);
        std::string out;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        bool exited = false;
        double exited_at = 0;
        for (;;) {
            if (std::chrono::steady_clock::now() > deadline) break;
            if (!exited && waitpid(p.pid, nullptr, WNOHANG) == p.pid) { exited = true; exited_at = now_s(); }
            if (exited && now_s() - exited_at > 0.3) break;
            pollfd pf{p.out, POLLIN, 0};
            if (poll(&pf, 1, 100) <= 0) continue;
            char buf[16384];
            ssize_t n = read(p.out, buf, sizeof buf);
            if (n <= 0) break;
            out.append(buf, (size_t)n);
        }
        close(p.out);
        if (!exited) { killpg(p.pid, SIGKILL); waitpid(p.pid, nullptr, 0); }
        size_t k = out.find("__ARYA_ENV__");
        if (k != std::string::npos) {
            Env found = parse_env0(out.substr(k + 12));
            if (!env_get(found, "PATH").empty()) env = found;
        }
    }
#endif
    return with_extras(clean_env(env));
}

class Session {
public:
    void start() {
        std::thread([this] {
            Env e = login_env();
            std::lock_guard<std::mutex> lock(m_);
            base_ = env_ = e;
            ready_ = true;
            cv_.notify_all();
        }).detach();
    }
    Env get() {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait_for(lock, std::chrono::seconds(15), [&] { return ready_; });
        return ready_ ? env_ : with_extras(clean_env(current_env()));
    }
    void update(const Env& e) {
        Env c = clean_env(e);
        if (env_get(c, "PATH").empty()) return;
        std::lock_guard<std::mutex> lock(m_);
        env_ = c;
    }
    void reset() {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait_for(lock, std::chrono::seconds(15), [&] { return ready_; });
        env_ = base_;
    }
    std::string venv() {
        std::lock_guard<std::mutex> lock(m_);
        std::string v = env_get(env_, "VIRTUAL_ENV");
        return v.empty() ? v : base_name(v);
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool ready_ = false;
    Env base_, env_;
} g_session;

const std::string POSIX_PRE = "trap '{ pwd; printf \"\\0\"; env -0; } > \"$ARYA_STATE\" 2>/dev/null' EXIT\n";
const std::string MAC_SUDO = "sudo() { command sudo -A \"$@\"; }\n";

const std::string PS_PRE =
    "$ProgressPreference = 'SilentlyContinue'; "
    "try { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) } catch {}; "
    "$OutputEncoding = [Text.UTF8Encoding]::new($false); "
    "try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072 } catch {}; "
    "$global:LASTEXITCODE = 0\n";
const std::string PS_POST =
    "\n$__aryaOk = $?\n"
    "try { @{ cwd = (Get-Location).ProviderPath; env = [Environment]::GetEnvironmentVariables() } | "
    "ConvertTo-Json -Compress -Depth 3 | Set-Content -LiteralPath $env:ARYA_STATE -Encoding UTF8 } catch {}\n"
    "if ($LASTEXITCODE) { exit $LASTEXITCODE } elseif (-not $__aryaOk) { exit 1 }\n";

struct Job {
    std::string id, cmd, cwd, state_file;
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> chunks;
    size_t size = 0;
    bool done = false, stopped = false, reader_done = false, stdin_closed = false;
    int code = 0;
    double secs = 0, t0 = 0;
    Proc p;
};
using JobPtr = std::shared_ptr<Job>;

std::mutex g_jobs_m;
std::map<std::string, JobPtr> g_jobs;

void job_emit(Job& j, const std::string& text) {
    std::lock_guard<std::mutex> lock(j.m);
    if (j.size < MAX_OUTPUT) {
        j.chunks.push_back(text);
        j.size += text.size();
        if (j.size >= MAX_OUTPUT) j.chunks.push_back("\n[\xE2\x80\xA6 output too long \xE2\x80\x94 the rest is not shown \xE2\x80\xA6]\n");
    }
    j.cv.notify_all();
}

#ifdef _WIN32
std::string oem_to_utf8(const char* data, size_t n) {
    int w = MultiByteToWideChar(CP_OEMCP, 0, data, (int)n, nullptr, 0);
    std::wstring ws(w, L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, data, (int)n, &ws[0], w);
    return narrow(ws);
}
#endif

void job_reader(JobPtr j) {
    std::string carry;
    bool oem = false;
    (void)oem;
    std::vector<char> buf(65536);
    for (;;) {
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(j->p.out, buf.data(), (DWORD)buf.size(), &n, nullptr) || n == 0) break;
#else
        ssize_t n = read(j->p.out, buf.data(), buf.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
#endif
        std::string text;
#ifdef _WIN32
        if (oem) {
            text = oem_to_utf8(buf.data(), n);
        } else {
            std::string saved = carry;
            if (!utf8_append(carry, buf.data(), n, text)) {
                oem = true;
                carry.clear();
                std::string raw = saved + std::string(buf.data(), n);
                text = oem_to_utf8(raw.data(), raw.size());
            }
        }
#else
        utf8_append(carry, buf.data(), (size_t)n, text);
#endif
        if (!text.empty()) job_emit(*j, text);
    }
    if (!carry.empty()) job_emit(*j, "\xEF\xBF\xBD");
#ifdef _WIN32
    CloseHandle(j->p.out);
#else
    close(j->p.out);
#endif
    std::lock_guard<std::mutex> lock(j->m);
    j->reader_done = true;
    j->cv.notify_all();
}

void load_state(Job& j) {
    std::string raw;
    if (!read_file(j.state_file, raw)) return;
    remove_file(j.state_file);
    std::string cwd;
    Env env;
#ifdef _WIN32
    if (starts_with(raw, "\xEF\xBB\xBF")) raw.erase(0, 3);
    JVal v;
    if (!json_parse(raw, v)) return;
    cwd = v.str("cwd");
    if (const JVal* e = v.get("env")) {
        if (e->t == JVal::OBJ) for (auto& kv : e->o) if (kv.second.t == JVal::STR) env.emplace_back(kv.first, kv.second.s);
    }
#else
    size_t z = raw.find('\0');
    if (z == std::string::npos) return;
    cwd = raw.substr(0, z);
    while (!cwd.empty() && cwd.back() == '\n') cwd.pop_back();
    env = parse_env0(raw.substr(z + 1));
#endif
    if (!cwd.empty() && is_dir(cwd)) {
        std::lock_guard<std::mutex> lock(j.m);
        j.cwd = cwd;
    }
    g_session.update(env);
}

void job_waiter(JobPtr j) {
    int code = -1;
#ifdef _WIN32
    WaitForSingleObject(j->p.proc, INFINITE);
    DWORD ec = 1;
    GetExitCodeProcess(j->p.proc, &ec);
    code = (int)ec;
#else
    int status = 0;
    while (waitpid(j->p.pid, &status, 0) < 0 && errno == EINTR) {}
    code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? -WTERMSIG(status) : -1;
#endif
    {
        std::unique_lock<std::mutex> lock(j->m);
        j->cv.wait_for(lock, std::chrono::milliseconds(1500), [&] { return j->reader_done; });
    }
    load_state(*j);
    std::lock_guard<std::mutex> lock(j->m);
    j->done = true;
    j->code = code;
    j->secs = std::round((now_s() - j->t0) * 100) / 100;
    if (!j->stdin_closed) {
#ifdef _WIN32
        CloseHandle(j->p.in);
#else
        close(j->p.in);
#endif
        j->stdin_closed = true;
    }
#ifdef _WIN32
    CloseHandle(j->p.proc);
    if (j->p.job) CloseHandle(j->p.job);
    j->p.proc = j->p.job = nullptr;
#endif
    j->cv.notify_all();
}

JobPtr new_job(const std::string& cmd, const std::string& cwd, std::string& err) {
    auto j = std::make_shared<Job>();
    j->id = random_hex(8);
    j->cmd = cmd;
    j->cwd = cwd;
    j->state_file = join(g_tmp, j->id + ".state");
    Env env = g_session.get();
    env_set(env, "ARYA_STATE", j->state_file);
    if (!g_askpass.empty()) env_set(env, "SUDO_ASKPASS", g_askpass);
    j->t0 = now_s();
#ifdef _WIN32
    std::wstring cl = quote_arg(powershell_exe()) + L" -NoLogo -NoProfile -NonInteractive -InputFormat None -OutputFormat Text -Command " +
                      quote_arg(widen(PS_PRE + cmd + PS_POST));
    if (!spawn(cl, cwd, env, j->p, err)) return nullptr;
#else
    std::string script = POSIX_PRE + (g_askpass.empty() ? "" : MAC_SUDO) + cmd + "\n";
    if (!spawn({g_shell, "-c", script}, cwd, env, j->p, err)) return nullptr;
#endif
    std::thread(job_reader, j).detach();
    std::thread(job_waiter, j).detach();
    std::lock_guard<std::mutex> lock(g_jobs_m);
    double now = now_s();
    for (auto it = g_jobs.begin(); it != g_jobs.end();) {
        bool old;
        { std::lock_guard<std::mutex> jl(it->second->m); old = it->second->done && now - it->second->t0 > 900; }
        it = old ? g_jobs.erase(it) : std::next(it);
    }
    g_jobs[j->id] = j;
    return j;
}

JobPtr find_job(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_jobs_m);
    auto it = g_jobs.find(id);
    return it == g_jobs.end() ? nullptr : it->second;
}

std::string job_poll(const JobPtr& j, size_t from) {
    bool done;
    {
        std::unique_lock<std::mutex> lock(j->m);
        if (from >= j->chunks.size() && !j->done)
            j->cv.wait_for(lock, std::chrono::seconds(15), [&] { return j->done || j->chunks.size() > from; });
        done = j->done;
    }
    if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::string venv = g_session.venv();
    std::lock_guard<std::mutex> lock(j->m);
    std::string out;
    for (size_t k = from; k < j->chunks.size(); k++) out += j->chunks[k];
    char secs[32];
    snprintf(secs, sizeof secs, "%.2f", j->secs);
    return "{\"out\":" + jq(out) + ",\"next\":" + std::to_string(j->chunks.size()) + ",\"done\":" + (j->done ? "true" : "false") +
           ",\"code\":" + (j->done ? std::to_string(j->code) : "null") + ",\"cwd\":" + jq(j->cwd) +
           ",\"stopped\":" + (j->stopped ? "true" : "false") + ",\"secs\":" + secs + ",\"venv\":" + jq(j->done ? venv : "") + "}";
}

void job_write(const JobPtr& j, const std::string& text, bool eof) {
    std::lock_guard<std::mutex> lock(j->m);
    if (j->stdin_closed) return;
#ifdef _WIN32
    DWORD w = 0;
    if (!text.empty()) WriteFile(j->p.in, text.data(), (DWORD)text.size(), &w, nullptr);
    if (eof) { CloseHandle(j->p.in); j->stdin_closed = true; }
#else
    size_t off = 0;
    while (off < text.size()) {
        ssize_t n = write(j->p.in, text.data() + off, text.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += (size_t)n;
    }
    if (eof) { close(j->p.in); j->stdin_closed = true; }
#endif
}

void job_stop(const JobPtr& j, bool hard) {
    std::lock_guard<std::mutex> lock(j->m);
    if (j->done) return;
    j->stopped = true;
#ifdef _WIN32
    (void)hard;
    if (j->p.job) TerminateJobObject(j->p.job, 1);
    else if (j->p.proc) TerminateProcess(j->p.proc, 1);
#else
    pid_t pg = j->p.pid;
    if (hard) { killpg(pg, SIGKILL); return; }
    std::thread([pg] {
        const int sigs[3] = {SIGINT, SIGTERM, SIGKILL};
        const int delays[3] = {1200, 1500, 0};
        for (int k = 0; k < 3; k++) {
            if (killpg(pg, sigs[k]) != 0) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(delays[k]));
        }
    }).detach();
#endif
}

bool any_running() {
    std::lock_guard<std::mutex> lock(g_jobs_m);
    for (auto& kv : g_jobs) {
        std::lock_guard<std::mutex> jl(kv.second->m);
        if (!kv.second->done) return true;
    }
    return false;
}

std::string os_label() {
#ifdef _WIN32
    typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi;
    ZeroMemory(&vi, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    auto fn = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (fn) fn(&vi);
    if (vi.dwMajorVersion >= 10) return vi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
    return "Windows " + std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion);
#elif defined(__APPLE__)
    char buf[64] = {0};
    size_t len = sizeof buf;
    if (sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0) == 0) return std::string("macOS ") + buf;
    return "macOS";
#else
    return "Linux";
#endif
}

bool is_admin() {
#ifdef _WIN32
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD sz = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &el, sizeof el, &sz);
    CloseHandle(tok);
    return ok && el.TokenIsElevated;
#else
    return geteuid() == 0;
#endif
}

std::string open_terminal(const std::string& cmd, const std::string& cwd) {
#ifdef _WIN32
    std::string esc_cwd;
    for (char c : cwd) { esc_cwd += c; if (c == '\'') esc_cwd += '\''; }
    std::string script = "Set-Location -LiteralPath '" + esc_cwd + "'\n" + cmd;
    std::string err;
    if (!spawn_console(quote_arg(powershell_exe()) + L" -NoExit -Command " + quote_arg(widen(script)), cwd, g_session.get(), err)) return err;
    return "";
#elif defined(__APPLE__)
    std::string path = join(g_tmp, "arya-" + random_hex(4) + ".command");
    if (!write_file(path, "#!" + g_shell + "\nrm -f \"$0\"\ncd " + shell_quote(cwd) + " || exit 1\nclear\n" + cmd + "\nexec " + g_shell + " -l\n"))
        return "Could not create the terminal script.";
    chmod(path.c_str(), 0700);
    Proc p;
    std::string err;
    if (!spawn({"/usr/bin/open", path}, g_home, current_env(), p, err)) return err;
    close(p.in);
    close(p.out);
    pid_t pid = p.pid;
    std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
    return "";
#else
    (void)cmd; (void)cwd;
    return "Opening a system terminal is supported on macOS and Windows.";
#endif
}

bool run_elevated(const std::string& cmd, const std::string& cwd) {
#ifdef _WIN32
    std::string esc_cwd;
    for (char c : cwd) { esc_cwd += c; if (c == '\'') esc_cwd += '\''; }
    std::wstring params = L"-NoExit -Command " + quote_arg(widen("Set-Location -LiteralPath '" + esc_cwd + "'\n" + cmd));
    HINSTANCE rc = ShellExecuteW(nullptr, L"runas", powershell_exe().c_str(), params.c_str(), widen(cwd).c_str(), SW_SHOWNORMAL);
    return (INT_PTR)rc > 32;
#else
    (void)cmd; (void)cwd;
    return false;
#endif
}

const char* MY_FILE = "my_commands.json";
const char* STATE_FILE = ".arya_state.json";

std::string read_valid(const std::string& path, JVal::Type want, const std::string& fallback) {
    std::string txt;
    JVal v;
    if (read_file(path, txt) && json_parse(txt, v) && v.t == want) return txt;
    return fallback;
}

std::string ui_html_text() {
    std::string override_file = get_env("ARYA_UI_FILE"), txt;
    if (!override_file.empty() && read_file(override_file, txt)) return txt;
    return std::string((const char*)ui_html, ui_html_len);
}

std::string init_payload() {
    std::string lib, lib_error, custom = join(g_data, "commands.json"), txt;
    if (read_file(custom, txt)) {
        JVal v;
        if (json_parse(txt, v) && v.t == JVal::OBJ && v.get("commands") && v.get("commands")->t == JVal::ARR) lib = txt;
        else lib_error = "Your " + custom + " is not valid JSON, so the built-in command library is used instead.";
    }
    if (lib.empty()) lib.assign((const char*)commands_json, commands_json_len);

    std::string state = read_valid(join(g_data, STATE_FILE), JVal::OBJ, "{}");
    std::string mine = read_valid(join(g_data, MY_FILE), JVal::ARR, "[]");
    std::string cwd = g_start_dir;
    g_start_dir.clear();
    if (cwd.empty()) {
        JVal st;
        if (json_parse(state, st) && is_dir(st.str("cwd"))) cwd = st.str("cwd");
        else cwd = g_home;
    }
    return std::string("{\"app\":") + jq(APP_NAME) + ",\"version\":" + jq(VERSION) + ",\"edition\":\"native\"" +
           ",\"os\":" + jq(IS_WIN ? "win" : "mac") + ",\"osName\":" + jq(os_label()) + ",\"shell\":" + jq(g_shell_name) +
           ",\"home\":" + jq(g_home) + ",\"cwd\":" + jq(cwd) + ",\"sep\":" + jq(std::string(1, SEP)) +
           ",\"admin\":" + (is_admin() ? "true" : "false") + ",\"dataDir\":" + jq(g_data) + ",\"customLibrary\":" + jq(custom) +
           ",\"libraryError\":" + jq(lib_error) + ",\"library\":" + lib + ",\"mine\":" + mine + ",\"state\":" + state + "}";
}

std::string api_complete(const JVal& b) {
    std::string cwd = valid_dir(b.str("cwd")), prefix = b.str("prefix");
    if (prefix == "~") return "{\"items\":[{\"n\":\"~\",\"d\":true}]}";
    std::string path = starts_with(prefix, "~") ? expand_home(prefix) : prefix;
    size_t cut = path.find_last_of(IS_WIN ? "/\\" : "/");
    std::string head = cut == std::string::npos ? "" : path.substr(0, cut + 1);
    std::string part = cut == std::string::npos ? path : path.substr(cut + 1);
    std::string base = head.empty() ? cwd : (is_abs(head) ? head : join(cwd, head));
    std::string low = lower(part);
    std::vector<DirEntry> items;
    for (auto& e : list_dir(base)) {
        if (!starts_with(lower(e.name), low)) continue;
        if (e.name[0] == '.' && (part.empty() || part[0] != '.')) continue;
        items.push_back(e);
    }
    std::sort(items.begin(), items.end(), [](const DirEntry& x, const DirEntry& y) {
        return x.dir != y.dir ? x.dir : lower(x.name) < lower(y.name);
    });
    if (items.size() > 300) items.resize(300);
    std::string out = "{\"items\":[";
    for (size_t k = 0; k < items.size(); k++) out += (k ? "," : "") + std::string("{\"n\":") + jq(items[k].name) + ",\"d\":" + (items[k].dir ? "true" : "false") + "}";
    return out + "]}";
}

std::string api_post(const std::string& path, const JVal& b) {
    if (path == "/api/run") {
        std::string cmd = b.str("cmd");
        while (!cmd.empty() && isspace((unsigned char)cmd.back())) cmd.pop_back();
        if (cmd.find_first_not_of(" \t\r\n") == std::string::npos) return "{\"error\":\"Empty command.\"}";
        std::string err;
        JobPtr j = new_job(cmd, valid_dir(b.str("cwd")), err);
        if (!j) return "{\"error\":" + jq("Could not start the command: " + err) + "}";
        return "{\"id\":" + jq(j->id) + ",\"cwd\":" + jq(j->cwd) + "}";
    }
    if (path == "/api/input" || path == "/api/stop") {
        JobPtr j = find_job(b.str("id"));
        if (!j) return "{\"error\":\"Unknown command id.\"}";
        if (path == "/api/stop") job_stop(j, false);
        else job_write(j, b.str("text"), b.flag("eof"));
        return "{\"ok\":true}";
    }
    if (path == "/api/complete") return api_complete(b);
    if (path == "/api/cd") {
        std::string p = resolve(valid_dir(b.str("cwd")), b.str("path"));
        return std::string("{\"cwd\":") + (is_dir(p) ? jq(p) : "null") + "}";
    }
    if (path == "/api/terminal") {
        std::string err = open_terminal(b.str("cmd"), valid_dir(b.str("cwd")));
        return err.empty() ? "{\"ok\":true}" : "{\"error\":" + jq(err) + "}";
    }
    if (path == "/api/elevate") return std::string("{\"ok\":") + (run_elevated(b.str("cmd"), valid_dir(b.str("cwd"))) ? "true" : "false") + "}";
    if (path == "/api/mine") {
        const JVal* items = b.get("items");
        if (!items || items->t != JVal::ARR) return "{\"error\":\"A list is expected.\"}";
        std::string out;
        json_dump(out, *items, 2);
        return write_file(join(g_data, MY_FILE), out + "\n") ? "{\"ok\":true}" : "{\"error\":\"Could not save your commands.\"}";
    }
    if (path == "/api/state") {
        const JVal* st = b.get("state");
        if (st && st->t == JVal::OBJ) { std::string out; json_dump(out, *st); write_file(join(g_data, STATE_FILE), out); }
        return "{\"ok\":true}";
    }
    if (path == "/api/reset") { g_session.reset(); return "{\"ok\":true,\"cwd\":" + jq(g_home) + "}"; }
    if (path == "/api/hello") return "{\"ok\":true}";
    return "";
}

std::mutex g_life_m;
std::map<std::string, double> g_clients;
double g_exit_at = 0;
double g_started = 0;

void touch(const std::string& cid) {
    if (cid.empty()) return;
    std::lock_guard<std::mutex> lock(g_life_m);
    g_clients[cid] = now_s();
    g_exit_at = 0;
}

void bye(const std::string& cid) {
    std::lock_guard<std::mutex> lock(g_life_m);
    g_clients.erase(cid);
    if (g_clients.empty() && !g_window_mode) g_exit_at = now_s() + 6;
}

void watchdog() {
    double prev = now_s();
    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        double now = now_s();
        bool quit = false, idle = false;
        {
            std::lock_guard<std::mutex> lock(g_life_m);
            if (now - prev > 60) for (auto& kv : g_clients) kv.second = now;
            prev = now;
            for (auto it = g_clients.begin(); it != g_clients.end();) it = now - it->second > 1800 ? g_clients.erase(it) : std::next(it);
            if (g_exit_at > 0 && now >= g_exit_at) quit = true;
            idle = g_clients.empty() && g_exit_at == 0 && now - g_started > 120;
        }
        if (quit || (idle && !any_running())) g_quit = true;
    }
}

#ifdef _WIN32
using sock_t = SOCKET;
const sock_t BAD_SOCK = INVALID_SOCKET;
void sock_close(sock_t s) { closesocket(s); }
#else
using sock_t = int;
const sock_t BAD_SOCK = -1;
void sock_close(sock_t s) { close(s); }
#endif

struct Resp { int code; std::string type, body; };
Resp json_resp(int code, const std::string& body) { return {code, "application/json; charset=utf-8", body}; }

bool authed(const std::string& tok) {
    if (tok.size() != g_token.size()) return false;
    unsigned char diff = 0;
    for (size_t k = 0; k < tok.size(); k++) diff |= (unsigned char)(tok[k] ^ g_token[k]);
    return diff == 0;
}

std::string query_param(const std::string& q, const std::string& key) {
    size_t i = 0;
    while (i <= q.size()) {
        size_t amp = q.find('&', i);
        if (amp == std::string::npos) amp = q.size();
        std::string part = q.substr(i, amp - i);
        size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == key) return part.substr(eq + 1);
        i = amp + 1;
    }
    return "";
}

Resp route(const std::string& method, const std::string& target, std::map<std::string, std::string>& h, const std::string& body) {
    if (h["host"] != g_host1 && h["host"] != g_host2) return json_resp(403, "{\"error\":\"forbidden\"}");
    std::string path = target, query;
    size_t q = target.find('?');
    if (q != std::string::npos) { path = target.substr(0, q); query = target.substr(q + 1); }
    if (method == "GET" && (path == "/" || path == "/index.html")) return {200, "text/html; charset=utf-8", ui_html_text()};
    if (method == "GET") {
        if (!authed(h["x-arya-token"])) return json_resp(401, "{\"error\":\"unauthorized\"}");
        touch(h["x-arya-client"]);
        if (path == "/api/init") return json_resp(200, init_payload());
        if (path == "/api/poll") {
            JobPtr j = find_job(query_param(query, "id"));
            if (!j) return json_resp(404, "{\"error\":\"Unknown command id.\"}");
            return json_resp(200, job_poll(j, (size_t)strtoul(query_param(query, "from").c_str(), nullptr, 10)));
        }
        return json_resp(404, "{\"error\":\"not found\"}");
    }
    if (method != "POST") return json_resp(404, "{\"error\":\"not found\"}");
    JVal b;
    if (body.empty()) b.t = JVal::OBJ;
    else if (!json_parse(body, b) || b.t != JVal::OBJ) return json_resp(400, "{\"error\":\"bad request\"}");
    std::string tok = h.count("x-arya-token") ? h["x-arya-token"] : b.str("token");
    if (!authed(tok)) return json_resp(401, "{\"error\":\"unauthorized\"}");
    std::string cid = h.count("x-arya-client") ? h["x-arya-client"] : b.str("cid");
    if (path == "/api/bye") { bye(cid); return json_resp(200, "{\"ok\":true}"); }
    touch(cid);
    std::string out = api_post(path, b);
    return out.empty() ? json_resp(404, "{\"error\":\"not found\"}") : json_resp(200, out);
}

const char* status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        default: return "Internal Server Error";
    }
}

void send_all(sock_t c, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        int n = send(c, data.data() + off, (int)std::min<size_t>(data.size() - off, 1 << 20), 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

void handle_client(sock_t c) {
    std::string req;
    char buf[16384];
    size_t hdr_end = std::string::npos;
    while (req.size() < 65536) {
        int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
        if ((hdr_end = req.find("\r\n\r\n")) != std::string::npos) break;
    }
    Resp r{400, "application/json; charset=utf-8", "{\"error\":\"bad request\"}"};
    if (hdr_end != std::string::npos) {
        size_t line_end = req.find("\r\n");
        std::string line = req.substr(0, line_end);
        size_t s1 = line.find(' '), s2 = line.find(' ', s1 + 1);
        std::map<std::string, std::string> headers;
        size_t pos = line_end + 2;
        while (pos < hdr_end) {
            size_t e = req.find("\r\n", pos);
            std::string hl = req.substr(pos, e - pos);
            size_t colon = hl.find(':');
            if (colon != std::string::npos) {
                std::string v = hl.substr(colon + 1);
                v.erase(0, v.find_first_not_of(" \t"));
                headers[lower(hl.substr(0, colon))] = v;
            }
            pos = e + 2;
        }
        size_t clen = (size_t)strtoul(headers["content-length"].c_str(), nullptr, 10);
        if (s1 != std::string::npos && s2 != std::string::npos && clen <= 4000000) {
            std::string body = req.substr(hdr_end + 4);
            while (body.size() < clen) {
                int n = recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                body.append(buf, (size_t)n);
            }
            if (body.size() >= clen) {
                body.resize(clen);
                try {
                    r = route(line.substr(0, s1), line.substr(s1 + 1, s2 - s1 - 1), headers, body);
                } catch (const std::exception& ex) {
                    r = json_resp(500, "{\"error\":" + jq(ex.what()) + "}");
                }
            }
        } else if (clen > 4000000) {
            r = json_resp(413, "{\"error\":\"too large\"}");
        }
    }
    std::string head = "HTTP/1.1 " + std::to_string(r.code) + " " + status_text(r.code) + "\r\nContent-Type: " + r.type +
                       "\r\nContent-Length: " + std::to_string(r.body.size()) +
                       "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n";
    send_all(c, head + r.body);
#ifdef _WIN32
    shutdown(c, SD_SEND);
#else
    shutdown(c, SHUT_WR);
#endif
    sock_close(c);
}

sock_t g_listen = BAD_SOCK;

int start_server() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_listen = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
#else
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen != BAD_SOCK) fcntl(g_listen, F_SETFD, FD_CLOEXEC);
#endif
    if (g_listen == BAD_SOCK) return -1;
    sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(g_listen, (sockaddr*)&addr, sizeof addr) != 0 || listen(g_listen, 64) != 0) return -1;
    socklen_t len = sizeof addr;
    getsockname(g_listen, (sockaddr*)&addr, &len);
    return ntohs(addr.sin_port);
}

void serve() {
    while (!g_quit) {
#ifdef _WIN32
        WSAPOLLFD pf{g_listen, POLLRDNORM, 0};
        int r = WSAPoll(&pf, 1, 500);
#else
        pollfd pf{g_listen, POLLIN, 0};
        int r = poll(&pf, 1, 500);
#endif
        if (r <= 0) continue;
        sock_t c = accept(g_listen, nullptr, nullptr);
        if (c == BAD_SOCK) continue;
#ifdef _WIN32
        SetHandleInformation((HANDLE)c, HANDLE_FLAG_INHERIT, 0);
        DWORD tmo = 30000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
#else
        fcntl(c, F_SETFD, FD_CLOEXEC);
        timeval tv{30, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#  ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#  endif
#endif
        std::thread(handle_client, c).detach();
    }
}

std::string pick_data_dir() {
    std::string d = get_env("ARYA_DATA_DIR");
    if (d.empty()) {
#ifdef __APPLE__
        d = g_home + "/Library/Application Support/Arya Terminal Assistant";
#elif defined(_WIN32)
        std::string exe_dir = dir_name(exe_path());
        if (!exe_dir.empty() && dir_writable(exe_dir)) return exe_dir;
        d = get_env("APPDATA") + "\\Arya Terminal Assistant";
#else
        std::string xdg = get_env("XDG_CONFIG_HOME");
        d = (xdg.empty() ? g_home + "/.config" : xdg) + "/arya-terminal-assistant";
#endif
    }
    make_dirs(d);
    return d;
}

std::string make_temp_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    std::string d = narrow(std::wstring(buf, n)) + "arya-" + random_hex(6);
    CreateDirectoryW(widen(d).c_str(), nullptr);
    return d;
#else
    std::string base = get_env("TMPDIR");
    if (base.empty()) base = "/tmp";
    std::string tmpl = join(base, "arya-XXXXXX");
    std::vector<char> b(tmpl.begin(), tmpl.end());
    b.push_back('\0');
    return mkdtemp(b.data()) ? std::string(b.data()) : base;
#endif
}

void init_globals() {
#ifdef _WIN32
    g_home = get_env("USERPROFILE");
    g_shell = "powershell.exe";
    g_shell_name = "PowerShell";
#else
    g_home = get_env("HOME");
    if (g_home.empty()) { if (passwd* pw = getpwuid(getuid())) g_home = pw->pw_dir; }
    std::string sh = get_env("SHELL");
    std::string bn = base_name(sh);
    struct stat st;
    if ((bn == "zsh" || bn == "bash") && stat(sh.c_str(), &st) == 0) g_shell = sh;
    else g_shell = stat("/bin/zsh", &st) == 0 ? "/bin/zsh" : "/bin/bash";
    g_shell_name = base_name(g_shell);
#endif
    g_token = random_token(32);
    g_tmp = make_temp_dir();
    g_data = pick_data_dir();
    g_started = now_s();
#ifdef __APPLE__

    g_askpass = join(g_tmp, "askpass.sh");
    write_file(g_askpass,
               "#!/bin/sh\nexec /usr/bin/osascript -e 'display dialog \"A command needs administrator rights (sudo).\" & return & "
               "\"Enter your Mac password:\" with title \"Arya Terminal Assistant\" default answer \"\" with hidden answer with icon caution "
               "buttons {\"Cancel\", \"OK\"} default button 2 cancel button 1' -e 'text returned of result'\n");
    chmod(g_askpass.c_str(), 0700);
#endif
}

#ifndef __APPLE__
void open_ui(const std::string& url) {
#ifdef _WIN32

    const wchar_t* candidates[] = {L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",
                                   L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe",
                                   L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe",
                                   L"%LocalAppData%\\Google\\Chrome\\Application\\chrome.exe"};
    for (const wchar_t* c : candidates) {
        wchar_t exe[MAX_PATH];
        ExpandEnvironmentStringsW(c, exe, MAX_PATH);
        if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) continue;
        std::wstring cl = quote_arg(exe) + L" --app=" + widen(url) + L" --window-size=1280,820 --no-first-run --no-default-browser-check";
        STARTUPINFOW si;
        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        PROCESS_INFORMATION pi;
        if (CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    Proc p;
    std::string err;
    if (spawn({"/usr/bin/xdg-open", url}, g_home, current_env(), p, err)) { close(p.in); close(p.out); }
#endif
}
#endif

}

extern "C" void arya_on_quit(void) {
    g_quit = true;
    std::vector<JobPtr> jobs;
    {
        std::lock_guard<std::mutex> lock(g_jobs_m);
        for (auto& kv : g_jobs) jobs.push_back(kv.second);
    }
    for (auto& j : jobs) job_stop(j, true);
    if (!g_tmp.empty()) remove_tree(g_tmp);
}

extern "C" bool arya_should_quit(void) { return g_quit; }

#ifdef __APPLE__
extern "C" void arya_mac_run(const char* url);
#endif

#ifdef _WIN32
static BOOL WINAPI on_console_ctrl(DWORD) { g_quit = true; return TRUE; }
#else
static void on_signal(int) { g_quit = true; }
#endif

int main(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) { freopen("CONOUT$", "w", stdout); freopen("CONOUT$", "w", stderr); }
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int k = 1; k < wargc; k++) args.push_back(narrow(wargv[k]));
    if (wargv) LocalFree(wargv);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    (void)argc; (void)argv;
#else
    for (int k = 1; k < argc; k++) args.push_back(argv[k]);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif
    bool no_browser = false;
    std::string start;
    for (auto& a : args) {
        if (a == "--no-browser") no_browser = true;
        else if (!starts_with(a, "-") && start.empty()) start = a;
    }
    init_globals();
    if (!start.empty() && is_dir(expand_home(start))) g_start_dir = normalize(expand_home(start));
    g_session.start();

    int port = start_server();
    if (port <= 0) { fprintf(stderr, "Arya: could not start the local server.\n"); return 1; }
    g_host1 = "127.0.0.1:" + std::to_string(port);
    g_host2 = "localhost:" + std::to_string(port);
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/#" + g_token;
    printf("%s %s\n  %s\n  (close the window or press Ctrl+C to quit)\n", APP_NAME, VERSION, url.c_str());
    fflush(stdout);

#ifdef __APPLE__
    if (!no_browser) {
        g_window_mode = true;
        std::thread(serve).detach();
        arya_mac_run(url.c_str());
        arya_on_quit();
        return 0;
    }
#else
    if (!no_browser) open_ui(url);
#endif
    std::thread(watchdog).detach();
    serve();
    arya_on_quit();
    return 0;
}
