#include "pcies2mm_device.h"

#include <windows.h>
#include <setupapi.h>

#include <cstdint>

#pragma comment(lib, "SetupAPI.lib")

namespace
{
    // Device interface exported by pcie_s2mm_driver.
    const GUID kPcieS2mmDeviceInterfaceGuid =
        {0x8c47b9c3, 0x1faa, 0x4557, {0xbc, 0x1d, 0xf2, 0x25, 0xd2, 0x6c, 0x9e, 0x91}};

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
    std::vector<PcieS2mmDevice> enumerate_pcies2mm_devices()
    {
        std::vector<PcieS2mmDevice> devices;
        HDEVINFO info = SetupDiGetClassDevsW(&kPcieS2mmDeviceInterfaceGuid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == INVALID_HANDLE_VALUE)
            return devices;

        SP_DEVICE_INTERFACE_DATA iface = {};
        iface.cbSize = sizeof(iface);
        for (DWORD index = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &kPcieS2mmDeviceInterfaceGuid, index, &iface); ++index)
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

            PcieS2mmDevice device;
            device.interface_path = detail->DevicePath;
            device.friendly_name = device_property(info, deviceInfo, SPDRP_FRIENDLYNAME);
            if (device.friendly_name.empty())
                device.friendly_name = device_property(info, deviceInfo, SPDRP_DEVICEDESC);
            if (device.friendly_name.empty())
                device.friendly_name = L"PcieS2mm Capture Device " + std::to_wstring(devices.size());
            devices.push_back(device);
        }

        SetupDiDestroyDeviceInfoList(info);
        return devices;
    }
}
