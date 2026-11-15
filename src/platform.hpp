#pragma once
#include "common.hpp"

#ifndef _WIN32
extern char** environ;
#endif

namespace arya {

inline std::string get_env(const char* name) {
#ifdef _WIN32
    const wchar_t* v = _wgetenv(widen(name).c_str());
    return v ? narrow(v) : std::string();
#else
    const char* v = getenv(name);
    return v ? v : "";
#endif
}

inline bool is_dir(const std::string& p) {
    if (p.empty()) return false;
#ifdef _WIN32
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

inline bool is_abs(const std::string& p) {
#ifdef _WIN32
    return (p.size() >= 3 && isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) ||
           starts_with(p, "\\\\") || starts_with(p, "//");
#else
    return !p.empty() && p[0] == '/';
#endif
}

inline std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == SEP) return a + b;
    return a + SEP + b;
}

inline std::string base_name(std::string p) {
    while (p.size() > 1 && (p.back() == '/' || p.back() == SEP)) p.pop_back();
    size_t k = p.find_last_of(IS_WIN ? "/\\" : "/");
    return k == std::string::npos ? p : p.substr(k + 1);
}

inline std::string dir_name(const std::string& p) {
    size_t k = p.find_last_of(IS_WIN ? "/\\" : "/");
    return k == std::string::npos ? std::string() : p.substr(0, k == 0 ? 1 : k);
}

inline std::string normalize(const std::string& path) {
#ifdef _WIN32
    std::wstring w = widen(path);
    DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (!n) return path;
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(w.c_str(), n, &out[0], nullptr);
    out.resize(n);
    std::string r = narrow(out);
    if (r.size() > 3 && r.back() == '\\') r.pop_back();
    return r;
#else
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            if (cur == "..") { if (!parts.empty()) parts.pop_back(); }
            else if (!cur.empty() && cur != ".") parts.push_back(cur);
            cur.clear();
        } else {
            cur += path[i];
        }
    }
    std::string r;
    for (auto& p : parts) r += "/" + p;
    return r.empty() ? "/" : r;
#endif
}

inline bool read_file(const std::string& path, std::string& out) {
#ifdef _WIN32
    FILE* f = _wfopen(widen(path).c_str(), L"rb");
#else
    FILE* f = fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

inline bool write_file(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
#ifdef _WIN32
    FILE* f = _wfopen(widen(tmp).c_str(), L"wb");
#else
    FILE* f = fopen(tmp.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) return false;
#ifdef _WIN32
    return MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

inline void remove_file(const std::string& path) {
#ifdef _WIN32
    DeleteFileW(widen(path).c_str());
#else
    unlink(path.c_str());
#endif
}

inline bool make_dirs(const std::string& path) {
    if (path.empty() || is_dir(path)) return true;
    std::string parent = dir_name(path);
    if (!parent.empty() && parent != path && !is_dir(parent)) make_dirs(parent);
#ifdef _WIN32
    return CreateDirectoryW(widen(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

struct DirEntry { std::string name; bool dir; };

inline std::vector<DirEntry> list_dir(const std::string& dir) {
    std::vector<DirEntry> out;
#ifdef _WIN32
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(join(dir, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = narrow(fd.cFileName);
        if (n == "." || n == "..") continue;
        out.push_back({n, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        bool isd = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) isd = is_dir(join(dir, n));
        out.push_back({n, isd});
    }
    closedir(d);
#endif
    return out;
}

inline void remove_tree(const std::string& dir) {
    for (auto& e : list_dir(dir)) {
        std::string p = join(dir, e.name);
        if (e.dir) remove_tree(p);
        else remove_file(p);
    }
#ifdef _WIN32
    RemoveDirectoryW(widen(dir).c_str());
#else
    rmdir(dir.c_str());
#endif
}

inline bool dir_writable(const std::string& dir) {
    std::string probe = join(dir, ".arya-write-test-" + random_hex(4));
    if (!write_file(probe, "x")) return false;
    remove_file(probe);
    return true;
}

inline std::string exe_path() {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    return narrow(std::wstring(buf, n));
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    return _NSGetExecutablePath(buf, &size) == 0 ? std::string(buf) : std::string();
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
#endif
}

using Env = std::vector<std::pair<std::string, std::string>>;

inline bool env_key_eq(const std::string& a, const std::string& b) { return IS_WIN ? iequals(a, b) : a == b; }

inline std::string env_get(const Env& env, const std::string& key) {
    for (auto& kv : env) if (env_key_eq(kv.first, key)) return kv.second;
    return "";
}

inline void env_set(Env& env, const std::string& key, const std::string& val) {
    for (auto& kv : env) if (env_key_eq(kv.first, key)) { kv.second = val; return; }
    env.emplace_back(key, val);
}

inline Env current_env() {
    Env env;
#ifdef _WIN32
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) return env;
    for (wchar_t* p = block; *p; p += wcslen(p) + 1) {
        std::wstring item(p);
        size_t eq = item.find(L'=', 1);
        if (item[0] == L'=' || eq == std::wstring::npos) continue;
        env.emplace_back(narrow(item.substr(0, eq)), narrow(item.substr(eq + 1)));
    }
    FreeEnvironmentStringsW(block);
#else
    for (char** e = environ; e && *e; e++) {
        const char* eq = strchr(*e, '=');
        if (eq && eq != *e) env.emplace_back(std::string(*e, eq - *e), std::string(eq + 1));
    }
#endif
    return env;
}

inline Env parse_env0(const std::string& raw) {
    Env env;
    size_t i = 0;
    while (i < raw.size()) {
        size_t z = raw.find('\0', i);
        if (z == std::string::npos) z = raw.size();
        std::string item = raw.substr(i, z - i);
        size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0) env.emplace_back(item.substr(0, eq), item.substr(eq + 1));
        i = z + 1;
    }
    return env;
}

#ifdef _WIN32
struct Proc { HANDLE proc = nullptr, job = nullptr, in = nullptr, out = nullptr; };

inline std::string win_error(DWORD code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   (LPWSTR)&msg, 0, nullptr);
    std::string s = msg ? narrow(msg) : "error " + std::to_string(code);
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

inline std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (size_t i = 0;; i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { i++; bs++; }
        if (i == a.size()) { r.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; }
        else { r.append(bs, L'\\'); r += a[i]; }
    }
    return r + L"\"";
}

inline std::wstring env_block(const Env& env) {
    std::vector<std::pair<std::wstring, std::wstring>> v;
    for (auto& kv : env) v.emplace_back(widen(kv.first), widen(kv.second));
    std::sort(v.begin(), v.end(), [](const std::pair<std::wstring, std::wstring>& x, const std::pair<std::wstring, std::wstring>& y) {
        return _wcsicmp(x.first.c_str(), y.first.c_str()) < 0;
    });
    std::wstring blk;
    for (auto& kv : v) { blk += kv.first; blk += L'='; blk += kv.second; blk += L'\0'; }
    blk += L'\0';
    if (v.empty()) blk += L'\0';
    return blk;
}

inline std::wstring powershell_exe() {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring p = std::wstring(sys, n) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES ? p : L"powershell.exe";
}

inline bool spawn(const std::wstring& cmdline, const std::string& cwd, const Env& env, Proc& p, std::string& err) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)) {
        err = win_error(GetLastError());
        for (HANDLE h : {inR, inW, outR, outW}) if (h) CloseHandle(h);
        return false;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf.data();
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    HANDLE inherit[2] = {inR, outW};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);

    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = outW;
    si.lpAttributeList = attrs;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    std::wstring cl = cmdline, wcwd = widen(cwd), blk = env_block(env);
    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
    BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, flags, (LPVOID)blk.c_str(),
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi);
    DWORD e = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(inR);
    CloseHandle(outW);
    if (!ok) {
        err = win_error(e);
        CloseHandle(inW);
        CloseHandle(outR);
        return false;
    }
    p.job = CreateJobObjectW(nullptr, nullptr);
    if (p.job && !AssignProcessToJobObject(p.job, pi.hProcess)) { CloseHandle(p.job); p.job = nullptr; }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p.proc = pi.hProcess;
    p.in = inW;
    p.out = outR;
    return true;
}

inline bool spawn_console(const std::wstring& cmdline, const std::string& cwd, const Env& env, std::string& err) {
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    std::wstring cl = cmdline, wcwd = widen(cwd), blk = env_block(env);
    if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                        (LPVOID)blk.c_str(), wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        err = win_error(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

#else

struct Proc { pid_t pid = -1; int in = -1, out = -1; };

inline std::mutex& spawn_mutex() { static std::mutex m; return m; }

inline bool spawn(const std::vector<std::string>& argv, const std::string& cwd, const Env& env, Proc& p, std::string& err) {
    std::vector<char*> av;
    for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    std::vector<std::string> items;
    for (auto& kv : env) items.push_back(kv.first + "=" + kv.second);
    std::vector<char*> ev;
    for (auto& s : items) ev.push_back(const_cast<char*>(s.c_str()));
    ev.push_back(nullptr);
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 256 || maxfd > 4096) maxfd = 4096;

    std::lock_guard<std::mutex> lock(spawn_mutex());
    int inp[2], outp[2];
    if (pipe(inp) != 0) { err = strerror(errno); return false; }
    if (pipe(outp) != 0) { err = strerror(errno); close(inp[0]); close(inp[1]); return false; }
    for (int fd : {inp[0], inp[1], outp[0], outp[1]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        err = strerror(errno);
        for (int fd : {inp[0], inp[1], outp[0], outp[1]}) close(fd);
        return false;
    }
    if (pid == 0) {
        setsid();
        dup2(inp[0], 0);
        dup2(outp[1], 1);
        dup2(outp[1], 2);
        for (long fd = 3; fd < maxfd; fd++) close((int)fd);
        signal(SIGPIPE, SIG_DFL);
        if (chdir(cwd.c_str()) != 0) {
            const char msg[] = "arya: cannot open the current folder\n";
            ssize_t ignored = write(2, msg, sizeof msg - 1);
            (void)ignored;
            _exit(126);
        }
        execve(av[0], av.data(), ev.data());
        _exit(127);
    }
    close(inp[0]);
    close(outp[1]);
    p.pid = pid;
    p.in = inp[1];
    p.out = outp[0];
    return true;
}

inline std::string shell_quote(const std::string& s) {
    std::string r = "'";
    for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
    return r + "'";
}
#endif

}
