// src/ProcessWin.cpp
#ifdef _WIN32

#include "ProcessWin.h"

#include <windows.h>

#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

namespace WinProc {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Les regles de CommandLineToArgvW a l'envers : un argument avec espace ou
// guillemet est mis entre guillemets, les barres inverses qui precedent un
// guillemet sont doublees.
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') { ++slashes; ++i; }
        if (i == a.size()) { out.append(slashes * 2, L'\\'); break; }
        if (a[i] == L'"') { out.append(slashes * 2 + 1, L'\\'); out += L'"'; }
        else              { out.append(slashes, L'\\'); out += a[i]; }
    }
    return out + L"\"";
}

// L'environnement courant, complete par `extra`, sous la forme attendue par
// CreateProcess : NOM=valeur\0 ... \0\0, en UTF-16.
std::wstring environment_block(const std::vector<std::string>& extra) {
    std::map<std::wstring, std::wstring> vars;   // tri : CreateProcess l'exige
    if (LPWCH env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            const std::wstring kv = p;
            const size_t eq = kv.find(L'=', 1);   // « =C:=... » : le nom commence par =
            if (eq != std::wstring::npos) vars[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& e : extra) {
        const std::wstring kv = widen(e);
        const size_t eq = kv.find(L'=');
        if (eq != std::wstring::npos) vars[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    std::wstring block;
    for (const auto& [k, v] : vars) { block += k; block += L'='; block += v; block += L'\0'; }
    block += L'\0';
    return block;
}

std::mutex g_mutex;
std::map<int, HANDLE> g_handles;

struct Started { HANDLE process = nullptr; int pid = -1; };

// `out_write` : si non nul, l'extremite d'ecriture du tube donnee en stdout.
Started start(const std::vector<std::string>& args, const std::vector<std::string>& env,
              HANDLE out_write) {
    Started s;
    if (args.empty()) return s;

    std::wstring cmd;
    for (const auto& a : args) { if (!cmd.empty()) cmd += L' '; cmd += quote(widen(a)); }

    // Un programme donne par chemin complet tourne dans son dossier : c'est la
    // que FinalBurn Neo et MAME cherchent leurs fichiers.
    std::wstring cwd;
    const std::wstring exe = widen(args[0]);
    if (exe.find_first_of(L"\\/") != std::wstring::npos) {
        const size_t cut = exe.find_last_of(L"\\/");
        cwd = exe.substr(0, cut);
    }

    STARTUPINFOW si{};
    si.cb = sizeof si;
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (out_write) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out_write;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = nullptr;
        flags |= CREATE_NO_WINDOW;
    }

    std::wstring envblock = environment_block(env);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, out_write != nullptr, flags,
                        envblock.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        std::cerr << "[ERROR] CreateProcess failed (" << GetLastError() << ") for: " << args[0] << std::endl;
        return s;
    }
    CloseHandle(pi.hThread);
    s.process = pi.hProcess;
    s.pid = (int)pi.dwProcessId;
    return s;
}

}  // namespace

int spawn(const std::vector<std::string>& args, const std::vector<std::string>& env) {
    const Started s = start(args, env, nullptr);
    if (s.pid < 0) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handles[s.pid] = s.process;
    return s.pid;
}

int wait(int pid) {
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(pid);
        if (it == g_handles.end()) return -1;
        h = it->second;
    }
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    const int result = GetExitCodeProcess(h, &code) ? (int)code : -1;
    CloseHandle(h);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handles.erase(pid);
    return result;
}

bool running(int pid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(pid);
    return it != g_handles.end() && WaitForSingleObject(it->second, 0) == WAIT_TIMEOUT;
}

bool run_streaming(const std::vector<std::string>& args, const std::vector<std::string>& env,
                   const std::function<bool(const char*, size_t)>& sink,
                   int timeout_ms, bool* timed_out) {
    if (timed_out) *timed_out = false;

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   // le fils ne herite que de l'ecriture

    const Started s = start(args, env, wr);
    CloseHandle(wr);
    if (s.pid < 0) { CloseHandle(rd); return false; }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[1 << 16];
    bool keep = true, killed = false;
    while (keep) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;   // tube ferme : fin
        if (avail == 0) {
            if (WaitForSingleObject(s.process, 0) != WAIT_TIMEOUT) {
                // Fini : un dernier passage pour ce qui reste dans le tube.
                if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
            } else {
                if (timeout_ms > 0 && std::chrono::steady_clock::now() > deadline) {
                    TerminateProcess(s.process, 1);
                    killed = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
        }
        DWORD got = 0;
        if (!ReadFile(rd, buf, (DWORD)sizeof buf, &got, nullptr) || got == 0) break;
        keep = sink(buf, got);
    }
    if (!keep) TerminateProcess(s.process, 1);   // l'appelant n'en veut plus
    CloseHandle(rd);
    WaitForSingleObject(s.process, 5000);
    CloseHandle(s.process);
    if (timed_out) *timed_out = killed;
    return true;
}

}  // namespace WinProc

#endif
