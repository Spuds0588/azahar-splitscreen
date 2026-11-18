// Azahar Splitscreen — a libretro core that runs one ROM on 2-6 consoles.
//
// This is not a port of the emulator into a core. Azahar's core is
// singleton-heavy (memory hooks, the JIT code cache, global state), so N seats
// cannot share one process: mgba-splitscreen gets away with an in-core array of
// instances, Azahar cannot. Instead this core does what the session window does,
// from inside RetroArch: the boot menu asks how many players to spawn, then the
// core launches `azahar-room` plus N headless `azahar` processes (each with its
// own shared-memory frame ring and input socket), joins them to the room and
// composites their rings into the core's frame. RetroPad port <n> drives seat
// <n>, so RetroArch's controller setup maps 1:1 onto the consoles.
//
// Two consequences worth knowing:
//   * The core needs the emulator binaries beside it (a beta bundle, or
//     AZAHAR_SPLITS_ROOT pointing at one); see find_install().
//   * Each seat writes its audio into a shared-memory ring (`--shm-audio`) instead
//     of opening a device, and this core plays seat 1's ring through RetroArch's
//     audio callback: one stream, one volume, one recorder.
//   * A seat that never draws a frame says why on its tile, reading the loader's
//     verdict out of the seat's own log (an encrypted dump, an unreadable game)
//     or reporting that the process exited before its first frame.

#include "libretro.h"

#ifdef _WIN32
// Winsock's headers have to come before windows.h.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <sys/types.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// libretro callbacks
// ---------------------------------------------------------------------------

retro_environment_t environ_cb = nullptr;
retro_video_refresh_t video_cb = nullptr;
retro_audio_sample_batch_t audio_batch_cb = nullptr;
retro_input_poll_t input_poll_cb = nullptr;
retro_input_state_t input_state_cb = nullptr;
retro_log_printf_t log_cb = nullptr;

// ---------------------------------------------------------------------------
// Frame ring, mirrored from src/citra_qt/shm_frame.h. Deliberately duplicated:
// this core is a launcher and must not link the emulator.
// ---------------------------------------------------------------------------

constexpr uint32_t kMagic = 0x48535A41;      // 'AZSH'
constexpr uint32_t kAudioMagic = 0x41535A41; // 'AZSA'
constexpr size_t kHeaderBytes = 64;
constexpr uint32_t kMaxPlayers = 6;
constexpr unsigned kWidth = 1280;
constexpr unsigned kHeight = 720;
/// Seats mix at the 3DS's native rate (audio_core/audio_types.h).
constexpr unsigned kAudioRate = 32728;
/// One video frame's worth of audio at that rate (the rate does not divide by
/// 60, so the exact count is carried between calls), plus slack.
constexpr size_t kAudioFrames = kAudioRate / 60 + 2;

#ifdef _WIN32
/// The seat binaries carry the platform's extension.
constexpr const char* kEmulatorName = "azahar.exe";
constexpr const char* kRoomName = "azahar-room.exe";
/// A UDP socket, spelled for whichever platform is compiling.
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline void ensure_winsock() {
    static const bool started = [] {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        return true;
    }();
    (void)started;
}
inline int close_socket(socket_t fd) {
    return closesocket(fd);
}
inline int current_pid() {
    return static_cast<int>(GetCurrentProcessId());
}
#else
constexpr const char* kEmulatorName = "azahar";
constexpr const char* kRoomName = "azahar-room";
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline void ensure_winsock() {}
inline int close_socket(socket_t fd) {
    return close(fd);
}
inline int current_pid() {
    return static_cast<int>(getpid());
}
#endif

struct RingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t slot_count;
    uint32_t slot_bytes;
    uint64_t seq;
    uint64_t request;
    uint64_t reserved[2];
};
static_assert(sizeof(RingHeader) == kHeaderBytes, "ring header layout changed");

/// Mirrored from src/audio_core/shm_audio.h: interleaved stereo PCM16.
struct AudioRingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t capacity_frames;
    uint32_t reserved;
    uint64_t write_frames;
    uint64_t reserved2[4];
};
static_assert(sizeof(AudioRingHeader) == kHeaderBytes, "audio ring header layout changed");

/// The 12-byte input datagram the seats listen for (src/citra_qt/input_socket.h).
struct InputMessage {
    uint8_t type;
    uint8_t pad[3];
    uint32_t key;
    uint16_t x;
    uint16_t y;
};
static_assert(sizeof(InputMessage) == 12, "input message must stay 12 bytes");

struct Seat {
    pid_t pid = -1;
    std::string ring_name;
    std::string socket_path;
    std::string user_dir;
    std::string log_path;
    std::string log_label;

    std::string audio_ring_name;
    int audio_fd = -1;
    uint8_t* audio_base = nullptr;
#ifdef _WIN32
    /// A named file mapping has no descriptor to reopen: keep the handle so it
    /// can be closed with its view.
    HANDLE map_handle = nullptr;
    HANDLE audio_map_handle = nullptr;
#endif
    size_t audio_size = 0;
    uint32_t audio_capacity = 0;
    uint64_t audio_read = 0;

    /// Why this seat never appeared; drawn on its tile once known.
    std::string failure;
    double started = 0;
    double last_probe = 0;

    int fd = -1;
    uint8_t* base = nullptr;
    size_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t slot_count = 3;
    uint32_t slot_bytes = 0;
    uint64_t last_seq = 0;

    /// Bit per entry of kPadKeys, so only key changes go on the wire.
    uint32_t held = 0;

    /// One A press per seat, a couple of seconds after its first frame: a fresh
    /// slot has no Mii, so a game can sit on a "no Mii characters" dialog that
    /// waits for A on the touch screen (hidden in a 3+ seat layout).
    bool nudge_done = false;
    double nudge_press_at = 0;
    double nudge_pressed_at = 0;
    /// L held: show this seat's touch screen instead of its action screen.
    bool peek = false;
    float pointer_x = -1;
    float pointer_y = -1;
    int sent_x = -1;
    int sent_y = -1;
};

std::vector<Seat> seats;
pid_t room_pid = -1;
std::string session_dir;
std::string install_dir;

// ---------------------------------------------------------------------------
// Boot menu
// ---------------------------------------------------------------------------

bool in_menu = true;
int menu_players = 2;
std::string rom_path;
std::string failure;

// ---------------------------------------------------------------------------
// Budget: enough text for the menu, nothing more. 5x7 rows, bit 4 = leftmost.
// ---------------------------------------------------------------------------

struct Glyph {
    char code;
    uint8_t rows[7];
};

const Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}},
    {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
    {':', {0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
};

std::vector<uint32_t> frame; // XRGB8888, top-down

void put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= static_cast<int>(kWidth) || y >= static_cast<int>(kHeight)) {
        return;
    }
    frame[static_cast<size_t>(y) * kWidth + x] = color;
}

void fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            put_pixel(x + col, y + row, color);
        }
    }
}

void draw_text(int x, int y, int scale, uint32_t color, const std::string& text) {
    int cursor = x;
    for (char ch : text) {
        if (ch == ' ') {
            cursor += 6 * scale;
            continue;
        }
        const Glyph* glyph = nullptr;
        for (const auto& candidate : kFont) {
            if (candidate.code == ch) {
                glyph = &candidate;
                break;
            }
        }
        if (glyph == nullptr) {
            cursor += 6 * scale;
            continue;
        }
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph->rows[row] & (1u << (4 - col))) == 0) {
                    continue;
                }
                fill_rect(cursor + col * scale, y + row * scale, scale, scale, color);
            }
        }
        cursor += 6 * scale;
    }
}

int text_width(const std::string& text, int scale) {
    return static_cast<int>(text.size()) * 6 * scale;
}

void draw_centered(int y, int scale, uint32_t color, const std::string& text) {
    draw_text(static_cast<int>(kWidth) / 2 - text_width(text, scale) / 2, y, scale, color, text);
}

// ---------------------------------------------------------------------------
// Files, processes, sockets
// ---------------------------------------------------------------------------

std::string home_dir() {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
#endif
    return home != nullptr ? std::string(home) : std::string(".");
}

std::string save_dir() {
    const char* dir = nullptr;
    if (environ_cb != nullptr && environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) &&
        dir != nullptr) {
        return dir;
    }
#ifdef _WIN32
    return home_dir() + "/AppData/Roaming/RetroArch/saves";
#else
    return home_dir() + "/.config/retroarch/saves";
#endif
}

void make_dir(const std::string& path) {
#ifdef _WIN32
    CreateDirectoryA(path.c_str(), nullptr);
#else
    mkdir(path.c_str(), 0755);
#endif
}

void sleep_ms(unsigned milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    usleep(milliseconds * 1000);
#endif
}

std::string dirname_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The end of a log file. Seat logs stay small, but a stall detector must never
/// pull a runaway log into a frame callback.
std::string read_tail(const std::string& path, size_t limit = 65536) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    std::string text;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long size = ftell(file);
        const long start = size > static_cast<long>(limit) ? size - static_cast<long>(limit) : 0;
        if (fseek(file, start, SEEK_SET) == 0) {
            char buffer[8192];
            size_t got = 0;
            while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
                text.append(buffer, got);
            }
        }
    }
    fclose(file);
    return text;
}

/// True when `path` names a file this process could run. Windows has no
/// execute bit, so existence is the whole test there.
bool executable(const std::string& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

std::string exe_dir() {
#ifdef _WIN32
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&retro_init), &self) != 0) {
        char path[MAX_PATH];
        const DWORD length = GetModuleFileNameA(self, path, sizeof(path));
        if (length > 0) {
            return dirname_of(std::string(path, length));
        }
    }
    return ".";
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&retro_init), &info) != 0 && info.dli_fname != nullptr) {
        return dirname_of(info.dli_fname);
    }
    return ".";
#endif
}

/// The folder holding `build/bin/Release/azahar`. A beta bundle puts the core
/// inside the bundle, so the folder above the core is checked too.
std::string find_install() {
    std::vector<std::string> candidates;
    const char* env = getenv("AZAHAR_SPLITS_ROOT");
    if (env != nullptr && *env != '\0') {
        candidates.emplace_back(env);
    }
    const std::string here = exe_dir();
    candidates.push_back(here);
    candidates.push_back(here + "/..");
    candidates.push_back(".");
    for (const auto& candidate : candidates) {
        if (executable(candidate + "/build/bin/Release/" + kEmulatorName) ||
            executable(candidate + "/build/bin/" + kEmulatorName)) {
            return candidate;
        }
    }
    return {};
}

std::string emulator_path(const std::string& install) {
    const std::string released = install + "/build/bin/Release/" + kEmulatorName;
    if (executable(released)) {
        return released;
    }
    return install + "/build/bin/" + kEmulatorName;
}

/// One argument of a Windows command line: quoted when it needs to be, with the
/// backslashes before a quote doubled the way the CRT's parser expects.
std::string quote_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string quoted = "\"";
    size_t backslashes = 0;
    for (const char ch : arg) {
        if (ch == '\\') {
            ++backslashes;
            continue;
        }
        if (ch == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted += '"';
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, '\\');
        backslashes = 0;
        quoted += ch;
    }
    quoted.append(backslashes * 2, '\\');
    quoted += '"';
    return quoted;
}

pid_t spawn_process(const std::string& binary, const std::vector<std::string>& args,
                    const std::string& log_path) {
#ifdef _WIN32
    // No fork/exec: one CreateProcess, with the seat's stdout and stderr going
    // straight into its log file.
    std::string command = quote_arg(binary);
    for (const auto& arg : args) {
        command += " " + quote_arg(arg);
    }
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE log = CreateFileA(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log;
    startup.hStdError = log;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
    if (!started) {
        return -1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess); // terminate() reopens it from the pid
    return static_cast<pid_t>(process.dwProcessId);
#else
    const pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        // Own process group: teardown signals the whole tree.
        setpgid(0, 0);
        const int fd = open(log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        setenv("QT_QPA_PLATFORM", "xcb", 0);
        setenv("QT_SCALE_FACTOR", "1", 1);
        setenv("QT_ENABLE_HIGHDPI_SCALING", "0", 1);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(binary.c_str()));
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execv(binary.c_str(), argv.data());
        _exit(127);
    }
    return pid;
#endif
}

void terminate(pid_t& pid) {
    if (pid <= 0) {
        return;
    }
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process != nullptr) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 3000);
        CloseHandle(process);
    }
    pid = -1;
#else
    kill(-pid, SIGTERM);
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    kill(-pid, SIGKILL);
    pid = -1;
#endif
}

void send_input(Seat& seat, uint8_t type, uint32_t key, uint16_t x, uint16_t y) {
    if (seat.pid <= 0) {
        return;
    }
    InputMessage message{};
    message.type = type;
    message.key = key;
    message.x = x;
    message.y = y;
#ifdef _WIN32
    // A datagram to the seat's loopback port: Windows' stand-in for the unix
    // datagram socket, and non-blocking for free.
    const auto colon = seat.socket_path.find(':');
    if (colon == std::string::npos) {
        return;
    }
    ensure_winsock();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(std::stoi(seat.socket_path.substr(colon + 1))));
    const socket_t fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kInvalidSocket) {
        return;
    }
    sendto(fd, reinterpret_cast<const char*>(&message), sizeof(message), 0,
           reinterpret_cast<sockaddr*>(&address), sizeof(address));
    close_socket(fd);
#else
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (seat.socket_path.size() >= sizeof(address.sun_path)) {
        return;
    }
    std::memcpy(address.sun_path, seat.socket_path.c_str(), seat.socket_path.size());
    const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    sendto(fd, &message, sizeof(message), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&address),
           sizeof(address));
    close(fd);
#endif
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

constexpr uint32_t key_A = 65;
constexpr uint32_t key_B = 83;
constexpr uint32_t key_X = 90;
constexpr uint32_t key_Y = 88;
constexpr uint32_t key_up = 84;
constexpr uint32_t key_down = 71;
constexpr uint32_t key_left = 70;
constexpr uint32_t key_right = 72;
constexpr uint32_t key_start = 77;
constexpr uint32_t key_select = 78;
constexpr uint32_t key_home = 66;
constexpr uint32_t key_R = 87;
constexpr uint32_t key_L = 81;
constexpr uint32_t key_ZL = 49;
constexpr uint32_t key_ZR = 50;

/// RetroPad button -> the key the seat's input profile binds. L is the peek (it
/// shows the touch screen while held, as the session window's peek does) and R3
/// taps the touch pointer.
struct PadKey {
    unsigned id;
    uint32_t key;
};
const PadKey kPadKeys[] = {
    {RETRO_DEVICE_ID_JOYPAD_A, key_A},         {RETRO_DEVICE_ID_JOYPAD_B, key_B},
    {RETRO_DEVICE_ID_JOYPAD_X, key_X},         {RETRO_DEVICE_ID_JOYPAD_Y, key_Y},
    {RETRO_DEVICE_ID_JOYPAD_UP, key_up},       {RETRO_DEVICE_ID_JOYPAD_DOWN, key_down},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, key_left},   {RETRO_DEVICE_ID_JOYPAD_RIGHT, key_right},
    {RETRO_DEVICE_ID_JOYPAD_START, key_start}, {RETRO_DEVICE_ID_JOYPAD_SELECT, key_select},
    {RETRO_DEVICE_ID_JOYPAD_R, key_R},         {RETRO_DEVICE_ID_JOYPAD_L3, key_L},
    {RETRO_DEVICE_ID_JOYPAD_L2, key_ZL},       {RETRO_DEVICE_ID_JOYPAD_R2, key_ZR},
};
constexpr unsigned kPeekButton = RETRO_DEVICE_ID_JOYPAD_L;
constexpr unsigned kTapButton = RETRO_DEVICE_ID_JOYPAD_R3;

#ifdef _WIN32
void close_mapping(HANDLE& handle, uint8_t*& base, size_t& size) {
    if (base != nullptr) {
        UnmapViewOfFile(base);
        base = nullptr;
    }
    if (handle != nullptr) {
        CloseHandle(handle);
        handle = nullptr;
    }
    size = 0;
}

/// Open the named file mapping a seat created, read-only, and hand back its
/// view. Windows rounds a mapping up to a 64 KiB granule, so the size is only a
/// bound: the header's own dimensions say what is live.
bool open_mapping(const std::string& name, HANDLE& handle, uint8_t*& base, size_t& size) {
    close_mapping(handle, base, size);
    handle = OpenFileMappingA(FILE_MAP_READ, FALSE, ("Local\\" + name).c_str());
    if (handle == nullptr) {
        return false;
    }
    void* view = MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        CloseHandle(handle);
        handle = nullptr;
        return false;
    }
    MEMORY_BASIC_INFORMATION region{};
    size = VirtualQuery(view, &region, sizeof(region)) == 0 ? 0 : region.RegionSize;
    base = static_cast<uint8_t*>(view);
    return size >= kHeaderBytes;
}
#endif

bool open_ring(Seat& seat) {
#ifdef _WIN32
    if (!open_mapping(seat.ring_name, seat.map_handle, seat.base, seat.size)) {
        return false;
    }
#elif defined(__ANDROID__)
    // Android's libc has no POSIX shm objects, and nothing there spawns seats.
    return false;
#else
    const std::string name = "/" + seat.ring_name;
    const int fd = shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || info.st_size < static_cast<off_t>(kHeaderBytes)) {
        close(fd);
        return false;
    }
    auto* base = static_cast<uint8_t*>(
        mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_SHARED, fd, 0));
    if (base == MAP_FAILED) {
        close(fd);
        return false;
    }
    seat.fd = fd;
    seat.base = base;
    seat.size = static_cast<size_t>(info.st_size);
#endif
    const auto* header = reinterpret_cast<const RingHeader*>(seat.base);
    if (header->magic != kMagic || header->width == 0 || header->height == 0) {
#ifdef _WIN32
        close_mapping(seat.map_handle, seat.base, seat.size);
#else
        munmap(seat.base, seat.size);
        close(seat.fd);
        seat.fd = -1;
        seat.base = nullptr;
        seat.size = 0;
#endif
        return false;
    }
    seat.width = header->width;
    seat.height = header->height;
    seat.stride = header->stride;
    seat.slot_count = std::max(header->slot_count, 1u);
    seat.slot_bytes = header->slot_bytes;
    return true;
}

void close_rings() {
    for (auto& seat : seats) {
#ifdef _WIN32
        close_mapping(seat.map_handle, seat.base, seat.size);
#else
        if (seat.base != nullptr) {
            munmap(seat.base, seat.size);
            seat.base = nullptr;
        }
        if (seat.fd >= 0) {
            close(seat.fd);
            seat.fd = -1;
        }
#endif
        seat.last_seq = 0;
    }
}

void close_audio_rings() {
    for (auto& seat : seats) {
#ifdef _WIN32
        close_mapping(seat.audio_map_handle, seat.audio_base, seat.audio_size);
#else
        if (seat.audio_base != nullptr) {
            munmap(seat.audio_base, seat.audio_size);
            seat.audio_base = nullptr;
        }
        if (seat.audio_fd >= 0) {
            close(seat.audio_fd);
            seat.audio_fd = -1;
        }
#endif
        seat.audio_size = 0;
        seat.audio_capacity = 0;
        seat.audio_read = 0;
    }
}

/// The seat creates its ring before it boots, so the first probe can miss it.
bool open_audio_ring(Seat& seat) {
    if (seat.audio_base != nullptr || seat.audio_ring_name.empty()) {
        return seat.audio_base != nullptr;
    }
#ifdef _WIN32
    if (!open_mapping(seat.audio_ring_name, seat.audio_map_handle, seat.audio_base,
                      seat.audio_size)) {
        return false;
    }
#elif defined(__ANDROID__)
    return false;
#else
    const int fd = shm_open(("/" + seat.audio_ring_name).c_str(), O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || info.st_size < static_cast<off_t>(kHeaderBytes)) {
        close(fd);
        return false;
    }
    auto* base = static_cast<uint8_t*>(
        mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_SHARED, fd, 0));
    if (base == MAP_FAILED) {
        close(fd);
        return false;
    }
    seat.audio_fd = fd;
    seat.audio_base = base;
    seat.audio_size = static_cast<size_t>(info.st_size);
#endif
    const auto* header = reinterpret_cast<const AudioRingHeader*>(seat.audio_base);
    if (header->magic != kAudioMagic || header->channels != 2 || header->capacity_frames == 0) {
#ifdef _WIN32
        close_mapping(seat.audio_map_handle, seat.audio_base, seat.audio_size);
#else
        munmap(seat.audio_base, seat.audio_size);
        close(seat.audio_fd);
        seat.audio_fd = -1;
        seat.audio_base = nullptr;
        seat.audio_size = 0;
#endif
        return false;
    }
    seat.audio_capacity = header->capacity_frames;
    return true;
}

/// Copy up to `frames` new frames into `mix`, scaling by `gain`.
size_t pull_audio(Seat& seat, int32_t* mix, size_t frames, float gain) {
    if (seat.audio_base == nullptr) {
        return 0;
    }
    const auto* header = reinterpret_cast<const AudioRingHeader*>(seat.audio_base);
    const uint64_t write = __atomic_load_n(&header->write_frames, __ATOMIC_ACQUIRE);
    const uint32_t capacity = seat.audio_capacity;
    if (capacity == 0) {
        return 0;
    }
    if (write < seat.audio_read) {
        seat.audio_read = write; // the seat restarted its ring under us
    }
    // Half a ring of slack. Past that the reader is behind: lose the oldest
    // audio rather than play a session's worth of delay.
    if (write - seat.audio_read > capacity / 2) {
        seat.audio_read = write - capacity / 2;
    }
    const size_t want = std::min<uint64_t>(write - seat.audio_read, frames);
    const auto* samples = reinterpret_cast<const int16_t*>(seat.audio_base + kHeaderBytes);
    for (size_t i = 0; i < want; ++i) {
        const size_t at = static_cast<size_t>((seat.audio_read + i) % capacity) * 2;
        mix[i * 2 + 0] += static_cast<int32_t>(samples[at + 0] * gain);
        mix[i * 2 + 1] += static_cast<int32_t>(samples[at + 1] * gain);
    }
    seat.audio_read += want;
    return want;
}

void stop_session() {
    for (auto& seat : seats) {
        terminate(seat.pid);
#ifndef _WIN32
        if (!seat.socket_path.empty()) {
            unlink(seat.socket_path.c_str());
        }
#endif
#if !defined(_WIN32) && !defined(__ANDROID__)
        if (!seat.ring_name.empty()) {
            shm_unlink(("/" + seat.ring_name).c_str());
        }
        if (!seat.audio_ring_name.empty()) {
            shm_unlink(("/" + seat.audio_ring_name).c_str());
        }
#endif
    }
    close_rings();
    close_audio_rings();
    seats.clear();
    terminate(room_pid);
}

uint16_t free_udp_port() {
    ensure_winsock();
    const socket_t fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kInvalidSocket) {
        return 24872;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t length = sizeof(address);
    uint16_t port = 24872;
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        port = ntohs(address.sin_port);
    }
    close_socket(fd);
    return port;
}

bool start_session(int players) {
    install_dir = find_install();
    if (install_dir.empty()) {
        failure = "Azahar binaries not found. Put the core inside the beta bundle, or set "
                  "AZAHAR_SPLITS_ROOT to the folder that holds build/bin/Release/azahar.";
        return false;
    }
    const std::string emulator = emulator_path(install_dir);
    const std::string room_bin = dirname_of(emulator) + "/" + kRoomName;
    if (!executable(room_bin)) {
        failure = "azahar-room is missing from the Azahar folder (" + dirname_of(emulator) + ")";
        return false;
    }

    session_dir = save_dir() + "/azahar-splitscreen";
    make_dir(save_dir());
    make_dir(session_dir);

    const uint16_t port = free_udp_port();
    const std::string room_log = session_dir + "/room.log";
    room_pid =
        spawn_process(room_bin,
                      {"--room-name", "splitscreen", "--preferred-app", "Azahar Splitscreen",
                       "--port", std::to_string(port), "--max_members", std::to_string(players)},
                      room_log);
    if (room_pid <= 0) {
        failure = "could not start azahar-room";
        return false;
    }
    sleep_ms(700); // the room needs to bind before the seats join

    seats.resize(players);
    const int session_pid = current_pid();
    for (int index = 0; index < players; ++index) {
        Seat& seat = seats[index];
        const std::string name = "Player " + std::to_string(index + 1);
        const std::string suffix = std::to_string(session_pid) + "-" + std::to_string(index + 1);
        seat.ring_name = "azsh-" + suffix;
        seat.audio_ring_name = "azsh-audio-" + suffix;
#ifdef _WIN32
        // A loopback datagram port stands in for the unix datagram socket.
        seat.socket_path = "127.0.0.1:" + std::to_string(free_udp_port());
#else
        seat.socket_path = session_dir + "/" + name + ".sock";
#endif
        seat.user_dir = session_dir + "/p" + std::to_string(index + 1);
        seat.log_path = session_dir + "/" + name + ".log";
        seat.log_label = name + ".log";
        seat.started = now_seconds();
        seat.last_probe = seat.started;
        make_dir(seat.user_dir);
        seat.pid =
            spawn_process(emulator,
                          {"--user", seat.user_dir, "--shm-frame", seat.ring_name, "--shm-audio",
                           seat.audio_ring_name, "--input-socket", seat.socket_path, "--join-room",
                           "127.0.0.1:" + std::to_string(port), "--player-name", name, "--stats",
                           session_dir + "/" + name + ".fps", rom_path},
                          seat.log_path);
        if (seat.pid <= 0) {
            failure = "could not start azahar for player " + std::to_string(index + 1);
            stop_session();
            return false;
        }
    }
    if (log_cb != nullptr) {
        log_cb(RETRO_LOG_INFO, "Azahar Splitscreen: %d seats on room port %u\n", players, port);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

/// A fresh slot has no Mii, so a game such as MK7 opens a "there are currently
/// no Mii characters in Mii Maker" dialog on the touch screen and waits for A.
/// With three or more seats that screen is behind the peek, so the tile looks
/// frozen on the splash forever; press A once, briefly, once the seat is drawing.
void boot_nudge(Seat& seat) {
    if (seat.nudge_done || seat.last_seq == 0) {
        return;
    }
    const double now = now_seconds();
    if (seat.nudge_press_at == 0) {
        seat.nudge_press_at = now + 4.0; // let the game put the dialog up first
        return;
    }
    if (seat.nudge_pressed_at == 0) {
        if (now < seat.nudge_press_at) {
            return;
        }
        send_input(seat, 1, key_A, 0, 0);
        seat.nudge_pressed_at = now;
        return;
    }
    if (now - seat.nudge_pressed_at >= 0.5) { // a few frames, not one tick
        send_input(seat, 2, key_A, 0, 0);
        seat.nudge_done = true;
    }
}

void pad_buttons(Seat& seat, int port) {
    uint32_t held = 0;
    for (int i = 0; i < static_cast<int>(sizeof(kPadKeys) / sizeof(kPadKeys[0])); ++i) {
        if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, kPadKeys[i].id) == 0) {
            continue;
        }
        held |= 1u << i;
        if ((seat.held & (1u << i)) == 0) {
            send_input(seat, 1, kPadKeys[i].key, 0, 0);
        }
    }
    for (int i = 0; i < static_cast<int>(sizeof(kPadKeys) / sizeof(kPadKeys[0])); ++i) {
        if ((seat.held & (1u << i)) != 0 && (held & (1u << i)) == 0) {
            send_input(seat, 2, kPadKeys[i].key, 0, 0);
        }
    }
    seat.held = held;
    seat.peek = input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, kPeekButton) != 0;

    const int16_t stick_x = static_cast<int16_t>(input_state_cb(
        port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X));
    const int16_t stick_y = static_cast<int16_t>(-input_state_cb(
        port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y));
    static std::vector<int16_t> last_x(kMaxPlayers, 0);
    static std::vector<int16_t> last_y(kMaxPlayers, 0);
    if (std::abs(stick_x - last_x[port]) >= 512 || std::abs(stick_y - last_y[port]) >= 512) {
        last_x[port] = stick_x;
        last_y[port] = stick_y;
        send_input(seat, 6, 0, static_cast<uint16_t>(stick_x), static_cast<uint16_t>(stick_y));
    }

    // Right stick walks the touch pointer, R3 taps it: the same two gestures the
    // session window offers, mapped onto RetroPad.
    const int16_t walk_x = static_cast<int16_t>(input_state_cb(
        port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X));
    const int16_t walk_y = static_cast<int16_t>(input_state_cb(
        port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y));
    if (seat.pointer_x < 0) {
        seat.pointer_x = 200;
        seat.pointer_y = 360;
    }
    const float dt = 1.0f / 60.0f;
    seat.pointer_x = std::clamp(seat.pointer_x + (walk_x / 32767.0f) * 320.0f * dt, 0.0f, 400.0f);
    seat.pointer_y = std::clamp(seat.pointer_y + (walk_y / 32767.0f) * 320.0f * dt, 240.0f, 480.0f);
    static bool tap_held[kMaxPlayers] = {};
    const bool tap = input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, kTapButton) != 0;
    const int pointer_x = static_cast<int>(seat.pointer_x);
    const int pointer_y = static_cast<int>(seat.pointer_y);
    if (pointer_x != seat.sent_x || pointer_y != seat.sent_y || tap != tap_held[port]) {
        seat.sent_x = pointer_x;
        seat.sent_y = pointer_y;
        tap_held[port] = tap;
        // 3 is touch down, 5 is touch up: a fresh pair at the new spot is a drag.
        send_input(seat, 3, 0, static_cast<uint16_t>(pointer_x), static_cast<uint16_t>(pointer_y));
        send_input(seat, 5, 0, static_cast<uint16_t>(pointer_x), static_cast<uint16_t>(pointer_y));
    }
}

// ---------------------------------------------------------------------------
// Compositing
// ---------------------------------------------------------------------------

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

Rect tile_rect(int index, int count) {
    const int columns = count >= 5 ? 3 : (count >= 3 ? 2 : count);
    const int rows = (count + columns - 1) / columns;
    const int cell_width = static_cast<int>(kWidth) / columns;
    const int cell_height = static_cast<int>(kHeight) / rows;
    return {(index % columns) * cell_width, (index / columns) * cell_height, cell_width,
            cell_height};
}

/// What part of a 400x480 seat frame a tile shows: both screens for two players,
/// the action screen for three or more, with the touch screen while peeking.
Rect source_rect(const Seat& seat, bool peek, bool both) {
    const int width = static_cast<int>(seat.width);
    const int height = static_cast<int>(std::max(seat.height, 1u));
    if (both) {
        return {0, 0, width, height};
    }
    const int half = std::max(height / 2, 1);
    return peek ? Rect{0, height - half, width, half} : Rect{0, 0, width, height - half};
}

const uint32_t kSeatColors[] = {0xF14B4B, 0x4B8BF1, 0x4BC96A, 0xF1C24B, 0xB04BF1, 0x4BE1E1};

void draw_tile_label(const Rect& tile, int index) {
    const uint32_t color = kSeatColors[index % 6];
    fill_rect(tile.x, tile.y, tile.w, 3, color);
    draw_text(tile.x + 8, tile.y + 10, 3, color, "P" + std::to_string(index + 1));
}

/// Word-wrapped, centred, upper-case text inside a tile. A word longer than the
/// tile (a file name) is split rather than clipped.
void draw_wrapped_text(const Rect& area, const std::string& text, uint32_t color, int scale) {
    const int limit = std::max(area.w - 40, 6 * scale);
    const int per_line = std::max(1, limit / (6 * scale));
    std::vector<std::string> words;
    std::string word;
    for (char ch : text) {
        if (ch == ' ' || ch == '\n') {
            if (!word.empty()) {
                words.push_back(word);
                word.clear();
            }
            continue;
        }
        word += static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    }
    if (!word.empty()) {
        words.push_back(word);
    }

    std::vector<std::string> lines;
    std::string line;
    for (const auto& each : words) {
        for (size_t start = 0; start < each.size(); start += per_line) {
            const std::string piece = each.substr(start, per_line);
            const bool continues = start + per_line < each.size();
            if (!line.empty() && text_width(line + " " + piece, scale) > limit) {
                lines.push_back(line);
                line = piece;
            } else if (line.empty()) {
                line = piece;
            } else {
                line += " " + piece;
            }
            if (continues) {
                lines.push_back(line);
                line.clear();
            }
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }

    const int line_height = 9 * scale;
    int y = area.y + (area.h - static_cast<int>(lines.size()) * line_height) / 2;
    for (const auto& each : lines) {
        const int x = area.x + (area.w - text_width(each, scale)) / 2;
        draw_text(x, y, scale, color, each);
        y += line_height;
    }
}

/// What a seat shows until its first frame: why it is not there, or that it is
/// still booting.
void draw_placeholder(const Seat& seat, int index, int count) {
    const Rect tile = tile_rect(index, count);
    fill_rect(tile.x, tile.y, tile.w, tile.h, 0x00181A20u);
    draw_tile_label(tile, index);
    const bool failed = !seat.failure.empty();
    Rect area = tile;
    area.y += 40;
    area.h -= 40;
    draw_wrapped_text(area, failed ? seat.failure : std::string("BOOTING"),
                      failed ? 0x00F1A03Au : 0x008B93A7u, 2);
}

/// Read a seat's own logs until its first frame appears, so a dump that cannot
/// boot says so on the TV instead of leaving a black tile.
void probe_seat(Seat& seat, int index) {
    if (!seat.failure.empty() || seat.last_seq != 0) {
        return;
    }
    const double now = now_seconds();
    if (now - seat.last_probe < 0.5) {
        return;
    }
    seat.last_probe = now;

    auto fail = [&seat](std::string reason) {
        seat.failure = std::move(reason);
        if (log_cb != nullptr) {
            // The tile carries it too; the log is for bug reports and terminals.
            log_cb(RETRO_LOG_ERROR, "Azahar Splitscreen: %s\n", seat.failure.c_str());
        }
    };

    const std::string text =
        read_tail(seat.log_path) + read_tail(seat.user_dir + "/log/azahar_log.txt");
    if (text.find("Encrypted applications are not supported") != std::string::npos ||
        text.find("Failed to determine system mode") != std::string::npos) {
        fail("THIS GAME DID NOT BOOT: STILL ENCRYPTED OR UNREADABLE. USE A DECRYPTED DUMP OR ADD "
             "THE "
             "CONSOLE KEYS.");
        return;
    }
    if (text.find("Failed to obtain loader") != std::string::npos) {
        fail("THIS FILE IS NOT A GAME AZAHAR CAN READ. PICK A .3DS OR .CCI DUMP.");
        return;
    }
    if (text.find("GBA Virtual Console is not supported") != std::string::npos) {
        fail("GBA VIRTUAL CONSOLE TITLES ARE NOT SUPPORTED BY AZAHAR.");
        return;
    }
    if (seat.pid > 0) {
        bool exited = false;
        int code = 0;
#ifdef _WIN32
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                     static_cast<DWORD>(seat.pid));
        if (process != nullptr) {
            DWORD status = STILL_ACTIVE;
            exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0 &&
                     GetExitCodeProcess(process, &status) != 0;
            code = static_cast<int>(status);
            CloseHandle(process);
        }
#else
        int status = 0;
        exited = waitpid(seat.pid, &status, WNOHANG) == seat.pid;
        code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
        if (exited) {
            seat.pid = -1; // reaped: nothing left to signal
            fail("PLAYER " + std::to_string(index + 1) + " EXITED (CODE " + std::to_string(code) +
                 ") BEFORE ITS FIRST FRAME. SEE " + seat.log_label + ".");
            return;
        }
        if (now - seat.started > 30.0) {
            fail("NO PICTURE AFTER 30 SECONDS. SEE " + seat.log_label + ".");
        }
    }
}

bool grab_frame(Seat& seat) {
    if (seat.base == nullptr) {
        return false;
    }
    auto* header = reinterpret_cast<RingHeader*>(seat.base);
    const uint64_t seq = __atomic_load_n(&header->seq, __ATOMIC_ACQUIRE);
    if (seq == 0 || seq == seat.last_seq) {
        return false;
    }
    const uint64_t after = __atomic_load_n(&header->seq, __ATOMIC_ACQUIRE);
    if (after != seq) {
        return false; // writer moved on mid-read: skip rather than tear
    }
    seat.last_seq = seq;
    return true;
}

void blit_seat(const Seat& seat, int index, int count, bool peek, bool both) {
    if (seat.base == nullptr || seat.width == 0 || seat.height == 0) {
        return;
    }
    const uint64_t seq = seat.last_seq;
    if (seq == 0) {
        return;
    }
    const uint32_t slot = static_cast<uint32_t>((seq - 1) % seat.slot_count);
    const uint8_t* source = seat.base + kHeaderBytes + static_cast<size_t>(slot) * seat.slot_bytes;
    const Rect tile = tile_rect(index, count);
    const Rect part = source_rect(seat, peek, both);

    // The ring is BGRA, bottom-up. Keep the tile's aspect ratio.
    const float scale_x = static_cast<float>(tile.w) / part.w;
    const float scale_y = static_cast<float>(tile.h) / part.h;
    const float scale = std::min(scale_x, scale_y);
    const int draw_w = std::max(1, static_cast<int>(part.w * scale));
    const int draw_h = std::max(1, static_cast<int>(part.h * scale));
    const int offset_x = tile.x + (tile.w - draw_w) / 2;
    const int offset_y = tile.y + (tile.h - draw_h) / 2;

    for (int row = 0; row < draw_h; ++row) {
        const int source_row = part.y + std::min(part.h - 1, static_cast<int>(row / scale_y));
        const uint8_t* line =
            source + static_cast<size_t>(seat.height - 1 - source_row) * seat.stride;
        uint32_t* out = frame.data() + static_cast<size_t>(offset_y + row) * kWidth + offset_x;
        for (int col = 0; col < draw_w; ++col) {
            const int source_col = std::min(part.w - 1, static_cast<int>(col / scale_x));
            const uint8_t* pixel = line + static_cast<size_t>(source_col) * 4;
            out[col] = (static_cast<uint32_t>(pixel[2]) << 16) |
                       (static_cast<uint32_t>(pixel[1]) << 8) | static_cast<uint32_t>(pixel[0]);
        }
    }
    // The tile's own colour bar, so the couch can tell the seats apart.
    draw_tile_label(tile, index);
}

void composite() {
    std::fill(frame.begin(), frame.end(), 0x00101012u);
    const int count = static_cast<int>(seats.size());
    const bool both = count <= 2;
    for (int index = 0; index < count; ++index) {
        Seat& seat = seats[index];
        if (seat.base == nullptr) {
            open_ring(seat);
        }
        grab_frame(seat);
        if (seat.last_seq == 0) {
            probe_seat(seat, index);
            draw_placeholder(seat, index, count);
            continue;
        }
        blit_seat(seat, index, count, seat.peek, both);
    }
}

/// Player 1 owns the speakers. Every seat renders the same game, so summing the
/// rings stacked N copies of the same music a few milliseconds apart: a phased,
/// hollow roar that only got worse per player. Seat 1's ring goes to RetroArch
/// alone, at full level; the other rings are left unread and their producers
/// overwrite whatever does not survive.
void mix_audio() {
    if (audio_batch_cb == nullptr || seats.empty()) {
        return;
    }
    static std::vector<int32_t> mix(kAudioFrames * 2);
    static std::vector<int16_t> out(kAudioFrames * 2);
    static double carry = 0.0;
    const double exact = static_cast<double>(kAudioRate) / 60.0;
    const double pending = carry + exact;
    const size_t want = std::min<size_t>(static_cast<size_t>(pending), kAudioFrames);
    carry = pending - static_cast<double>(want);

    std::fill(mix.begin(), mix.end(), 0);
    const size_t got =
        open_audio_ring(seats.front()) ? pull_audio(seats.front(), mix.data(), want, 1.0f) : 0;
    // One seat cannot mask another's short frame, so hold the last sample
    // instead of dropping to silence: a click is worse than a repeated sample.
    static int32_t last_left = 0;
    static int32_t last_right = 0;
    if (got > 0) {
        last_left = mix[(got - 1) * 2 + 0];
        last_right = mix[(got - 1) * 2 + 1];
    }
    for (size_t i = got; i < want; ++i) {
        mix[i * 2 + 0] = last_left;
        mix[i * 2 + 1] = last_right;
    }
    // Silence before the first seat boots: a steady clock, not a stutter.
    for (size_t i = 0; i < want * 2; ++i) {
        out[i] = static_cast<int16_t>(std::clamp(mix[i], -32768, 32767));
    }
    if (want > 0) {
        audio_batch_cb(out.data(), want);
    }
}

void draw_menu() {
    std::fill(frame.begin(), frame.end(), 0x00121418u);
    draw_centered(150, 6, 0x00E6E8EE, "AZAHAR SPLITSCREEN");
    draw_centered(260, 3, 0x008B93A7, "ONE ROM, ONE CONSOLE PER PLAYER");
    draw_centered(360, 5, 0x00FFFFFF, "PLAYERS  < " + std::to_string(menu_players) + " >");
    draw_centered(470, 3, 0x008B93A7, "D-PAD OR STICK: CHANGE   START: PLAY");
    draw_centered(560, 3, 0x008B93A7,
                  menu_players <= 2 ? "BOTH SCREENS PER PLAYER"
                                    : "TOP SCREEN PER TILE, HOLD L FOR TOUCH SCREEN");
}

void draw_failure() {
    std::fill(frame.begin(), frame.end(), 0x00121418u);
    draw_centered(200, 4, 0x00F14B4B, "COULD NOT START THE SESSION");
    int y = 300;
    std::string line;
    for (char ch : failure) {
        if (ch == '\n' || text_width(line, 2) > static_cast<int>(kWidth) - 120) {
            draw_centered(y, 2, 0x00E6E8EE, line);
            y += 26;
            line.clear();
            if (ch == '\n') {
                continue;
            }
        }
        line += static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    }
    if (!line.empty()) {
        draw_centered(y, 2, 0x00E6E8EE, line);
    }
    draw_centered(y + 60, 2, 0x008B93A7, "SEE RETROARCH'S LOG FOR THE SEAT LOGS");
}

// ---------------------------------------------------------------------------
// Menu input
// ---------------------------------------------------------------------------

void menu_input() {
    const bool left = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT) != 0;
    const bool right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT) != 0;
    const bool start = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START) != 0;
    static bool held_left = false;
    static bool held_right = false;
    static bool held_start = false;

    if (left && !held_left) {
        menu_players = menu_players <= 2 ? static_cast<int>(kMaxPlayers) : menu_players - 1;
    }
    if (right && !held_right) {
        menu_players = menu_players >= static_cast<int>(kMaxPlayers) ? 2 : menu_players + 1;
    }
    held_left = left;
    held_right = right;

    if (start && !held_start) {
        if (start_session(menu_players)) {
            in_menu = false;
            failure.clear();
        } else {
            in_menu = false; // show the failure; unload/reload retries
        }
    }
    held_start = start;
}

} // namespace

// ---------------------------------------------------------------------------
// libretro entry points
// ---------------------------------------------------------------------------

unsigned retro_api_version() {
    return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    retro_log_callback logging{};
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
        log_cb = logging.log;
    }

    unsigned format = RETRO_PIXEL_FORMAT_XRGB8888;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);

    static const retro_core_option_definition options[] = {
        {"azahar_splitscreen_players",
         "Players per ROM",
         "How many consoles the boot menu offers when a game starts. The menu can still change "
         "it before pressing Start.",
         {{"2", "2 players"},
          {"3", "3 players"},
          {"4", "4 players"},
          {"5", "5 players"},
          {"6", "6 players"},
          {nullptr, nullptr}},
         "2"},
        {},
    };
    cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, const_cast<retro_core_option_definition*>(options));
}

void retro_set_video_refresh(retro_video_refresh_t cb) {
    video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t) {}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb) {
    input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb) {
    input_state_cb = cb;
}

void retro_init() {
    frame.assign(static_cast<size_t>(kWidth) * kHeight, 0x00101012u);
    retro_log_callback logging{};
    if (environ_cb != nullptr && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
        log_cb = logging.log;
    }
}

void retro_deinit() {
    stop_session();
    frame.clear();
}

void retro_get_system_info(retro_system_info* info) {
    info->library_name = "Azahar Splitscreen";
    info->library_version = "0.1.0";
    info->need_fullpath = true;
    info->valid_extensions = "3ds|cci|cxi|app";
}

void retro_get_system_av_info(retro_system_av_info* info) {
    info->geometry.base_width = kWidth;
    info->geometry.base_height = kHeight;
    info->geometry.max_width = kWidth;
    info->geometry.max_height = kHeight;
    info->geometry.aspect_ratio = static_cast<float>(kWidth) / kHeight;
    info->timing.fps = 60.0;
    info->timing.sample_rate = kAudioRate;
}

void retro_set_controller_port_device(unsigned, unsigned) {}

void retro_reset() {}

void retro_run() {
    input_poll_cb();
    if (in_menu) {
        menu_input();
        if (in_menu) {
            draw_menu();
        } else if (!failure.empty()) {
            draw_failure();
        }
        video_cb(frame.data(), kWidth, kHeight, kWidth * sizeof(uint32_t));
        return;
    }
    if (!failure.empty()) {
        draw_failure();
        video_cb(frame.data(), kWidth, kHeight, kWidth * sizeof(uint32_t));
        return;
    }
    for (int port = 0; port < static_cast<int>(seats.size()); ++port) {
        boot_nudge(seats[port]);
        pad_buttons(seats[port], port);
    }
    composite();
    video_cb(frame.data(), kWidth, kHeight, kWidth * sizeof(uint32_t));
    mix_audio();
}

size_t retro_serialize_size() {
    return 0;
}

bool retro_serialize(void*, size_t) {
    return false;
}

bool retro_unserialize(const void*, size_t) {
    return false;
}

void retro_cheat_reset() {}

void retro_cheat_set(unsigned, bool, const char*) {}

bool retro_load_game(const retro_game_info* info) {
    if (info == nullptr) {
        return false;
    }
    if (info->path != nullptr) {
        rom_path = info->path;
    } else if (info->data != nullptr && info->size > 0) {
        // RetroArch normally hands over a path; a frontend that only supplies
        // bytes gets them written beside the other session state.
        rom_path = save_dir() + "/azahar-splitscreen.3ds";
        make_dir(save_dir());
        FILE* file = fopen(rom_path.c_str(), "wb");
        if (file == nullptr) {
            return false;
        }
        fwrite(info->data, 1, info->size, file);
        fclose(file);
    } else {
        return false;
    }

    retro_variable variable{};
    variable.key = "azahar_splitscreen_players";
    if (environ_cb != nullptr && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) &&
        variable.value != nullptr) {
        menu_players = std::clamp(atoi(variable.value), 2, static_cast<int>(kMaxPlayers));
    }
    in_menu = true;
    failure.clear();
    return true;
}

bool retro_load_game_special(unsigned, const retro_game_info*, size_t) {
    return false;
}

void retro_unload_game() {
    stop_session();
    in_menu = true;
    rom_path.clear();
}

unsigned retro_get_region() {
    return RETRO_REGION_NTSC;
}

void* retro_get_memory_data(unsigned) {
    return nullptr;
}

size_t retro_get_memory_size(unsigned) {
    return 0;
}
