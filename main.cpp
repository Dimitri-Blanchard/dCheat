#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <string>
#include <future>
#include <mutex>
#include <fstream>
#include <sstream>

struct FoundAddress {
    uintptr_t address;
    int value;
};

// Globals
HANDLE hProcess = nullptr;
DWORD pid = 0;
std::vector<FoundAddress> results;
std::mutex resultsMutex;

// --- Fonctions basiques de lecture/écriture mémoire ---
bool ReadInt(HANDLE hProcess, uintptr_t address, int& outValue) {
    return ReadProcessMemory(hProcess, (LPCVOID)address, &outValue, sizeof(int), nullptr) != 0;
}

bool WriteInt(HANDLE hProcess, uintptr_t address, int value) {
    return WriteProcessMemory(hProcess, (LPVOID)address, &value, sizeof(int), nullptr) != 0;
}

// --- Scan d'une région mémoire ---
std::vector<FoundAddress> ScanRegion(HANDLE hProcess, uintptr_t start, uintptr_t end, int searchValue) {
    std::vector<FoundAddress> localResults;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = start;

    while (addr < end) {
        if (VirtualQueryEx(hProcess, (LPCVOID)addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;

        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {

            SIZE_T regionSize = mbi.RegionSize;
            std::vector<int> temp(regionSize / sizeof(int));

            SIZE_T bytesRead;
            if (ReadProcessMemory(hProcess, mbi.BaseAddress, temp.data(), regionSize, &bytesRead)) {
                for (SIZE_T i = 0; i < bytesRead / sizeof(int); i++) {
                    if (temp[i] == searchValue) {
                        localResults.push_back({ (uintptr_t)mbi.BaseAddress + i * sizeof(int), temp[i] });
                    }
                }
            }
        }
        addr += mbi.RegionSize;
    }
    return localResults;
}

// --- Scan initial asynchrone ---
std::vector<FoundAddress> FirstScan(HANDLE hProcess, int searchValue) {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);

    uintptr_t start = (uintptr_t)sysInfo.lpMinimumApplicationAddress;
    uintptr_t end = (uintptr_t)sysInfo.lpMaximumApplicationAddress;

    const int numThreads = std::thread::hardware_concurrency();
    uintptr_t chunkSize = (end - start) / numThreads;

    std::vector<std::future<std::vector<FoundAddress>>> futures;
    for (int i = 0; i < numThreads; i++) {
        uintptr_t chunkStart = start + i * chunkSize;
        uintptr_t chunkEnd = (i == numThreads - 1) ? end : chunkStart + chunkSize;

        futures.push_back(std::async(std::launch::async, [chunkStart, chunkEnd, hProcess, searchValue]() {
            return ScanRegion(hProcess, chunkStart, chunkEnd, searchValue);
            }));
    }

    std::vector<FoundAddress> found;
    for (auto& f : futures) {
        auto partial = f.get();
        std::lock_guard<std::mutex> lock(resultsMutex);
        found.insert(found.end(), partial.begin(), partial.end());
    }

    return found;
}

// --- Scan intelligent : filtrage progressif ---
void NextScan(int searchValue) {
    std::vector<FoundAddress> filtered;
    std::mutex localMutex;

    std::vector<std::future<void>> futures;
    const int numThreads = std::thread::hardware_concurrency();
    size_t chunk = results.size() / numThreads;

    for (int t = 0; t < numThreads; t++) {
        size_t start = t * chunk;
        size_t end = (t == numThreads - 1) ? results.size() : start + chunk;

        futures.push_back(std::async(std::launch::async, [start, end, searchValue, &filtered, &localMutex]() {
            std::vector<FoundAddress> temp;
            for (size_t i = start; i < end; i++) {
                int val;
                if (ReadInt(hProcess, results[i].address, val) && val == searchValue) {
                    temp.push_back({ results[i].address, val });
                }
            }
            std::lock_guard<std::mutex> lock(localMutex);
            filtered.insert(filtered.end(), temp.begin(), temp.end());
            }));
    }

    for (auto& f : futures) f.get();

    std::lock_guard<std::mutex> lock(resultsMutex);
    results = filtered;
}

// --- PID depuis nom ---
DWORD GetPIDByName(const std::wstring& processName) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, processName.c_str()) == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    return pid;
}

// --- Sauvegarde / reload ---
void SaveResults(const std::string& filename) {
    std::ofstream file(filename);
    for (auto& r : results) file << std::hex << r.address << " " << std::dec << r.value << "\n";
}

void LoadResults(const std::string& filename) {
    std::ifstream file(filename);
    results.clear();
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        uintptr_t addr;
        int val;
        iss >> std::hex >> addr >> std::dec >> val;
        results.push_back({ addr, val });
    }
}

// --- Interface console enrichie ---
void PrintResults() {
    std::lock_guard<std::mutex> lock(resultsMutex);
    for (auto& r : results) {
        std::cout << "0x" << std::hex << r.address << " -> " << std::dec << r.value << std::endl;
    }
}

int wmain() {
    std::wcout << L"=== Async Memory Scanner v2 ===\n";
    std::wcout << L"Enter process name or PID: ";
    std::wstring input;
    std::getline(std::wcin, input);

    if (iswdigit(input[0])) pid = std::stoul(input);
    else pid = GetPIDByName(input);

    if (pid == 0) {
        std::wcerr << L"Process not found!" << std::endl;
        return 1;
    }

    hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess) {
        std::wcerr << L"Cannot open process!" << std::endl;
        return 1;
    }

    int choice;
    do {
        std::wcout << L"\n1: First Scan\n2: Next Scan\n3: Edit Value\n4: Save\n5: Load\n6: Print Results\n0: Exit\nChoice: ";
        std::wcin >> choice;
        std::wcin.ignore();

        if (choice == 1) {
            int searchValue;
            std::wcout << L"Value to search: ";
            std::wcin >> searchValue;
            std::wcin.ignore();
            results = FirstScan(hProcess, searchValue);
            PrintResults();
        }
        else if (choice == 2) {
            int searchValue;
            std::wcout << L"New value to filter: ";
            std::wcin >> searchValue;
            std::wcin.ignore();
            NextScan(searchValue);
            PrintResults();
        }
        else if (choice == 3) {
            uintptr_t addr;
            int val;
            std::wcout << L"Address to edit (hex): ";
            std::wcin >> std::hex >> addr >> std::dec;
            std::wcout << L"New value: ";
            std::wcin >> val;
            WriteInt(hProcess, addr, val);
        }
        else if (choice == 4) {
            std::string fname;
            std::cout << "Filename to save: ";
            std::cin >> fname;
            SaveResults(fname);
        }
        else if (choice == 5) {
            std::string fname;
            std::cout << "Filename to load: ";
            std::cin >> fname;
            LoadResults(fname);
        }
        else if (choice == 6) {
            PrintResults();
        }
    } while (choice != 0);

    CloseHandle(hProcess);
    return 0;
}
