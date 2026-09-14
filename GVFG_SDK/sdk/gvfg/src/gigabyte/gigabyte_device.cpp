#include "gigabyte_device.h"
#include "gvfgsdkapi.h"

#include <windows.h>
#include <setupapi.h>

#include <cstdint>

#pragma comment(lib, "SetupAPI.lib")

namespace
{
    // Device interface exported by pcie_s2mm_driver.
    const GUID kGigabyteDeviceInterfaceGuid = GUID_DEVINTERFACE_GVFG100;

    std::wstring device_property(HDEVINFO info, SP_DEVINFO_DATA &deviceInfo, DWORD property)
    {
        DWORD required = 0;
        SetupDiGetDeviceRegistryPropertyW(info, &deviceInfo, property, nullptr,
                                          nullptr, 0, &required);
        if (required < sizeof(wchar_t))
            return {};

        std::vector<uint8_t> bytes(required);
        if (!SetupDiGetDeviceRegistryPropertyW(info, &deviceInfo, property, nullptr,
                                               bytes.data(), required, nullptr))
            return {};
        return reinterpret_cast<const wchar_t *>(bytes.data());
    }
}

namespace gvfg::internal
{
    std::vector<GigabyteDevice> enumerate_gigabyte_devices()
    {
        std::vector<GigabyteDevice> devices;
        HDEVINFO info = SetupDiGetClassDevsW(&kGigabyteDeviceInterfaceGuid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == INVALID_HANDLE_VALUE)
            return devices;

        SP_DEVICE_INTERFACE_DATA iface = {};
        iface.cbSize = sizeof(iface);
        for (DWORD index = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &kGigabyteDeviceInterfaceGuid, index, &iface); ++index)
        {
            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &required, nullptr);
            if (required == 0)
                continue;

            std::vector<uint8_t> detailBytes(required);
            auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detailBytes.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA deviceInfo = {};
            deviceInfo.cbSize = sizeof(deviceInfo);
            if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, required, nullptr, &deviceInfo))
                continue;

            GigabyteDevice device;
            device.interface_path = detail->DevicePath;
            device.friendly_name = device_property(info, deviceInfo, SPDRP_FRIENDLYNAME);
            if (device.friendly_name.empty())
                device.friendly_name = device_property(info, deviceInfo, SPDRP_DEVICEDESC);
            if (device.friendly_name.empty())
                device.friendly_name = L"GVFG Capture Device " + std::to_wstring(devices.size());
            devices.push_back(device);
        }

        SetupDiDestroyDeviceInfoList(info);
        return devices;
    }
}
