// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstdint>

int main()
{
    constexpr std::array<std::uint8_t, 8> payload{
        0x00, 0x41, 0x0a, 0x42, 0x0d, 0x0a, 0x43, 0xff
    };
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE) return 2;
    DWORD written = 0;
    if (::WriteFile(output,
                    payload.data(),
                    static_cast<DWORD>(payload.size()),
                    &written,
                    nullptr) == 0) {
        return 3;
    }
    return written == static_cast<DWORD>(payload.size()) ? 0 : 4;
}
