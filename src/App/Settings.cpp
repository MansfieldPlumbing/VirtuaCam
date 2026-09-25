// =============================================================================
// Settings.cpp
// =============================================================================

#include "Settings.h"
#include "Formats.h"

#include <string>

namespace {

constexpr wchar_t kKey[] = L"Software\\VirtuaCam";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"VirtuaCam";

DWORD ReadDword(const wchar_t* name, DWORD fallback)
{
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : fallback;
}

void WriteDword(const wchar_t* name, DWORD value)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &value, sizeof(value));
}

bool IsOffered(UINT width, UINT height, UINT fps)
{
    bool size = false, rate = false;
    for (const auto& s : Formats::kOutputSizes)
        size |= s.width == width && s.height == height;
    for (UINT r : Formats::kOutputRates)
        rate |= r == fps;
    return size && rate;
}

} // namespace

Settings Settings::Load()
{
    Settings s{ ReadDword(L"OutputWidth", Formats::kDefaultOutputSize.width),
                ReadDword(L"OutputHeight", Formats::kDefaultOutputSize.height),
                ReadDword(L"OutputFps", Formats::kDefaultOutputRate) };
    if (!IsOffered(s.outputWidth, s.outputHeight, s.outputFps))
        s = { Formats::kDefaultOutputSize.width, Formats::kDefaultOutputSize.height, Formats::kDefaultOutputRate };
    return s;
}

void Settings::Save() const
{
    WriteDword(L"OutputWidth", outputWidth);
    WriteDword(L"OutputHeight", outputHeight);
    WriteDword(L"OutputFps", outputFps);
}

bool IsAutostartEnabled()
{
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostartEnabled(bool enabled)
{
    if (!enabled) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return;
    }
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)))
        return;
    const std::wstring command = L"\"" + std::wstring(path) + L"\"";
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(), (DWORD)((command.size() + 1) * sizeof(wchar_t)));
}
