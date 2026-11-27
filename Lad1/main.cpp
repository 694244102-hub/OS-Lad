#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace {

// ---------- ตัวช่วยทั่วไป ----------

std::string trim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string fit(const std::string& s, std::size_t width) {
    if (s.size() <= width) return s;
    if (width < 4) return s.substr(0, width);
    return s.substr(0, width - 3) + "...";
}

std::string humanSize(uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < 6) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 2)
        << value << ' ' << units[unit];
    return out.str();
}

std::string percent(uint64_t part, uint64_t total) {
    if (total == 0) return "0.0%";
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(part) * 100.0 / static_cast<double>(total)) << '%';
    return out.str();
}

// ---------- 1. CPU ----------

std::string cpuName() {
#if defined(_WIN32)
    HKEY key = nullptr;
    std::string name;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        char buffer[256] = {};
        DWORD size = sizeof(buffer);
        if (RegQueryValueExA(key, "ProcessorNameString", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
            name = trim(buffer);
        }
        RegCloseKey(key);
    }
    return name.empty() ? "Unknown CPU" : name;
#else
    std::ifstream file("/proc/cpuinfo");
    std::string line, model, hardware, processor;
    while (std::getline(file, line)) {
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        if (key == "model name" && model.empty()) model = value;
        else if (key == "Hardware" && hardware.empty()) hardware = value;
        else if (key == "Processor" && processor.empty()) processor = value;
        if (!model.empty()) break; // x86: อ่านเจอชื่อรุ่นแล้วไม่ต้องอ่านต่อ
    }
    if (!model.empty()) return model;
    if (!hardware.empty()) return hardware;   // ARM บางรุ่นใช้ฟิลด์นี้
    if (!processor.empty()) return processor;
    return "Unknown CPU";
#endif
}

// ---------- 2. Memory ----------

struct MemoryInfo {
    uint64_t totalKb = 0;
    uint64_t availableKb = 0;
};

MemoryInfo memoryInfo() {
    MemoryInfo info;
#if defined(_WIN32)
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        info.totalKb = status.ullTotalPhys / 1024;
        info.availableKb = status.ullAvailPhys / 1024;
    }
#else
    std::ifstream file("/proc/meminfo");
    std::string line;
    uint64_t freeKb = 0, buffersKb = 0, cachedKb = 0;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string key;
        uint64_t value = 0;
        std::string unit;
        if (!(iss >> key >> value >> unit)) continue;
        if (key == "MemTotal:") info.totalKb = value;
        else if (key == "MemAvailable:") info.availableKb = value;
        else if (key == "MemFree:") freeKb = value;
        else if (key == "Buffers:") buffersKb = value;
        else if (key == "Cached:") cachedKb = value;
    }
    // เคอร์เนลเก่า ๆ อาจไม่มี MemAvailable ให้คำนวณเอาเอง
    if (info.availableKb == 0) info.availableKb = freeKb + buffersKb + cachedKb;
#endif
    return info;
}

// ---------- 3. OS ----------

struct OsInfo {
    std::string name;
    std::string kernel;
    std::string arch;
    std::string hostname;
};

#if defined(_WIN32)
std::string registryString(const char* subKey, const char* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    char buffer[1024] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    std::string result;
    if (RegQueryValueExA(key, valueName, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        result = trim(buffer);
    }
    RegCloseKey(key);
    return result;
}
#endif

OsInfo osInfo() {
    OsInfo info;
#if defined(_WIN32)
    info.name = registryString("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName");
    std::string version = registryString("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion");
    std::string build = registryString("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild");
    if (!version.empty()) info.name += " " + version;
    if (!build.empty()) info.name += " (Build " + build + ")";
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: info.arch = "x86_64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL:  info.arch = "x86"; break;
        case PROCESSOR_ARCHITECTURE_ARM64:  info.arch = "ARM64"; break;
        case PROCESSOR_ARCHITECTURE_ARM:    info.arch = "ARM"; break;
        default: info.arch = "unknown";
    }
    char host[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = sizeof(host);
    if (GetComputerNameA(host, &size)) info.hostname = host;
    info.kernel = "Windows NT";
#else
    std::ifstream release("/etc/os-release");
    std::string line;
    while (std::getline(release, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            info.name = trim(line.substr(std::string("PRETTY_NAME=").size()));
            if (info.name.size() >= 2 && info.name.front() == '"' && info.name.back() == '"')
                info.name = info.name.substr(1, info.name.size() - 2);
            break;
        }
    }

    struct utsname uts = {};
    bool ok = uname(&uts) == 0;
    if (info.name.empty() && ok) info.name = uts.sysname;
    if (ok) {
        info.kernel = std::string(uts.sysname) + " " + uts.release + " (" + uts.version + ")";
        info.arch = uts.machine;
        info.hostname = uts.nodename;
    }
#endif
    return info;
}

// ---------- 4. Storage ----------

struct DiskInfo {
    std::string mount;
    std::string type;
    uint64_t total = 0;
    uint64_t freeBytes = 0;
    uint64_t available = 0;
};

std::vector<DiskInfo> disks() {
    std::vector<DiskInfo> result;
#if defined(_WIN32)
    DWORD drives = GetLogicalDrives();
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        if (!(drives & (1u << (letter - 'A')))) continue;
        std::string root = std::string(1, letter) + ":\\";
        if (GetDriveTypeA(root.c_str()) != DRIVE_FIXED) continue;
        ULARGE_INTEGER available, total, totalFree;
        if (!GetDiskFreeSpaceExA(root.c_str(), &available, &total, &totalFree)) continue;
        DiskInfo disk;
        disk.mount = root;
        disk.type = "NTFS";
        disk.total = total.QuadPart;
        disk.freeBytes = totalFree.QuadPart;
        disk.available = available.QuadPart;
        result.push_back(disk);
    }
#else
    // กรองระบบไฟล์เสมือน (proc/sys/tmpfs/...) ออกไป
    static const std::vector<std::string> ignored = {
        "proc", "sysfs", "devpts", "devtmpfs", "tmpfs", "cgroup", "cgroup2",
        "securityfs", "pstore", "bpf", "tracefs", "debugfs", "hugetlbfs",
        "ramfs", "mqueue", "fusectl", "configfs", "autofs", "binfmt_misc",
        "nsfs", "overlayfs"
    };
    std::ifstream file("/proc/mounts");
    std::string line;
    std::vector<dev_t> seenDevices; // ไม่แสดง bind mount ซ้ำของไดรฟ์เดิม
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string device, mount, type, options, rest;
        if (!(iss >> device >> mount >> type >> options)) continue;
        if (std::find(ignored.begin(), ignored.end(), type) != ignored.end()) continue;
        if (mount.rfind("/proc", 0) == 0 || mount.rfind("/sys", 0) == 0 ||
            mount.rfind("/dev", 0) == 0) continue;

        // /proc/mounts มีทั้ง bind mount ของไฟล์เดี่ยว เช่น /etc/hosts
        struct stat pathStat = {};
        if (stat(mount.c_str(), &pathStat) != 0 || !S_ISDIR(pathStat.st_mode)) continue;
        if (std::find(seenDevices.begin(), seenDevices.end(), pathStat.st_dev) != seenDevices.end())
            continue;

        struct statvfs stat = {};
        if (statvfs(mount.c_str(), &stat) != 0 || stat.f_blocks == 0) continue;
        DiskInfo disk;
        disk.mount = mount;
        disk.type = type;
        disk.total = static_cast<uint64_t>(stat.f_blocks) * stat.f_frsize;
        disk.freeBytes = static_cast<uint64_t>(stat.f_bfree) * stat.f_frsize;
        disk.available = static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
        seenDevices.push_back(pathStat.st_dev);
        result.push_back(disk);
    }
#endif
    if (result.empty()) { // กรณีอ่านรายการไดรฟ์ไม่ได้ ให้แสดงอย่างน้อย root
        DiskInfo disk;
#if defined(_WIN32)
        disk.mount = "C:\\";
#else
        disk.mount = "/";
#endif
#if defined(_WIN32)
        ULARGE_INTEGER available, total, totalFree;
        if (GetDiskFreeSpaceExA(disk.mount.c_str(), &available, &total, &totalFree)) {
            disk.total = total.QuadPart;
            disk.freeBytes = totalFree.QuadPart;
            disk.available = available.QuadPart;
        }
#else
        struct statvfs stat = {};
        if (statvfs(disk.mount.c_str(), &stat) == 0) {
            disk.total = static_cast<uint64_t>(stat.f_blocks) * stat.f_frsize;
            disk.freeBytes = static_cast<uint64_t>(stat.f_bfree) * stat.f_frsize;
            disk.available = static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
        }
#endif
        result.push_back(disk);
    }
    return result;
}

// ---------- 5. Processes ----------

struct ProcessInfo {
    uint64_t pid = 0;
    std::string name;
    std::string state;
};

std::vector<ProcessInfo> processes() {
    std::vector<ProcessInfo> result;
#if defined(_WIN32)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32A entry = {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstA(snapshot, &entry)) {
            do {
                result.push_back({entry.th32ProcessID, trim(entry.szExeFile), "running"});
            } while (Process32NextA(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
#else
    DIR* dir = opendir("/proc");
    if (dir != nullptr) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            std::string id = entry->d_name;
            if (id.empty() || !std::all_of(id.begin(), id.end(),
                                           [](unsigned char c) { return std::isdigit(c); })) continue;

            std::ifstream stat("/proc/" + id + "/stat");
            std::string content;
            std::getline(stat, content);
            if (content.empty()) continue;

            // รูปแบบ: pid (comm) state ...  ชื่ออาจมีวงเล็บ/ช่องว่างได้
            std::size_t open = content.find('(');
            std::size_t close = content.rfind(')');
            if (open == std::string::npos || close == std::string::npos || close <= open) continue;

            ProcessInfo proc;
            proc.pid = std::stoull(id);
            proc.name = content.substr(open + 1, close - open - 1);
            std::istringstream after(content.substr(close + 1));
            char state = '?';
            after >> state >> std::ws;
            switch (state) {
                case 'R': proc.state = "running"; break;
                case 'S': proc.state = "sleeping"; break;
                case 'D': proc.state = "disk sleep"; break;
                case 'T': proc.state = "stopped"; break;
                case 'Z': proc.state = "zombie"; break;
                case 'I': proc.state = "idle"; break;
                default:  proc.state = std::string(1, state); break;
            }
            result.push_back(proc);
        }
        closedir(dir);
    }
#endif
    std::sort(result.begin(), result.end(),
              [](const ProcessInfo& a, const ProcessInfo& b) { return a.pid < b.pid; });
    return result;
}

void line(char ch = '=', int width = 68) {
    std::cout << std::string(width, ch) << '\n';
}

} // namespace

int main() {
    line();
    std::cout << " SYSTEM INFORMATION\n";
    line();

    // 1. CPU
    line('-');
    std::cout << "[1] CPU\n";
    line('-');
    std::cout << "  Model         : " << cpuName() << '\n';
    std::cout << "  Logical cores : " << std::thread::hardware_concurrency() << '\n';

    // 2. Memory
    MemoryInfo mem = memoryInfo();
    uint64_t totalBytes = mem.totalKb * 1024ULL;
    uint64_t availableBytes = mem.availableKb * 1024ULL;
    uint64_t usedBytes = (totalBytes > availableBytes) ? totalBytes - availableBytes : 0;
    line('-');
    std::cout << "[2] MEMORY\n";
    line('-');
    std::cout << "  Total         : " << humanSize(totalBytes) << '\n';
    std::cout << "  Available     : " << humanSize(availableBytes) << '\n';
    std::cout << "  Used          : " << humanSize(usedBytes)
              << " (" << percent(usedBytes, totalBytes) << ")\n";

    // 3. OS
    OsInfo os = osInfo();
    line('-');
    std::cout << "[3] OPERATING SYSTEM\n";
    line('-');
    std::cout << "  Name          : " << (os.name.empty() ? "Unknown" : os.name) << '\n';
    std::cout << "  Kernel        : " << (os.kernel.empty() ? "Unknown" : os.kernel) << '\n';
    std::cout << "  Architecture  : " << (os.arch.empty() ? "Unknown" : os.arch) << '\n';
    std::cout << "  Hostname      : " << (os.hostname.empty() ? "Unknown" : os.hostname) << '\n';

    // 4. Storage
    std::vector<DiskInfo> disksList = disks();
    line('-');
    std::cout << "[4] STORAGE\n";
    line('-');
    std::cout << "  " << std::left << std::setw(24) << "MOUNT"
              << std::setw(12) << "TYPE"
              << std::setw(13) << "TOTAL"
              << std::setw(13) << "USED"
              << std::setw(13) << "FREE" << "USE%\n";
    for (const DiskInfo& disk : disksList) {
        uint64_t used = (disk.total > disk.freeBytes) ? disk.total - disk.freeBytes : 0;
        std::cout << "  " << std::left << std::setw(24) << fit(disk.mount, 23)
                  << std::setw(12) << fit(disk.type, 11)
                  << std::setw(13) << humanSize(disk.total)
                  << std::setw(13) << humanSize(used)
                  << std::setw(13) << humanSize(disk.available)
                  << percent(disk.total - disk.available, disk.total) << '\n';
    }

    // 5. Processes
    std::vector<ProcessInfo> procList = processes();
    line('-');
    std::cout << "[5] PROCESSES  (total: " << procList.size() << ")\n";
    line('-');
    std::cout << "  " << std::left << std::setw(10) << "PID"
              << std::setw(28) << "NAME" << "STATE\n";
    for (const ProcessInfo& proc : procList) {
        std::cout << "  " << std::left << std::setw(10) << proc.pid
                  << std::setw(28) << fit(proc.name, 27) << proc.state << '\n';
    }

    line();
    return 0;
}
