﻿#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <tchar.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <sstream>


typedef NTSTATUS(WINAPI* pNtSuspendProcess)(HANDLE);
pNtSuspendProcess NtSuspendProcess = (pNtSuspendProcess)GetProcAddress(GetModuleHandleW(L"ntdll"), "NtSuspendProcess");


bool enableDebugPrivilege() {
    HANDLE token;
    TOKEN_PRIVILEGES tp;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;

    LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &tp.Privileges[0].Luid);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL result = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(token);
    return result && GetLastError() == ERROR_SUCCESS;
}

void stopVanguardService() {
    system("sc stop vgc > nul");
    system("sc config vgc start= demand > nul");
    std::cout << "[SUCCESS] VGC Stoppppped !!!!! \n";
}

bool isVanguardRunning() {
    SERVICE_STATUS_PROCESS ssp;
    DWORD bytesNeeded;
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return false;

    SC_HANDLE svc = OpenService(scm, L"vgc", SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return false; }

    BOOL result = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    return result && (ssp.dwCurrentState == SERVICE_RUNNING || ssp.dwCurrentState == SERVICE_START_PENDING);
}

bool isProcessRunning(const std::wstring& processName) {
    PROCESSENTRY32W entry; entry.dwSize = sizeof(entry);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName.c_str()) == 0) {
                found = true; break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

void waitForInjectionWindow(int seconds) {
    MessageBoxW(NULL, L"Vanguard EZ!!!!!!\n\n OKを押したらチートをインジェクションできます ", L"EZ", MB_OK | MB_ICONINFORMATION);
    for (int i = seconds; i > 0; --i) {
        std::cout << "[WAIT]" << i << "\r";
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << "\n[TIMEOUT]\n";
}

void freezeTPMPopupProtection() {
    const std::wstring dns = L"svchost.exe";
    PROCESSENTRY32W entry; entry.dwSize = sizeof(entry);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, dns.c_str()) == 0) {
                HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SUSPEND_RESUME, FALSE, entry.th32ProcessID);
                if (hProcess) {
                    WCHAR path[MAX_PATH]; DWORD len = MAX_PATH;
                    if (QueryFullProcessImageNameW(hProcess, 0, path, &len)) {
                        std::wstring full(path);
                        if (full.find(L"dnscache") != std::wstring::npos) {
                            std::wcout << L"[INFO] (PID: " << entry.th32ProcessID << L")\n";
                            NtSuspendProcess(hProcess);
                        }
                    }
                    CloseHandle(hProcess);
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void monitorAndSuspendVGM() {
    const std::wstring target = L"vgm.exe";
    std::wcout << L"[INFO] stop " << target << L"...\n";
    while (true) {
        PROCESSENTRY32W entry; entry.dwSize = sizeof(entry);
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        bool found = false;
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, target.c_str()) == 0) {
                    found = true;
                    DWORD pid = entry.th32ProcessID;
                    HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    THREADENTRY32 te32; te32.dwSize = sizeof(te32);
                    if (Thread32First(hThreadSnap, &te32)) {
                        do {
                            if (te32.th32OwnerProcessID == pid) {
                                HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te32.th32ThreadID);
                                if (hThread) { SuspendThread(hThread); CloseHandle(hThread); }
                            }
                        } while (Thread32Next(hThreadSnap, &te32));
                    }
                    CloseHandle(hThreadSnap);
                    std::wcout << L"[ACTION] Process " << target << L" 一時停止中 (PID: " << pid << L")\n";
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
}


// 管理者権限をチェックし、デバッグ権限を有効に
bool isAdmin() {
    HANDLE token; TOKEN_ELEVATION elev; DWORD sz;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz)) {
            CloseHandle(token);
            return elev.TokenIsElevated;
        }
        CloseHandle(token);
    }
    return false;
}

// tpmcore.dll がロードされた svchost.exe プロセスのみを一時停止
bool processHasDLL(DWORD pid, const std::wstring& dll) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return false;
    HMODULE mods[1024]; DWORD cb;
    bool ok = false;
    if (EnumProcessModules(h, mods, sizeof(mods), &cb)) {
        for (unsigned i = 0; i < cb / sizeof(HMODULE); ++i) {
            wchar_t buf[MAX_PATH];
            if (GetModuleFileNameExW(h, mods[i], buf, MAX_PATH) && wcsstr(buf, dll.c_str())) {
                ok = true; break;
            }
        }
    }
    CloseHandle(h);
    return ok;
}

//  POPupbypass
void bypassTPMPopup() {
    const std::wstring dllTarget = L"tpmcore.dll";
    std::wcout << L"[BYPASS] svchostを検索中 '" << dllTarget << L"'...\n";

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W e;
    e.dwSize = sizeof(e);

    if (Process32FirstW(snap, &e)) {
        do {
            std::wstring exeName = e.szExeFile;

         
            if (_wcsicmp(exeName.c_str(), L"svchost.exe") == 0 && processHasDLL(e.th32ProcessID, dllTarget)) {
                std::wcout << L"[FOUND] svchost.exe com " << dllTarget << L" (PID: " << e.th32ProcessID << L")\n";
                std::wstringstream cmd;
                cmd << L"pssuspend.exe -accepteula " << e.th32ProcessID;
                _wsystem(cmd.str().c_str());
                continue;
            }

            // DNSキャッシュサービスかどうかを確認
            if (_wcsicmp(exeName.c_str(), L"svchost.exe") == 0 || _wcsicmp(exeName.c_str(), L"dnscache") == 0) {
                HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, e.th32ProcessID);
                if (hProcess) {
                    WCHAR path[MAX_PATH];
                    DWORD len = MAX_PATH;
                    if (QueryFullProcessImageNameW(hProcess, 0, path, &len)) {
                        std::wstring fullPath(path);
                        if (fullPath.find(L"dnscache") != std::wstring::npos) {
                            std::wcout << L"[FOUND] dnscache (PID: " << e.th32ProcessID << L")\n";
                            std::wstringstream cmd;
                            cmd << L"pssuspend.exe -accepteula " << e.th32ProcessID;
                            _wsystem(cmd.str().c_str());
                        }
                    }
                    CloseHandle(hProcess);
                }
            }

        } while (Process32NextW(snap, &e));
    }

    CloseHandle(snap);
}


int main() {
    MessageBoxW(NULL, L"ゲームが開いたらOKを押します", L"起動待ち", MB_ICONINFORMATION | MB_OK);
    std::wcout << L"[INFO] waiting boot for VALORANT \n";
    while (!isProcessRunning(L"VALORANT-Win64-Shipping.exe")) std::this_thread::sleep_for(std::chrono::seconds(1));
    std::wcout << L"[SUCCESS] VALORANT !\n";

    if (!isAdmin()) { std::cerr << "[ERROR] ur not administrator \n"; return 1; }
    if (!enableDebugPrivilege()) std::cerr << "[WARN] faild \n";

    stopVanguardService();
    while (isVanguardRunning()) std::this_thread::sleep_for(std::chrono::milliseconds(500));

    waitForInjectionWindow(20);
    bypassTPMPopup();

    monitorAndSuspendVGM();
    return 0;
}
