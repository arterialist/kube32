"""The pod ABI and the WebAssembly reader behind `kube32 check`.

The ABI it checks against is include/runtime/k32_app_abi.h; the import
signatures are the m3_LinkRawFunction calls in src/runtime/app_pod.cpp and
src/runtime/host_functions.cpp, and the opcode table follows the wasm3 build
flags in CMakeLists.txt and platformio.ini.
"""
import os

from .common import fmt_bytes, format_quantity

PAGE = 65536
MAX_REQUEST_BYTES = 16 * 1024   # K32_MAX_REQUEST_BYTES
MAX_EVENT_BYTES = 200           # K32_MAX_EVENT_BYTES
SD_IMAGE_LIMIT_NO_PSRAM = 256 * 1024     # kubelet.cpp sd_image_limit_bytes()
SD_IMAGE_LIMIT_PSRAM = 2 * 1024 * 1024

# ---------------------------------------------------------------------------
# ABI tables (signatures in wasm3 notation: result then params, i=i32 I=i64)
# ---------------------------------------------------------------------------
# src/runtime/app_pod.cpp link_app_host_functions
APP_IMPORTS = {
    "k32_resp_begin": "v(iii)",
    "k32_resp_header": "v(iiii)",
    "k32_resp_write": "i(ii)",
    "k32_resp_end": "v()",
    "k32_event_emit": "i(iiii)",
    "k32_time_ms": "I()",
    "k32_unix_time_ms": "I()",
    "k32_log": "v(ii)",
    "k32_random": "i()",
    "k32_env_get": "i(iiii)",
    "k32_mem_stats": "i(ii)",
    "k32_resp_asset": "i(iii)",
    "k32_http_call": "i(iiiiiiiiiiii)",
    "k32_http_status": "i()",
    "k32_display_text": "i(iiiiii)",
    "k32_display_rect": "i(iiiii)",
    "k32_display_bar": "i(iiiiiii)",
    "k32_key_read": "i()",
    "k32_tone": "i(iii)",
    "k32_audio_play": "i(iiii)",
    "k32_mic_read": "i(iii)",
    "k32_fs_open": "i(iii)",
    "k32_fs_read": "i(iii)",
    "k32_fs_write": "i(iii)",
    "k32_fs_close": "i(i)",
    "k32_fs_size": "i(i)",
    "k32_fs_list": "i(iiii)",
    "k32_fs_remove": "i(ii)",
    "k32_matvec_i8": "i(iiiifiii)",
    "k32_matvec_i4": "i(iiiifiii)",
    "k32_matmul_i8": "i(iiiiiiiii)",
    "k32_matmul_i4": "i(iiiiiiiii)",
    "k32_matvec_f32_i8": "i(iiiifiiii)",
    "k32_vecmat_f32_i8": "i(iiiiiiii)",
    "k32_softmax_f32": "i(ii)",
    "k32_blob_open": "i(ii)",
    "k32_blob_size": "i(i)",
    "k32_blob_read": "i(iiii)",
    "k32_blob_mapped": "i(i)",
    "k32_blob_close": "i(i)",
}
# src/runtime/host_functions.cpp link_host_functions
LOW_LEVEL_IMPORTS = {
    "esp32_gpio_set_mode": "i(ii)",
    "esp32_gpio_write": "i(ii)",
    "esp32_gpio_read": "i(i)",
    "esp32_adc_read": "i(i)",
    "esp32_chip_temp_read": "i(i)",
    "esp32_i2c_write": "i(iiii)",
    "esp32_i2c_read": "i(iiii)",
    "esp32_uart_write": "i(iii)",
    "esp32_uart_read": "i(iii)",
    "esp32_rmt_send": "i(ii)",
    "esp32_rmt_receive": "i(ii)",
    "esp32_get_time_us": "I()",
    "esp32_delay_ms": "v(i)",
    "kube32_channel_open": "i(ii)",
    "kube32_channel_send": "i(iii)",
    "kube32_channel_recv": "i(iiii)",
    "kube32_channel_close": "i(i)",
}
KNOWN_IMPORTS = dict(APP_IMPORTS)
KNOWN_IMPORTS.update(LOW_LEVEL_IMPORTS)
# GPIO/ADC/I2C/UART/RMT: granted by esp32.io/* resources, not by --hardware
PERIPHERAL_IMPORTS = ("esp32_gpio_set_mode", "esp32_gpio_write", "esp32_gpio_read", "esp32_adc_read",
                      "esp32_i2c_write", "esp32_i2c_read", "esp32_uart_write", "esp32_uart_read",
                      "esp32_rmt_send", "esp32_rmt_receive")

# Exports the host looks up (k32_app_abi.h, app_pod.cpp, container_lifecycle.cpp)
REQUEST_EXPORTS = {
    "k32_init": "i()",
    "k32_alloc": "i(i)",
    "k32_handle": "i(ii)",
    "k32_tick": "i(i)",
    "k32_event": "i(iiii)",
}
ENTRY_EXPORTS = ("_start", "app_main", "main")   # searched in this order, called with no arguments
ENTRY_SIGS = ("v()", "i()")

# Which manifest resource an import needs (capability firewall)
HARDWARE_FOR_IMPORT = {
    "k32_display_text": "display",
    "k32_display_rect": "display",
    "k32_display_bar": "display",
    "k32_key_read": "keyboard",
    "k32_tone": "speaker",
    "k32_audio_play": "speaker",
    "k32_mic_read": "microphone",
    "k32_event_emit": "ipc",
    "kube32_channel_open": "ipc",
    "kube32_channel_send": "ipc",
    "kube32_channel_recv": "ipc",
    "kube32_channel_close": "ipc",
    "k32_fs_open": "sd",
    "k32_fs_read": "sd",
    "k32_fs_write": "sd",
    "k32_fs_close": "sd",
    "k32_fs_size": "sd",
    "k32_fs_list": "sd",
    "k32_fs_remove": "sd",
    "k32_http_call": "service-call",
    "esp32_chip_temp_read": "chip-temp",
}
HARDWARE = {
    # name: (resource key, securityContext capability) as src/hal/capability_firewall.cpp reads them
    "display": ("hardware.kube32.io/tft-display", "DISPLAY"),
    "keyboard": ("hardware.kube32.io/keyboard", "KEYBOARD"),
    "speaker": ("hardware.kube32.io/speaker", "SPEAKER"),
    "microphone": ("hardware.kube32.io/microphone", "MICROPHONE"),
    "ipc": ("hardware.kube32.io/ipc", "CHANNEL_IPC"),
    "sd": ("hardware.kube32.io/sd", "SD"),
    "service-call": (None, "SERVICE_CALL"),   # k32_http_call: a capability only, no resource
    "chip-temp": ("hardware.kube32.io/chip-temp", "CHIP_TEMP"),
}
HARDWARE_ORDER = ("ipc", "display", "keyboard", "speaker", "microphone", "sd", "service-call", "chip-temp")
HARDWARE_ALIASES = {"tft-display": "display", "tft": "display", "channel_ipc": "ipc", "channels": "ipc", "sd-card": "sd", "mic": "microphone",
                    "service_call": "service-call", "http": "service-call"}


# ---------------------------------------------------------------------------
# Wasm binary reader
# ---------------------------------------------------------------------------
class WasmError(Exception):
    pass


class Reader:
    def __init__(self, data, pos=0, end=None):
        self.b = data
        self.i = pos
        self.end = len(data) if end is None else end

    def eof(self):
        return self.i >= self.end

    def byte(self):
        if self.i >= self.end:
            raise WasmError("unexpected end of section at offset %d" % self.i)
        v = self.b[self.i]
        self.i += 1
        return v

    def peek(self):
        if self.i >= self.end:
            raise WasmError("unexpected end of section at offset %d" % self.i)
        return self.b[self.i]

    def uleb(self, bits=32):
        r = s = 0
        while True:
            c = self.byte()
            r |= (c & 0x7F) << s
            s += 7
            if not c & 0x80:
                return r
            if s >= bits + 7:
                raise WasmError("LEB128 too long at offset %d" % self.i)

    def u32(self):
        return self.uleb(32)

    def s_leb(self, bits):
        r = s = 0
        while True:
            c = self.byte()
            r |= (c & 0x7F) << s
            s += 7
            if not c & 0x80:
                if s < bits and c & 0x40:
                    r -= 1 << s
                return r
            if s > bits + 7:
                raise WasmError("signed LEB128 too long at offset %d" % self.i)

    def bytes(self, n):
        if self.i + n > self.end:
            raise WasmError("unexpected end of section at offset %d" % self.i)
        v = self.b[self.i:self.i + n]
        self.i += n
        return v

    def name(self):
        n = self.u32()
        return self.bytes(n).decode("utf-8", errors="replace")


VALTYPE_LETTER = {0x7F: "i", 0x7E: "I", 0x7D: "f", 0x7C: "F", 0x7B: "v128", 0x70: "r", 0x6F: "e"}


def sig_string(params, results):
    """wasm3 notation: 'i(ii)' for (i32,i32)->i32, 'v()' for ()->()."""
    if len(results) > 1:
        res = "(" + "".join(VALTYPE_LETTER.get(t, "?") for t in results) + ")"
    elif results:
        res = VALTYPE_LETTER.get(results[0], "?")
    else:
        res = "v"
    return res + "(" + "".join(VALTYPE_LETTER.get(t, "?") for t in params) + ")"


class Module:
    def __init__(self):
        self.size = 0
        self.types = []          # [(params, results)]
        self.imports = []        # [(module, name, kind, typeidx or None)]
        self.func_types = []     # type index per defined function
        self.memories = []       # [(initial, max or None, flags)]
        self.imported_memory = False
        self.imported_table = False
        self.tables = 0
        self.globals = 0
        self.global_inits = []   # [(valtype, mutable, value of an i32.const initializer or None)] of defined globals
        self.exports = []        # [(name, kind, index)]
        self.start = None
        self.data_segments = 0
        self.custom_sections = []
        self.features = {}       # feature -> {opcode name: count}
        self.unsupported = []    # [(func index or None, description)]
        self.code_bytes = 0
        self.data_bytes = 0

    # --- lookups -----------------------------------------------------------
    def import_funcs(self):
        return [(m, n, t) for (m, n, k, t) in self.imports if k == 0]

    def func_sig(self, index):
        imported = self.import_funcs()
        if index < len(imported):
            t = imported[index][2]
        else:
            d = index - len(imported)
            if d >= len(self.func_types):
                return None
            t = self.func_types[d]
        if t >= len(self.types):
            return None
        return sig_string(*self.types[t])

    def export_map(self):
        return {name: (kind, idx) for (name, kind, idx) in self.exports}

    def exported_func_sig(self, name):
        kind_idx = self.export_map().get(name)
        if not kind_idx or kind_idx[0] != 0:
            return None
        return self.func_sig(kind_idx[1])

    def feature_note(self, feature, op):
        d = self.features.setdefault(feature, {})
        d[op] = d.get(op, 0) + 1


KIND_NAMES = {0: "func", 1: "table", 2: "memory", 3: "global", 4: "tag"}


def _read_limits(r):
    flags = r.byte()
    if flags & ~0x07:
        raise WasmError("unknown limits flag 0x%02x" % flags)
    wide = bool(flags & 0x04)
    initial = r.uleb(64 if wide else 32)
    maximum = r.uleb(64 if wide else 32) if flags & 0x01 else None
    return initial, maximum, flags


def _read_valtype_list(r):
    n = r.u32()
    return [r.byte() for _ in range(n)]


def parse_module(data):
    if len(data) < 8 or data[:4] != b"\0asm":
        raise WasmError("not a WebAssembly module (missing \\0asm magic)")
    version = int.from_bytes(data[4:8], "little")
    if version != 1:
        raise WasmError("unsupported wasm binary version %d" % version)
    m = Module()
    m.size = len(data)
    r = Reader(data, 8)
    while not r.eof():
        sid = r.byte()
        size = r.u32()
        start = r.i
        end = start + size
        if end > len(data):
            raise WasmError("section %d overruns the file" % sid)
        s = Reader(data, start, end)
        if sid == 0:
            m.custom_sections.append(s.name())
        elif sid == 1:
            for _ in range(s.u32()):
                form = s.byte()
                if form != 0x60:
                    raise WasmError("unsupported type form 0x%02x (GC types are not in this wasm3 build)" % form)
                params = _read_valtype_list(s)
                results = _read_valtype_list(s)
                if 0x7B in params or 0x7B in results:
                    m.unsupported.append((None, "the v128 value type (SIMD)"))
                if len(results) > 1:
                    m.feature_note("multi-value", "function type with %d results" % len(results))
                m.types.append((params, results))
        elif sid == 2:
            for _ in range(s.u32()):
                mod = s.name()
                nm = s.name()
                kind = s.byte()
                if kind == 0:
                    m.imports.append((mod, nm, 0, s.u32()))
                elif kind == 1:
                    s.byte()
                    _read_limits(s)
                    m.imported_table = True
                    m.imports.append((mod, nm, 1, None))
                elif kind == 2:
                    _read_limits(s)
                    m.imported_memory = True
                    m.imports.append((mod, nm, 2, None))
                elif kind == 3:
                    s.byte()
                    s.byte()
                    m.imports.append((mod, nm, 3, None))
                elif kind == 4:
                    s.byte()
                    s.u32()
                    m.imports.append((mod, nm, 4, None))
                else:
                    raise WasmError("unknown import kind %d for %s.%s" % (kind, mod, nm))
        elif sid == 3:
            m.func_types = [s.u32() for _ in range(s.u32())]
        elif sid == 4:
            n = s.u32()
            m.tables += n
            for _ in range(n):
                if s.peek() == 0x40:
                    raise WasmError("table with an initializer expression (typed function references) is not in this wasm3 build")
                s.byte()
                _read_limits(s)
        elif sid == 5:
            for _ in range(s.u32()):
                m.memories.append(_read_limits(s))
        elif sid == 6:
            m.globals = s.u32()
            for _ in range(m.globals):
                valtype = s.byte()
                if valtype == 0x7B:
                    m.unsupported.append((None, "a v128 global (SIMD)"))
                mutable = s.byte() == 1
                m.global_inits.append((valtype, mutable, _read_const_expr(m, s)))
        elif sid == 7:
            for _ in range(s.u32()):
                nm = s.name()
                kind = s.byte()
                idx = s.u32()
                m.exports.append((nm, kind, idx))
        elif sid == 8:
            m.start = s.u32()
        elif sid == 10:
            m.code_bytes = size
            n = s.u32()
            if n != len(m.func_types):
                raise WasmError("code section has %d bodies for %d functions" % (n, len(m.func_types)))
            n_imported = len(m.import_funcs())
            for fi in range(n):
                body_size = s.u32()
                body = Reader(data, s.i, s.i + body_size)
                _scan_body(m, body, n_imported + fi)
                s.i += body_size
        elif sid == 11:
            m.data_bytes = size
            n = s.u32()
            m.data_segments = n
            for _ in range(n):
                flag = s.u32()
                if flag == 1:
                    m.feature_note("bulk-memory", "passive data segment")
                else:
                    if flag == 2:
                        if s.u32() != 0:
                            m.feature_note("multi-memory", "data segment for memory > 0")
                    _read_const_expr(m, s)
                s.bytes(s.u32())
        elif sid == 13:
            m.unsupported.append((None, "a tag section (exception handling, disabled in this wasm3 build)"))
        r.i = end
    if r.i != len(data):
        raise WasmError("trailing bytes after the last section")
    return m


EXTENDED_CONST = {0x6A: "i32.add", 0x6B: "i32.sub", 0x6C: "i32.mul", 0x7C: "i64.add", 0x7D: "i64.sub", 0x7E: "i64.mul"}


def _read_const_expr(m, r):
    """Reads a constant expression; returns its value when it is a lone i32.const, else None."""
    value, ops = None, 0
    while True:
        op = r.byte()
        if op == 0x0B:
            return value if ops == 1 else None
        ops += 1
        if op == 0x41:
            value = r.s_leb(32) & 0xFFFFFFFF
        elif op == 0x42:
            r.s_leb(64)
        elif op == 0x43:
            r.bytes(4)
        elif op == 0x44:
            r.bytes(8)
        elif op == 0x23:
            r.u32()
        elif op == 0xD0:
            r.byte()
        elif op == 0xD2:
            r.u32()
        elif op in EXTENDED_CONST:
            m.feature_note("extended-const", EXTENDED_CONST[op])
        else:
            raise WasmError("unsupported opcode 0x%02x in a constant expression" % op)


# ---------------------------------------------------------------------------
# Opcode table of this wasm3 build
# ---------------------------------------------------------------------------
# third_party/wasm3/source/m3_compile.c with the flags of CMakeLists.txt and
# platformio.ini: d_m3HasExceptionHandling=0, d_m3HasTypedRefs=0,
# d_m3HasStackSwitching=0; reference types, tail calls, bulk memory,
# sign extension, saturating conversions, multi-value, extended const,
# multi-memory, memory64, wide arithmetic and atomics compiled in; no SIMD.
# Features listed are the ones beyond the MVP; floating point is MVP and
# compiled in (d_m3HasFloat=1).
_NONE, _BLOCK, _U32, _BRTABLE, _CALLIND, _SELECT_T, _MEMARG, _MEMIDX, _S32, _S64, _F32, _F64, _REFTYPE = range(13)

OPCODES = {}


def _reg(lo, hi, kind, feature=None):
    for op in range(lo, hi + 1):
        OPCODES[op] = (kind, feature)


_reg(0x00, 0x05, _NONE)
_reg(0x02, 0x04, _BLOCK)
_reg(0x0B, 0x0B, _NONE)
_reg(0x0C, 0x0D, _U32)
_reg(0x0E, 0x0E, _BRTABLE)
_reg(0x0F, 0x0F, _NONE)
_reg(0x10, 0x10, _U32)
_reg(0x11, 0x11, _CALLIND)
_reg(0x12, 0x12, _U32, "tail-call")
_reg(0x13, 0x13, _CALLIND, "tail-call")
_reg(0x1A, 0x1B, _NONE)
_reg(0x1C, 0x1C, _SELECT_T, "reference-types")
_reg(0x20, 0x24, _U32)
_reg(0x25, 0x26, _U32, "reference-types")
_reg(0x28, 0x3E, _MEMARG)
_reg(0x3F, 0x40, _MEMIDX)
_reg(0x41, 0x41, _S32)
_reg(0x42, 0x42, _S64)
_reg(0x43, 0x43, _F32)
_reg(0x44, 0x44, _F64)
_reg(0x45, 0xBF, _NONE)
_reg(0xC0, 0xC4, _NONE, "sign-ext")
_reg(0xD0, 0xD0, _REFTYPE, "reference-types")
_reg(0xD1, 0xD1, _NONE, "reference-types")
_reg(0xD2, 0xD2, _U32, "reference-types")

# Present in the wasm3 sources, compiled out of this build
DISABLED_OPCODES = {
    0x08: "throw (exception handling, d_m3HasExceptionHandling=0)",
    0x0A: "throw_ref (exception handling, d_m3HasExceptionHandling=0)",
    0x1F: "try_table (exception handling, d_m3HasExceptionHandling=0)",
    0x14: "call_ref (typed function references, d_m3HasTypedRefs=0)",
    0x15: "return_call_ref (typed function references, d_m3HasTypedRefs=0)",
    0xD4: "ref.as_non_null (typed function references, d_m3HasTypedRefs=0)",
    0xD5: "br_on_null (typed function references, d_m3HasTypedRefs=0)",
    0xD6: "br_on_non_null (typed function references, d_m3HasTypedRefs=0)",
}

OPCODE_NAMES = {
    0x12: "return_call", 0x13: "return_call_indirect",
    0x1C: "select t", 0x25: "table.get", 0x26: "table.set", 0xC0: "i32.extend8_s", 0xC1: "i32.extend16_s",
    0xC2: "i64.extend8_s", 0xC3: "i64.extend16_s", 0xC4: "i64.extend32_s", 0xD0: "ref.null",
    0xD1: "ref.is_null", 0xD2: "ref.func",
}
FC_OPS = {
    0: ("i32.trunc_sat_f32_s", "nontrapping-fptoint", []), 1: ("i32.trunc_sat_f32_u", "nontrapping-fptoint", []),
    2: ("i32.trunc_sat_f64_s", "nontrapping-fptoint", []), 3: ("i32.trunc_sat_f64_u", "nontrapping-fptoint", []),
    4: ("i64.trunc_sat_f32_s", "nontrapping-fptoint", []), 5: ("i64.trunc_sat_f32_u", "nontrapping-fptoint", []),
    6: ("i64.trunc_sat_f64_s", "nontrapping-fptoint", []), 7: ("i64.trunc_sat_f64_u", "nontrapping-fptoint", []),
    8: ("memory.init", "bulk-memory", ["u32", "u32"]), 9: ("data.drop", "bulk-memory", ["u32"]),
    10: ("memory.copy", "bulk-memory", ["u32", "u32"]), 11: ("memory.fill", "bulk-memory", ["u32"]),
    12: ("table.init", "reference-types", ["u32", "u32"]), 13: ("elem.drop", "reference-types", ["u32"]),
    14: ("table.copy", "reference-types", ["u32", "u32"]), 15: ("table.grow", "reference-types", ["u32"]),
    16: ("table.size", "reference-types", ["u32"]), 17: ("table.fill", "reference-types", ["u32"]),
    19: ("i64.add128", "wide-arithmetic", []), 20: ("i64.sub128", "wide-arithmetic", []),
    21: ("i64.mul_wide_s", "wide-arithmetic", []), 22: ("i64.mul_wide_u", "wide-arithmetic", []),
}


def _read_memarg(m, r):
    align = r.u32()
    if align & 0x40:
        if r.u32() != 0:
            m.feature_note("multi-memory", "access to memory > 0")
    r.uleb(64)


def _scan_body(m, r, func_index):
    for _ in range(r.u32()):      # locals
        r.u32()
        if r.byte() == 0x7B:
            m.unsupported.append((func_index, "a v128 local (SIMD)"))
            return
    last = None
    while not r.eof():
        op = r.byte()
        last = op
        if op == 0xFC:
            sub = r.u32()
            if sub not in FC_OPS:
                m.unsupported.append((func_index, "0xFC 0x%02x" % sub))
                return
            name, feature, imms = FC_OPS[sub]
            for _imm in imms:
                r.u32()
            m.feature_note(feature, name)
            continue
        if op == 0xFD:
            m.unsupported.append((func_index, "SIMD (0xFD prefix)"))
            return
        if op == 0xFE:
            sub = r.u32()
            if sub == 0x03:
                r.byte()
            elif sub <= 0x02 or 0x10 <= sub <= 0x4E:
                _read_memarg(m, r)
            else:
                m.unsupported.append((func_index, "0xFE 0x%02x" % sub))
                return
            m.feature_note("atomics", "0xFE 0x%02x" % sub)
            continue
        if op in DISABLED_OPCODES:
            m.unsupported.append((func_index, DISABLED_OPCODES[op]))
            return
        entry = OPCODES.get(op)
        if entry is None:
            m.unsupported.append((func_index, "opcode 0x%02x" % op))
            return
        kind, feature = entry
        if feature:
            m.feature_note(feature, OPCODE_NAMES.get(op, "0x%02x" % op))
        if kind == _NONE:
            pass
        elif kind == _BLOCK:
            b = r.peek()
            if b == 0x40 or b in VALTYPE_LETTER:
                r.byte()
            else:
                r.s_leb(33)
                m.feature_note("multi-value", "block with a type index")
        elif kind == _U32:
            r.u32()
        elif kind == _BRTABLE:
            for _ in range(r.u32()):
                r.u32()
            r.u32()
        elif kind == _CALLIND:
            r.u32()
            table = r.u32()
            if table != 0:
                m.feature_note("reference-types", "call_indirect on table %d" % table)
        elif kind == _SELECT_T:
            for _ in range(r.u32()):
                r.byte()
        elif kind == _MEMARG:
            _read_memarg(m, r)
        elif kind == _MEMIDX:
            if r.u32() != 0:
                m.feature_note("multi-memory", "memory.size/grow on memory > 0")
        elif kind == _S32:
            r.s_leb(32)
        elif kind == _S64:
            r.s_leb(64)
        elif kind == _F32:
            r.bytes(4)
        elif kind == _F64:
            r.bytes(8)
        elif kind == _REFTYPE:
            r.byte()
    if last != 0x0B:
        m.unsupported.append((func_index, "a body that does not end with 'end'"))


# ---------------------------------------------------------------------------
# ABI check
# ---------------------------------------------------------------------------
class CheckResult:
    def __init__(self, path, module):
        self.path = path
        self.module = module
        self.errors = []
        self.warnings = []
        self.kind = None             # "request", "event", "app_main"
        self.entry = None
        self.hardware = []           # inferred from imports, in HARDWARE_ORDER
        self.peripheral_imports = []  # esp32_gpio_* and friends
        self.memory_bytes = 0
        self.memory_max_bytes = None

    @property
    def ok(self):
        return not self.errors


def check_module(path, data, memory_limit=None):
    """Validates a module against the ABI. memory_limit is in bytes (None: not enforced)."""
    try:
        m = parse_module(data)
    except WasmError as e:
        res = CheckResult(path, None)
        res.errors.append("malformed module: %s" % e)
        return res
    res = CheckResult(path, m)
    err = res.errors.append
    warn = res.warnings.append

    # Imports: only functions, only from "env", only names the host links, with the host's signature
    hw = set()
    for (mod, name, kind, t) in m.imports:
        if kind != 0:
            err("import %s.%s is a %s; the host provides only functions" % (mod, name, KIND_NAMES.get(kind, kind)))
            continue
        if mod != "env":
            err("import %s.%s: the host links functions into the \"env\" module only" % (mod, name))
            continue
        if name not in KNOWN_IMPORTS:
            err("unknown import env.%s (the host does not provide it)" % name)
            continue
        sig = sig_string(*m.types[t]) if t < len(m.types) else "?"
        if sig != KNOWN_IMPORTS[name]:
            err("import env.%s has signature %s, the host links %s" % (name, sig, KNOWN_IMPORTS[name]))
        if name in HARDWARE_FOR_IMPORT:
            hw.add(HARDWARE_FOR_IMPORT[name])
        if name in PERIPHERAL_IMPORTS:
            res.peripheral_imports.append(name)
    res.hardware = [h for h in HARDWARE_ORDER if h in hw]

    # Memory: the host passes i32 offsets into memory 0, which the module defines
    if m.imported_memory:
        err("the module imports its memory; the host only runs modules that define their own")
    if not m.memories and not m.imported_memory:
        err("the module defines no linear memory")
    if m.memories:
        initial, maximum, flags = m.memories[0]
        if flags & 0x04:
            err("memory 0 is a 64-bit memory (memory64); the ABI passes 32-bit offsets")
        if flags & 0x02:
            warn("memory 0 is declared shared; pods run single-threaded")
        if len(m.memories) > 1:
            warn("the module defines %d memories; host imports read and write memory 0 only" % len(m.memories))
        res.memory_bytes = initial * PAGE
        res.memory_max_bytes = maximum * PAGE if maximum is not None else None
        if maximum is None:
            warn("memory has no maximum: memory.grow can run until the pod's limit traps it (link with --max-memory)")
        # A limit under one page backs the module's single first page with
        # the limit only (wasm3 memoryLimit): memory.grow then fails
        sub_page = memory_limit is not None and memory_limit < PAGE and initial <= 1
        if memory_limit is not None and res.memory_bytes > memory_limit and not sub_page:
            err("memory: the module declares %s (%d pages) of linear memory but the limit is %s"
                % (format_quantity(res.memory_bytes), initial, format_quantity(memory_limit)))
        if sub_page:
            warn("memory: the page is backed by %s only; accesses past it trap and memory.grow fails"
                             % format_quantity(memory_limit))

    # Exports
    ex = m.export_map()
    has_handle = "k32_handle" in ex
    entry = next((e for e in ENTRY_EXPORTS if e in ex), None)
    for name, want in REQUEST_EXPORTS.items():
        if name in ex:
            got = m.exported_func_sig(name)
            if got is None:
                err("export %s is not a function" % name)
            elif got != want:
                err("export %s has signature %s, the host calls %s" % (name, got, want))
    if entry:
        got = m.exported_func_sig(entry)
        if got is None:
            err("export %s is not a function" % entry)
        elif got not in ENTRY_SIGS:
            err("export %s has signature %s; the host calls it with no arguments (v() or i())" % (entry, got))
    event_like = any(n in ex for n in ("k32_init", "k32_tick", "k32_event"))
    if has_handle:
        res.kind = "request"
    elif entry:
        res.kind = "app_main"
    elif event_like:
        res.kind = "event"
    else:
        err("the module exports neither k32_handle (request pod) nor app_main, _start or main (app_main pod); exports: %s"
            % (", ".join(sorted(ex)) or "none"))
    res.entry = entry
    if has_handle and entry:
        warn("the module exports both k32_handle and %s: the host runs %s on the pod thread and serves requests between its calls" % (entry, entry))
    if (has_handle or "k32_event" in ex) and "k32_alloc" not in ex:
        err("k32_alloc is missing: the host copies requests and events into memory it obtains from k32_alloc")

    # Opcodes and sections
    for (fi, what) in m.unsupported:
        where = "function %d uses" % fi if fi is not None else "the module has"
        err("%s %s, which this wasm3 build does not run" % (where, what))
    if m.start is not None:
        warn("the module has a start function (index %d); it runs when the module is instantiated" % m.start)
    if m.imported_table:
        err("the module imports a table; the host provides none")
    return res


def static_memory_bytes(m):
    """The end of a module's static data, its zeroed data and its shadow stack in linear memory: the
    initial value of the exported global __heap_base, which wasm-ld defines (kube32 build exports it
    for C, and Rust exports it). None without that export: nothing then tells how much memory the
    module uses."""
    if m is None:
        return None
    imported = sum(1 for imp in m.imports if imp[2] == 3)
    for (name, kind, index) in m.exports:
        if kind == 3 and name == "__heap_base" and 0 <= index - imported < len(m.global_inits):
            return m.global_inits[index - imported][2]
    return None


def hardware_for_name(text):
    name = text.strip().lower()
    name = HARDWARE_ALIASES.get(name, name)
    if name not in HARDWARE:
        raise ValueError("unknown hardware '%s' (expected display, keyboard, speaker, microphone, ipc, sd, service-call, chip-temp or none)" % text.strip())
    return name


def format_report(res):
    lines = []
    m = res.module
    if m is None:
        lines.append("%s: unreadable" % res.path)
    else:
        lines.append("%s: %s bytes (code %s, data %s)" % (res.path, fmt_bytes(m.size), fmt_bytes(m.code_bytes), fmt_bytes(m.data_bytes)))
        if res.kind == "request":
            lines.append("kind: request pod (k32_handle)")
        elif res.kind == "event":
            lines.append("kind: event pod (no k32_handle; k32_init/k32_tick/k32_event)")
        elif res.kind == "app_main":
            lines.append("kind: app_main pod (%s runs on the pod thread)" % res.entry)
        else:
            lines.append("kind: unknown")
        funcs = sorted(n for (n, k, _i) in m.exports if k == 0)
        others = sorted("%s [%s]" % (n, KIND_NAMES.get(k, k)) for (n, k, _i) in m.exports if k != 0)
        lines.append("exports (%d): %s" % (len(m.exports), ", ".join(funcs + others) or "none"))
        imps = [(mod, n) for (mod, n, k, _t) in m.imports]
        if imps:
            lines.append("imports (%d): %s" % (len(imps), ", ".join(n if mod == "env" else mod + "." + n for mod, n in sorted(imps))))
        else:
            lines.append("imports (0)")
        if m.memories:
            initial, maximum, _f = m.memories[0]
            mx = "max %d page%s" % (maximum, "" if maximum == 1 else "s") if maximum is not None else "no maximum"
            lines.append("memory: %d page%s = %s bytes (%s), %s" % (initial, "" if initial == 1 else "s", fmt_bytes(initial * PAGE),
                                                                  format_quantity(initial * PAGE), mx))
        if m.features:
            parts = []
            for feat in sorted(m.features):
                ops = m.features[feat]
                detail = ", ".join("%s x%d" % (op, cnt) for op, cnt in sorted(ops.items()))
                parts.append("%s (%s)" % (feat, detail))
            lines.append("features beyond MVP: " + "; ".join(parts))
        else:
            lines.append("features beyond MVP: none")
        if res.hardware:
            resources = [HARDWARE[h][0] for h in res.hardware if HARDWARE[h][0]]
            lines.append("hardware: %s  (manifest resources: %s)" % (", ".join(res.hardware), ", ".join(resources) or "none"))
        else:
            lines.append("hardware: none")
        if res.peripheral_imports:
            lines.append("peripheral imports: %s  (granted by esp32.io/* resources, see docs/peripherals.md)"
                         % ", ".join(res.peripheral_imports))
    for w in res.warnings:
        lines.append("warning: " + w)
    for e in res.errors:
        lines.append("error: " + e)
    lines.append("ok" if res.ok else "FAILED (%d error%s)" % (len(res.errors), "" if len(res.errors) == 1 else "s"))
    return "\n".join(lines)


def read_wasm(path):
    if not os.path.isfile(path):
        raise FileNotFoundError("no such file: %s" % path)
    with open(path, "rb") as f:
        return f.read()
