#include "core/device/usbio.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace hx {

namespace {

constexpr ULONG kReadTimeoutMs = 3000;   // the unit sends keepalives every second, so reads rarely expire
constexpr ULONG kWriteTimeoutMs = 2000;
constexpr std::size_t kPacket = 512;

std::string win32Message(DWORD e) {
    char *text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   e, 0, reinterpret_cast<char *>(&text), 0, nullptr);
    std::string s = text ? text : "";
    if (text) LocalFree(text);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s + " (" + std::to_string(e) + ")";
}

void say(std::string *error, std::string s) {
    if (error) *error = std::move(s);
}

// REG_MULTI_SZ / SPDRP_HARDWAREID lists: strings back to back, ended by an empty one.
std::vector<std::wstring> multiSz(const std::vector<wchar_t> &buf) {
    std::vector<std::wstring> out;
    for (std::size_t i = 0; i < buf.size() && buf[i];) {
        const std::wstring s(&buf[i]);
        out.push_back(s);
        i += s.size() + 1;
    }
    return out;
}

bool sameText(const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool hasHardwareId(HDEVINFO set, SP_DEVINFO_DATA &dev, const std::wstring &want) {
    DWORD type = 0, size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, &type, nullptr, 0, &size);
    if (size == 0) return false;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, &type, reinterpret_cast<PBYTE>(buf.data()),
                                           size, nullptr))
        return false;
    for (const std::wstring &id : multiSz(buf))
        if (sameText(id, want)) return true;
    return false;
}

int hexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
bool parseGuid(const std::wstring &s, GUID &g) {
    if (s.size() != 38 || s.front() != L'{' || s.back() != L'}') return false;
    std::uint8_t b[16];
    std::size_t k = 0;
    for (std::size_t i = 1; i < 37 && k < 16;) {
        if (s[i] == L'-') {
            ++i;
            continue;
        }
        const int hi = hexDigit(s[i]), lo = hexDigit(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        b[k++] = std::uint8_t(hi << 4 | lo);
        i += 2;
    }
    if (k != 16) return false;
    g.Data1 = DWORD(b[0]) << 24 | DWORD(b[1]) << 16 | DWORD(b[2]) << 8 | DWORD(b[3]);
    g.Data2 = std::uint16_t(b[4] << 8 | b[5]);
    g.Data3 = std::uint16_t(b[6] << 8 | b[7]);
    for (int i = 0; i < 8; ++i) g.Data4[i] = b[8 + i];
    return true;
}

// The interface GUIDs the driver INF registered for this device ("DeviceInterfaceGUIDs").
std::vector<GUID> interfaceGuids(HDEVINFO set, SP_DEVINFO_DATA &dev) {
    std::vector<GUID> out;
    HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) return out;
    for (const wchar_t *name : {L"DeviceInterfaceGUIDs", L"DeviceInterfaceGUID"}) {
        DWORD type = 0, size = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || size == 0) continue;
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &size) != ERROR_SUCCESS)
            continue;
        for (const std::wstring &s : multiSz(buf)) {
            GUID g;
            if (parseGuid(s, g)) out.push_back(g);
        }
    }
    RegCloseKey(key);
    return out;
}

// Device path of the first present interface of class `guid` (one per plugged-in unit).
std::wstring interfacePath(const GUID &guid) {
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return {};
    std::wstring path;
    SP_DEVICE_INTERFACE_DATA itf{};
    itf.cbSize = sizeof itf;
    for (DWORD i = 0; path.empty() && SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &itf); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &itf, nullptr, 0, &size, nullptr);
        if (size == 0) continue;
        std::vector<std::uint8_t> buf(size, 0);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &itf, detail, size, nullptr, nullptr)) path = detail->DevicePath;
    }
    SetupDiDestroyDeviceInfoList(set);
    return path;
}

class WinUsbIo : public LinkIo {
public:
    ~WinUsbIo() override { shut(); }

    bool open(const std::wstring &path, std::string *error) {
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION)
                say(error, "the Helix is in use by another program (HX Edit or the Line 6 Updater?): close it and try "
                           "again");
            else
                say(error, "could not open the Helix: " + win32Message(e));
            return false;
        }
        if (!WinUsb_Initialize(file_, &usb_)) {
            say(error, "WinUSB could not start on the Helix: " + win32Message(GetLastError()));
            usb_ = nullptr;
            return false;
        }
        // Update mode uses interrupt 0x01 / 0x81; take those, else the first non-isochronous OUT + IN pair.
        USB_INTERFACE_DESCRIPTOR d{};
        if (!WinUsb_QueryInterfaceSettings(usb_, 0, &d)) {
            say(error, "could not read the Helix's interface: " + win32Message(GetLastError()));
            return false;
        }
        UCHAR firstIn = 0, firstOut = 0;
        std::string pipes;
        for (UCHAR i = 0; i < d.bNumEndpoints; ++i) {
            WINUSB_PIPE_INFORMATION p{};
            if (!WinUsb_QueryPipe(usb_, 0, i, &p)) continue;
            static const char *types[] = {"control", "iso", "bulk", "interrupt"};
            char buf[64];
            std::snprintf(buf, sizeof buf, " %#04x %s %u", unsigned(p.PipeId), types[p.PipeType & 3],
                          unsigned(p.MaximumPacketSize));
            pipes += buf;
            if (p.PipeType != UsbdPipeTypeBulk && p.PipeType != UsbdPipeTypeInterrupt) continue;
            const bool in = (p.PipeId & 0x80) != 0;
            if (in && !firstIn) firstIn = p.PipeId;
            if (!in && !firstOut) firstOut = p.PipeId;
            if (p.PipeId == 0x81) in_ = p.PipeId;
            if (p.PipeId == 0x01) out_ = p.PipeId;
        }
        if (!in_ || !out_) {
            in_ = firstIn;
            out_ = firstOut;
        }
        if (!in_ || !out_) {
            say(error, "the Helix's interface 0 has no message endpoints");
            return false;
        }
        char use[48];
        std::snprintf(use, sizeof use, "; using OUT %#04x, IN %#04x", unsigned(out_), unsigned(in_));
        description_ = "interface " + std::to_string(d.bInterfaceNumber) + ", endpoints:" + pipes + use;
        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!readEvent_ || !writeEvent_) {
            say(error, "could not create USB events: " + win32Message(GetLastError()));
            return false;
        }
        // Clear-halt both pipes: resets the data toggles on both sides. After another host (WSL / usbipd, the
        // Updater) used the endpoints, a toggle mismatch makes the unit drop our first message silently.
        WinUsb_ResetPipe(usb_, out_);
        WinUsb_ResetPipe(usb_, in_);
        WinUsb_FlushPipe(usb_, in_);
        ULONG t = kReadTimeoutMs;
        WinUsb_SetPipePolicy(usb_, in_, PIPE_TRANSFER_TIMEOUT, sizeof t, &t);
        t = kWriteTimeoutMs;
        WinUsb_SetPipePolicy(usb_, out_, PIPE_TRANSFER_TIMEOUT, sizeof t, &t);
        running_ = true;
        // One thread reads nonstop with a long timeout: cancelling short reads lost a unit message in the
        // first Python flash attempt.
        thread_ = std::thread([this] { readLoop(); });
        return true;
    }

    // Overlapped I/O for both directions: WinUSB serializes synchronous calls on one interface handle, so a
    // blocking read held every write back until it timed out (the first Windows build never got a connect
    // reply). The pipe timeouts still apply to overlapped transfers.
    void write(Bytes message) override {
        OVERLAPPED ov{};
        ov.hEvent = writeEvent_;
        ResetEvent(writeEvent_);
        ULONG sent = 0;
        if (!WinUsb_WritePipe(usb_, out_, const_cast<PUCHAR>(message.data()), ULONG(message.size()), nullptr, &ov) &&
            GetLastError() != ERROR_IO_PENDING)
            throw LinkError("USB write failed: " + win32Message(GetLastError()));
        if (!WinUsb_GetOverlappedResult(usb_, &ov, &sent, TRUE))
            throw LinkError("USB write failed: " + win32Message(GetLastError()));
        if (sent != message.size()) throw LinkError("USB write was cut short");
    }

    std::string description() const override { return description_; }

    std::optional<std::vector<std::uint8_t>> read(double timeoutS) override {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::duration<double>(std::max(0.0, timeoutS)),
                     [this] { return !inbox_.empty() || !error_.empty(); });
        if (!inbox_.empty()) {
            std::vector<std::uint8_t> m = std::move(inbox_.front());
            inbox_.pop_front();
            return m;
        }
        if (!error_.empty()) throw LinkError("USB read failed: " + error_);
        return std::nullopt;
    }

private:
    void readLoop() {
        std::vector<std::uint8_t> buf(kPacket);
        while (running_) {
            ULONG n = 0;
            OVERLAPPED ov{};
            ov.hEvent = readEvent_;
            ResetEvent(readEvent_);
            bool ok = WinUsb_ReadPipe(usb_, in_, buf.data(), ULONG(buf.size()), nullptr, &ov);
            if (ok || GetLastError() == ERROR_IO_PENDING) ok = WinUsb_GetOverlappedResult(usb_, &ov, &n, TRUE);
            if (ok) {
                if (n == 0) continue;
                std::lock_guard lock(mutex_);
                inbox_.emplace_back(buf.begin(), buf.begin() + n);
                cv_.notify_one();
                continue;
            }
            const DWORD e = GetLastError();
            if (e == ERROR_SEM_TIMEOUT) continue;
            if (e == ERROR_OPERATION_ABORTED && !running_) break;
            if (!running_) break;
            std::lock_guard lock(mutex_);
            error_ = win32Message(e);
            cv_.notify_one();
            return;
        }
    }

    void shut() {
        running_ = false;
        if (usb_ && in_) WinUsb_AbortPipe(usb_, in_);  // wakes the reader
        if (thread_.joinable()) thread_.join();
        if (usb_) WinUsb_Free(usb_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        if (readEvent_) CloseHandle(readEvent_);
        if (writeEvent_) CloseHandle(writeEvent_);
        readEvent_ = writeEvent_ = nullptr;
        usb_ = nullptr;
        file_ = INVALID_HANDLE_VALUE;
    }

    HANDLE file_ = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb_ = nullptr;
    UCHAR in_ = 0, out_ = 0;
    HANDLE readEvent_ = nullptr, writeEvent_ = nullptr;
    std::string description_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<std::uint8_t>> inbox_;
    std::string error_;
};

} // namespace

std::unique_ptr<LinkIo> openUsbLink(std::string *error) {

    wchar_t want[64];
    std::swprintf(want, 64, L"USB\\VID_%04X&PID_%04X&MI_00", kLine6Vid, kHelixLtPid);

    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        say(error, "could not list USB devices: " + win32Message(GetLastError()));
        return nullptr;
    }
    bool found = false;
    std::wstring path;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof dev;
    for (DWORD i = 0; path.empty() && SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        if (!hasHardwareId(set, dev, want)) continue;
        found = true;
        for (const GUID &g : interfaceGuids(set, dev)) {
            path = interfacePath(g);
            if (!path.empty()) break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!found) {
        say(error, "no Helix LT found on USB: is it connected and switched on?");
        return nullptr;
    }
    if (path.empty()) {
        say(error, "the Helix LT has no WinUSB driver on interface 0: install the Line 6 driver (it comes with HX "
                   "Edit), then reconnect the unit");
        return nullptr;
    }
    auto io = std::make_unique<WinUsbIo>();
    if (!io->open(path, error)) return nullptr;
    return io;
}

} // namespace hx

#else

namespace hx {

std::unique_ptr<LinkIo> openUsbLink(std::string *error) {
    if (error) *error = "USB access is only written for Windows so far";
    return nullptr;
}

} // namespace hx

#endif
