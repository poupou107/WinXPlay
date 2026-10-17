#include "../common/native_services.h"
#include <setupapi.h>
#include <iphlpapi.h>
#include <sstream>
#include <winusb.h>
// Apple Mobile Device Support 2023+ binds the iPhone mux interface to WinUSB
// and no longer loads USBAAPL64.sys; keep the import self-contained so every
// link line stays unchanged.
#pragma comment(lib, "winusb.lib")
namespace cp {
namespace {
bool ncmAdapter(const char *name) {
    GUID guid = {0x4d36e972, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    HDEVINFO set = SetupDiGetClassDevsW(&guid, 0, 0, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return false;
    bool found = false;
    for (DWORD i = 0; !found; ++i) {
        SP_DEVINFO_DATA item;
        ZeroMemory(&item, sizeof(item));
        item.cbSize = sizeof(item);
        if (!SetupDiEnumDeviceInfo(set, i, &item))
            break;
        wchar_t service[80] = {0};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &item, SPDRP_SERVICE, 0, (PBYTE)service,
                                               sizeof(service), 0) ||
            _wcsicmp(service, L"UsbNcm"))
            continue;
        HKEY key = SetupDiOpenDevRegKey(set, &item, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        char id[128] = {0};
        DWORD size = sizeof(id), type = 0;
        if (RegQueryValueExA(key, "NetCfgInstanceId", 0, &type, (BYTE *)id, &size) ==
                ERROR_SUCCESS &&
            type == REG_SZ && size < sizeof(id) && _stricmp(id, name) == 0)
            found = true;
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}
// USB control transports for the Apple mux interface. Current Apple Mobile
// Device Support (AppleUSB.inf, 2023) binds the iPhone mux interface (MI_01) to
// WinUSB and registers {664be590-...} ("New iTunes WinUSB interface"); older
// stacks used USBAAPL64.sys with the private IOCTL 0x2200a0 below. The CarPlay
// preparation sequence is identical on both transports.
struct UsbControl {
    virtual ~UsbControl() {}
    virtual Bytes control(U8 request, U16 value, U16 index, unsigned length) = 0;
};
struct LegacyUsbControl : UsbControl {
    HANDLE device;
    explicit LegacyUsbControl(HANDLE h) : device(h) {}
    virtual Bytes control(U8 request, U16 value, U16 index, unsigned length) {
        Bytes packet(8 + length, 0);
        packet[0] = request == 6 ? 0x80 : 0xc0;
        packet[1] = request;
        put16(packet, 2, value, true);
        put16(packet, 4, index, true);
        put16(packet, 6, U16(length), true);
        OVERLAPPED overlap;
        ZeroMemory(&overlap, sizeof(overlap));
        overlap.hEvent = CreateEventW(0, TRUE, FALSE, 0);
        require(overlap.hEvent != 0, "USB event allocation failed");
        DWORD done = 0;
        BOOL ok = DeviceIoControl(device, 0x2200a0, &packet[0], DWORD(packet.size()), &packet[0],
                                  DWORD(packet.size()), 0, &overlap);
        DWORD error = GetLastError();
        if (!ok && error != ERROR_IO_PENDING) {
            CloseHandle(overlap.hEvent);
            throw std::runtime_error("Apple USB control request rejected");
        }
        if (WaitForSingleObject(overlap.hEvent, 3000) != WAIT_OBJECT_0) {
            CancelIo(device);
            GetOverlappedResult(device, &overlap, &done, TRUE);
            CloseHandle(overlap.hEvent);
            throw std::runtime_error("Apple USB control request timed out");
        }
        ok = GetOverlappedResult(device, &overlap, &done, FALSE);
        CloseHandle(overlap.hEvent);
        if (!ok) {
            std::ostringstream message;
            message << "Invalid Apple USB control response (transfer failed, error "
                    << unsigned(GetLastError()) << ')';
            throw std::runtime_error(message.str());
        }
        if (done < 8 || done > packet.size()) {
            std::ostringstream message;
            message << "Invalid Apple USB control response (returned " << done << " bytes)";
            throw std::runtime_error(message.str());
        }
        return slice(packet, 8, done - 8);
    }
};
struct WinUsbControl : UsbControl {
    WINUSB_INTERFACE_HANDLE usb;
    explicit WinUsbControl(WINUSB_INTERFACE_HANDLE h) : usb(h) {}
    virtual Bytes control(U8 request, U16 value, U16 index, unsigned length) {
        WINUSB_SETUP_PACKET setup;
        ZeroMemory(&setup, sizeof(setup));
        setup.RequestType = request == 6 ? 0x80 : 0xc0;
        setup.Request = request;
        setup.Value = value;
        setup.Index = index;
        setup.Length = U16(length);
        Bytes packet(length, 0);
        ULONG done = 0;
        if (!WinUsb_ControlTransfer(usb, setup, length ? &packet[0] : (PUCHAR)0, DWORD(length),
                                    &done, 0)) {
            std::ostringstream message;
            message << "Apple USB control transfer failed (WinUSB error "
                    << unsigned(GetLastError()) << ')';
            throw std::runtime_error(message.str());
        }
        packet.resize(done);
        return packet;
    }
};
const GUID kAppleMuxWinUsb = {0x664be590, 0x54bd, 0x4964,
                              {0x8a, 0x8c, 0x6c, 0xd1, 0x31, 0x4f, 0x6d, 0xc2}};
const GUID kAppleMuxLegacy = {0xf0b32be3, 0x6678, 0x4879,
                              {0x92, 0x30, 0xe4, 0x38, 0x45, 0xd8, 0x05, 0xee}};
// Returns the first present device interface path of the class, or an empty
// string when no instance is connected.
std::wstring appleInterfacePath(const GUID &guid) {
    HDEVINFO set = SetupDiGetClassDevsW(&guid, 0, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Apple USB interface enumeration failed");
    std::wstring path;
    SP_DEVICE_INTERFACE_DATA item;
    ZeroMemory(&item, sizeof(item));
    item.cbSize = sizeof(item);
    if (SetupDiEnumDeviceInterfaces(set, 0, &guid, 0, &item) != FALSE) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &item, 0, 0, &needed, 0);
        require(needed >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) && needed <= 65536,
                "Invalid Apple USB interface path");
        Bytes buffer(needed);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)&buffer[0];
        detail->cbSize = sizeof(*detail);
        require(SetupDiGetDeviceInterfaceDetailW(set, &item, detail, needed, 0, 0) != FALSE,
                "Apple USB interface lookup failed");
        path = detail->DevicePath;
    }
    SetupDiDestroyDeviceInfoList(set);
    return path;
}
// Opens an Apple USB device interface. The iPhone re-enumerates after a
// CarPlay mode switch, so a few transient open failures are retried briefly.
// The retry window is deliberately short: hammering a device that is being
// surprise-removed stresses the USB filter stack (Apple's filters, UsbDk and
// monitoring drivers are known to bugcheck on rapid re-enumeration).
HANDLE openAppleUsb(const std::wstring &path, DWORD flags, DWORD &error) {
    HANDLE device = INVALID_HANDLE_VALUE;
    error = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        device = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, flags, 0);
        if (device != INVALID_HANDLE_VALUE)
            return device;
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NOT_READY &&
            error != ERROR_GEN_FAILURE && error != ERROR_DEVICE_REMOVED)
            break;
        Sleep(250);
    }
    return INVALID_HANDLE_VALUE;
}
// Records every transport attempt so a single failure explains the whole
// matrix: which interface path, which open mode, which stage failed and the
// Win32 error code. The detail is also written next to the executable.
class TransportReport {
  public:
    void add(const char *transport, const char *mode, const char *stage, DWORD error) {
        std::ostringstream entry;
        entry << transport << ' ' << mode << ' ' << stage << " error " << error;
        entries.push_back(entry.str());
    }
    std::string details() const {
        std::ostringstream message;
        for (size_t i = 0; i < entries.size(); ++i)
            message << (i ? "; " : "") << entries[i];
        return message.str();
    }
    std::string summary() const { return "Apple USB interface unavailable: " + details(); }
    bool empty() const { return entries.empty(); }

  private:
    std::vector<std::string> entries;
};
Value carPlayUsbPreparation(UsbControl &usb) {
    Value result = Value::dictionary();
    Bytes descriptor = usb.control(6, 0x100, 0, 18);
    require(descriptor.size() == 18 && le16(descriptor, 8) == 0x05ac, "Unexpected USB device");
    bool switched = descriptor[17] < 6;
    result.dict["configuration_count"] = Value::num(descriptor[17]);
    if (switched) {
        Bytes response = usb.control(0x52, 0, 4, 1);
        require(response.size() == 1 && response[0] == 0, "iPhone rejected CarPlay USB mode");
    }
    result.dict["mode_switch_requested"] = Value::flag(switched);
    return result;
}
// Runs the CarPlay preparation over the WinUSB API on an interface path.
// WinUSB devices expect FILE_FLAG_OVERLAPPED handles (the diagnostic in
// carplay/tests/windows/usb_configuration.py uses the same flag), so both
// open variants are attempted. Returns false when the path cannot be opened
// or initialized; control transfer failures propagate to the caller.
bool prepareOverWinUsb(const std::wstring &path, const char *transport, Value &result,
                       TransportReport &report) {
    const DWORD flags[] = {0, FILE_FLAG_OVERLAPPED};
    const char *modes[] = {"sync", "overlapped"};
    for (int variant = 0; variant < 2; ++variant) {
        DWORD error = 0;
        HANDLE device = openAppleUsb(path, flags[variant], error);
        if (device == INVALID_HANDLE_VALUE) {
            report.add(transport, modes[variant], "open", error);
            continue;
        }
        WINUSB_INTERFACE_HANDLE usb = 0;
        if (WinUsb_Initialize(device, &usb) == FALSE) {
            report.add(transport, modes[variant], "init", GetLastError());
            CloseHandle(device);
            continue;
        }
        try {
            WinUsbControl control(usb);
            result = carPlayUsbPreparation(control);
        } catch (const std::exception &e) {
            WinUsb_Free(usb);
            CloseHandle(device);
            if (report.empty())
                throw;
            throw std::runtime_error(std::string(e.what()) + "; prior attempts: " +
                                     report.details());
        } catch (...) {
            WinUsb_Free(usb);
            CloseHandle(device);
            throw;
        }
        WinUsb_Free(usb);
        CloseHandle(device);
        return true;
    }
    return false;
}
} // namespace
Value prepareUsbCarPlay() {
    // A device that already exposes CarPlay USB networking (an NCM adapter
    // with an IPv6 link-local address) is already in CarPlay mode: a real
    // iPhone reaches this state after the mode switch, and wired CarPlay
    // protocol boxes present this configuration from the start. The
    // mode-switch control request needs the mux interface, which Apple's
    // current filter drivers reject from third-party processes (error 50),
    // so skip the control channel entirely when networking is already up.
    bool networkingReady = false;
    try {
        receiverAddresses();
        networkingReady = true;
    } catch (...) {
    }
    if (networkingReady) {
        Value result = Value::dictionary();
        result.dict["configuration_count"] = Value::num(0);
        result.dict["mode_switch_requested"] = Value::flag(false);
        return result;
    }
    // Current Apple software exposes the iPhone mux interface as WinUSB and
    // registers {664be590-...}; older stacks used the USBAAPL64 compatibility
    // interface {f0b32be3-...} with a private control IOCTL. Apple's filter
    // drivers reject opens of the WinUSB interface from other processes
    // (error 50), while the legacy compatibility path still opens on this
    // stack and exposes the same device object to the WinUSB API, so each
    // registered path is tried with both transports.
    Value result;
    TransportReport report;
    std::wstring winusb = appleInterfacePath(kAppleMuxWinUsb);
    if (!winusb.empty() && prepareOverWinUsb(winusb, "WinUSB", result, report))
        return result;
    std::wstring legacy = appleInterfacePath(kAppleMuxLegacy);
    if (!legacy.empty()) {
        if (prepareOverWinUsb(legacy, "legacy", result, report))
            return result;
        DWORD error = 0;
        HANDLE device = openAppleUsb(legacy, FILE_FLAG_OVERLAPPED, error);
        if (device != INVALID_HANDLE_VALUE) {
            try {
                LegacyUsbControl control(device);
                return carPlayUsbPreparation(control);
            } catch (const std::exception &e) {
                CloseHandle(device);
                if (report.empty())
                    throw;
                throw std::runtime_error(std::string(e.what()) + "; prior attempts: " +
                                         report.details());
            } catch (...) {
                CloseHandle(device);
                throw;
            }
        }
        report.add("legacy", "ioctl", "open", error);
    }
    if (winusb.empty() && legacy.empty())
        throw std::runtime_error("Connect and unlock an iPhone first");
    std::string summary = report.empty() ? "Cannot open Apple USB compatibility interface"
                                         : report.summary();
    try {
        writeDiagnosticFile(executableDirectory() + L"/usb-transport.log",
                            Bytes(summary.begin(), summary.end()));
    } catch (...) {
    }
    throw std::runtime_error(summary);
}
std::string receiverAddresses() {
    std::wstring explicitAddress = environment(L"CARPLAY_ADDRESS", L"");
    if (!explicitAddress.empty())
        return utf8(explicitAddress);
    ULONG size = 16384;
    Bytes buffer(size);
    ULONG rc = GetAdaptersAddresses(AF_INET6, 0, 0, (IP_ADAPTER_ADDRESSES *)&buffer[0], &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        require(size <= 1024 * 1024, "Network adapter list limit");
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET6, 0, 0, (IP_ADAPTER_ADDRESSES *)&buffer[0], &size);
    }
    require(rc == NO_ERROR, "Network adapter enumeration failed");
    std::string result;
    for (IP_ADAPTER_ADDRESSES *a = (IP_ADAPTER_ADDRESSES *)&buffer[0]; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !ncmAdapter(a->AdapterName))
            continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET6)
                continue;
            sockaddr_in6 address = *(sockaddr_in6 *)u->Address.lpSockaddr;
            if (!IN6_IS_ADDR_LINKLOCAL(&address.sin6_addr))
                continue;
            address.sin6_scope_id = 0;
            address.sin6_port = 0;
            char text[128];
            DWORD n = sizeof(text);
            if (WSAAddressToStringA((sockaddr *)&address, sizeof(address), 0, text, &n) == 0) {
                if (!result.empty())
                    result += ';';
                result += text;
            }
        }
    }
    require(!result.empty(), "iPhone USB networking is unavailable. Run repair-usb-ncm.ps1 -Apply "
                             "in administrator PowerShell, then reconnect.");
    return result;
}
} // namespace cp
