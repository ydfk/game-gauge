#include "disk.h"
#include "common/platform.h"
#include <winioctl.h>
#include <setupapi.h>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace gauge {
namespace {
constexpr GUID disk_interface{0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
Metric temperature(HANDLE device) {
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceTemperatureProperty; query.QueryType = PropertyStandardQuery;
    alignas(STORAGE_TEMPERATURE_DATA_DESCRIPTOR) std::array<unsigned char, 4096> buffer{};
    DWORD bytes{};
    if (!DeviceIoControl(device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(),
        static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
        const auto error = GetLastError();
        return missing(error == ERROR_ACCESS_DENIED ? State::permission : State::unsupported,
            "驱动未提供硬盘温度：" + error_text(error), "Windows 存储温度接口");
    }
    const auto offset = offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo);
    if (bytes < offset) return missing(State::error, "硬盘温度响应不完整");
    const auto data = reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buffer.data());
    if (data->Size > bytes || data->Size < offset || data->InfoCount > (data->Size - offset) / sizeof(STORAGE_TEMPERATURE_INFO))
        return missing(State::error, "硬盘温度响应长度无效");
    std::optional<double> hottest;
    for (WORD i = 0; i < data->InfoCount; ++i) {
        const auto value = data->TemperatureInfo[i].Temperature;
        // 0x8000 表示传感器未提供温度，不能当作零度。
        if (value == static_cast<SHORT>(0x8000) || value < -40 || value > 150) continue;
        if (!hottest || value > *hottest) hottest = value;
    }
    return hottest ? available(*hottest, "Windows 存储温度接口 · 最高传感器温度") : missing(State::unsupported, "硬盘没有有效温度传感器");
}
std::string disk_name(HANDLE device) {
    STORAGE_PROPERTY_QUERY query{}; query.PropertyId = StorageDeviceProperty;
    alignas(STORAGE_DEVICE_DESCRIPTOR) std::array<char, 4096> buffer{}; DWORD bytes{};
    if (!DeviceIoControl(device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(),
        static_cast<DWORD>(buffer.size()), &bytes, nullptr) || bytes < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return {};
    const auto descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    auto field = [&](DWORD offset) {
        if (!offset || offset >= bytes) return std::string{};
        const auto end = static_cast<const char*>(memchr(buffer.data() + offset, 0, bytes - offset));
        return end ? std::string(buffer.data() + offset, static_cast<size_t>(end - buffer.data() - offset)) : std::string{};
    };
    auto name = field(descriptor->VendorIdOffset) + " " + field(descriptor->ProductIdOffset);
    const auto start = name.find_first_not_of(' ');
    return start == std::string::npos ? std::string{} : name.substr(start, name.find_last_not_of(' ') - start + 1);
}
}
void sample_disks(Snapshot& snapshot) {
    const auto devices = SetupDiGetClassDevsW(&disk_interface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    snapshot.hardware.disks.clear();
    snapshot.disk_temperature = missing(State::unsupported, "未发现支持温度查询的硬盘", "Windows 存储温度接口");
    if (devices == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA item{sizeof(item)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devices, nullptr, &disk_interface, i, &item); ++i) {
        DWORD size{};
        SetupDiGetDeviceInterfaceDetailW(devices, &item, nullptr, 0, &size, nullptr);
        if (!size || size > 65536) continue;
        std::vector<unsigned char> buffer(size);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data()); detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &item, detail, size, nullptr, nullptr)) continue;
        Disk disk; disk.id = utf8(detail->DevicePath); disk.name = "硬盘 " + std::to_string(i + 1);
        UniqueHandle device(CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        if (device) {
            const auto name = disk_name(device.value); if (!name.empty()) disk.name = name;
            disk.temperature = temperature(device.value);
        } else disk.temperature = missing(GetLastError() == ERROR_ACCESS_DENIED ? State::permission : State::error,
            "无法读取硬盘：" + error_text(GetLastError()), "Windows 存储温度接口");
        if (disk.temperature.state == State::valid && disk.temperature.value &&
            (!snapshot.disk_temperature.value || *disk.temperature.value > *snapshot.disk_temperature.value))
            snapshot.disk_temperature = available(*disk.temperature.value, disk.name + " · Windows 存储温度接口");
        snapshot.hardware.disks.push_back(std::move(disk));
    }
    SetupDiDestroyDeviceInfoList(devices);
}
}
