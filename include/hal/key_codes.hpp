#pragma once

// The codes k32_key_read returns on every board with a keyboard: ASCII for
// the printable keys, and these for the others. The arrow keys answer as the
// letters that name them.

namespace kube32::hal {

constexpr char KEY_CODE_BACKSPACE = '\b';
constexpr char KEY_CODE_TAB = '\t';
constexpr char KEY_CODE_ENTER = '\n';
constexpr char KEY_CODE_ESCAPE = 27;
constexpr char KEY_CODE_UP = 'U';
constexpr char KEY_CODE_DOWN = 'D';
constexpr char KEY_CODE_LEFT = 'L';
constexpr char KEY_CODE_RIGHT = 'R';

// What Shift makes of `c` on a US keyboard ('a' -> 'A', '1' -> '!',
// ';' -> ':'); `c` itself when Shift leaves it as it is
constexpr char shifted_key(char c) {
    if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
    constexpr char kPlain[] = "`1234567890-=[]\\;',./";
    constexpr char kShifted[] = "~!@#$%^&*()_+{}|:\"<>?";
    for (int i = 0; kPlain[i]; ++i) {
        if (kPlain[i] == c) return kShifted[i];
    }
    return c;
}

}  // namespace kube32::hal
