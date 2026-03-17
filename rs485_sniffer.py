#!/usr/bin/env python3
"""RS485 Dual Bus Sniffer — GUI com decodificação Modbus RTU estilo Wireshark."""

import sys
import time
import threading
from datetime import datetime

try:
    import serial
except ImportError:
    print("Instale: pip install pyserial")
    sys.exit(1)

try:
    import tkinter as tk
    from tkinter import ttk, scrolledtext, filedialog, messagebox
except ImportError:
    print("tkinter não encontrado. Instale: sudo apt install python3-tk")
    sys.exit(1)


# =============================================================================
# Modbus RTU Decoder
# =============================================================================

# CRC16 table (polynomial 0xA001)
_CRC_TABLE = [
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040,
]


def modbus_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc = (crc >> 8) ^ _CRC_TABLE[(crc ^ b) & 0xFF]
    return crc


# mightyZAP register map
MZAP_REGS = {
    0x0000: ("Model Number", "R"),
    0x0001: ("Firmware Version", "R"),
    0x0002: ("Servo ID", "RW"),
    0x0003: ("Baud Rate", "RW"),
    0x0004: ("Protocol Type", "RW"),
    0x0005: ("Short Stroke Limit", "RW"),
    0x0006: ("Long Stroke Limit", "RW"),
    0x0007: ("Lowest Voltage", "R"),
    0x0008: ("Highest Voltage", "R"),
    0x0009: ("Alarm LED", "RW"),
    0x000A: ("Alarm Shutdown", "RW"),
    0x000B: ("Start Compliance", "RW"),
    0x000C: ("End Compliance", "RW"),
    0x000D: ("Speed Limit", "RW"),
    0x000E: ("Current Limit", "RW"),
    0x0011: ("Accel Ratio", "RW"),
    0x0012: ("Decel Ratio", "RW"),
    0x0032: ("Force Enable", "RW"),
    0x0033: ("LED", "RW"),
    0x0034: ("Goal Position", "RW"),
    0x0035: ("Goal Speed", "RW"),
    0x0036: ("Goal Current", "RW"),
    0x0037: ("Present Position", "R"),
    0x0038: ("Present Current", "R"),
    0x0039: ("Motor Op Rate", "R"),
    0x003A: ("Present Voltage", "R"),
    0x003B: ("Moving", "R"),
    0x003C: ("HW Error State", "R"),
}

FC_NAMES = {
    0x03: "Read Holding Registers",
    0x04: "Read Input Registers",
    0x06: "Write Single Register",
    0x10: "Write Multiple Registers",
    0xF6: "mightyZAP Factory Reset",
    0xF8: "mightyZAP Restart",
}

EXCEPTION_NAMES = {
    0x01: "Illegal Function",
    0x02: "Illegal Data Address",
    0x03: "Illegal Data Value",
    0x04: "Slave Device Failure",
    0x05: "Acknowledge",
    0x06: "Slave Device Busy",
    0x21: "mZAP Motor Moving",
    0x22: "mZAP Overload",
    0x23: "mZAP Checksum Error",
    0x24: "mZAP Range Error",
    0x25: "mZAP Instruction Error",
}

BAUD_MAP = {16: 115200, 32: 57600, 48: 38400, 64: 19200, 128: 9600}


def _reg_name(addr: int) -> str:
    entry = MZAP_REGS.get(addr)
    if entry:
        return f"{entry[0]} (0x{addr:04X})"
    return f"Reg 0x{addr:04X}"


def _format_value(reg_addr: int, value: int) -> str:
    """Format a register value with human-readable context."""
    if reg_addr == 0x0003:  # Baud Rate
        baud = BAUD_MAP.get(value, "?")
        return f"{value} ({baud} baud)"
    if reg_addr == 0x0032:  # Force Enable
        return f"{value} ({'ON' if value else 'OFF'})"
    if reg_addr == 0x0033:  # LED
        return f"{value} ({'ON' if value else 'OFF'})"
    if reg_addr == 0x003B:  # Moving
        return f"{value} ({'MOVING' if value else 'STOPPED'})"
    if reg_addr in (0x003A, 0x0007, 0x0008):  # Voltage (0.1V units)
        return f"{value} ({value / 10:.1f}V)"
    if reg_addr in (0x0038, 0x000E, 0x0036):  # Current (mA)
        return f"{value} ({value}mA)"
    return str(value)


def _reg_range_str(start: int, count: int) -> str:
    """Return a human-readable list of register names for a range."""
    names = []
    for i in range(min(count, 6)):
        names.append(_reg_name(start + i))
    if count > 6:
        names.append(f"... (+{count - 6} more)")
    return ", ".join(names)


class ModbusDecoder:
    """Tracks request/response pairs and decodes Modbus RTU frames."""

    def __init__(self):
        self._last_request = None  # (addr, fc, start_reg, count)

    def decode(self, data: bytes) -> tuple[str, str]:
        """
        Returns (summary, detail) strings and a tag for coloring.
        tag is one of: 'req_read', 'req_write', 'resp_ok', 'resp_err', 'unknown'
        Actually returns (summary, tag).
        """
        if len(data) < 4:
            return f"Too short ({len(data)}B)", "unknown"

        # CRC check
        recv_crc = (data[-1] << 8) | data[-2]
        calc_crc = modbus_crc16(data[:-2])
        if recv_crc != calc_crc:
            hex_str = " ".join(f"{b:02X}" for b in data)
            return f"⚠ CRC ERROR (recv=0x{recv_crc:04X} calc=0x{calc_crc:04X}) | {hex_str}", "resp_err"

        addr = data[0]
        fc = data[1]
        addr_str = "BROADCAST" if addr == 0 else f"Slave {addr}"

        # Exception response
        if fc & 0x80:
            orig_fc = fc & 0x7F
            ex_code = data[2] if len(data) > 2 else 0
            fc_name = FC_NAMES.get(orig_fc, f"FC 0x{orig_fc:02X}")
            ex_name = EXCEPTION_NAMES.get(ex_code, f"0x{ex_code:02X}")
            self._last_request = None
            return f"✖ {addr_str} | EXCEPTION on {fc_name}: {ex_name}", "resp_err"

        fc_name = FC_NAMES.get(fc, f"FC 0x{fc:02X}")

        # FC 0x03 / 0x04: Read Holding/Input Registers
        if fc in (0x03, 0x04):
            if len(data) == 8:
                # Request: [addr][fc][startHi][startLo][countHi][countLo][crc][crc]
                start_reg = (data[2] << 8) | data[3]
                count = (data[4] << 8) | data[5]
                self._last_request = (addr, fc, start_reg, count)
                regs = _reg_range_str(start_reg, count)
                return f"→ {addr_str} | {fc_name} | {regs} (×{count})", "req_read"
            else:
                # Response: [addr][fc][byteCount][data...][crc][crc]
                byte_count = data[2]
                reg_count = byte_count // 2
                values = []
                for i in range(reg_count):
                    values.append((data[3 + i * 2] << 8) | data[4 + i * 2])

                # Use last request context to show register names
                detail_parts = []
                if self._last_request and self._last_request[0] == addr and self._last_request[1] == fc:
                    _, _, start_reg, _ = self._last_request
                    for i, val in enumerate(values):
                        rname = _reg_name(start_reg + i)
                        fval = _format_value(start_reg + i, val)
                        detail_parts.append(f"{rname}={fval}")
                else:
                    for i, val in enumerate(values):
                        detail_parts.append(f"[{i}]=0x{val:04X} ({val})")

                vals_str = " | ".join(detail_parts)
                self._last_request = None
                return f"← {addr_str} | {fc_name} Response ({reg_count} regs) | {vals_str}", "resp_ok"

        # FC 0x06: Write Single Register
        if fc == 0x06:
            if len(data) == 8:
                reg_addr = (data[2] << 8) | data[3]
                value = (data[4] << 8) | data[5]
                rname = _reg_name(reg_addr)
                fval = _format_value(reg_addr, value)

                # Detect if this is a request or echo response
                if self._last_request and self._last_request == (addr, fc, reg_addr, value):
                    # This is the echo response
                    self._last_request = None
                    return f"← {addr_str} | Write OK | {rname} = {fval}", "resp_ok"
                else:
                    # This is a request
                    self._last_request = (addr, fc, reg_addr, value)
                    return f"→ {addr_str} | {fc_name} | {rname} = {fval}", "req_write"

        # FC 0x10: Write Multiple Registers
        if fc == 0x10:
            if len(data) == 8:
                # Response: [addr][fc][startHi][startLo][countHi][countLo][crc][crc]
                start_reg = (data[2] << 8) | data[3]
                count = (data[4] << 8) | data[5]
                regs = _reg_range_str(start_reg, count)
                self._last_request = None
                return f"← {addr_str} | Write Multiple OK | {regs} (×{count})", "resp_ok"
            else:
                # Request: [addr][fc][startHi][startLo][countHi][countLo][byteCount][data...][crc][crc]
                start_reg = (data[2] << 8) | data[3]
                count = (data[4] << 8) | data[5]
                values = []
                for i in range(count):
                    idx = 7 + i * 2
                    if idx + 1 < len(data) - 2:
                        values.append((data[idx] << 8) | data[idx + 1])

                detail_parts = []
                for i, val in enumerate(values):
                    rname = _reg_name(start_reg + i)
                    fval = _format_value(start_reg + i, val)
                    detail_parts.append(f"{rname}={fval}")

                vals_str = " | ".join(detail_parts)
                self._last_request = (addr, fc, start_reg, count)
                return f"→ {addr_str} | {fc_name} | {vals_str}", "req_write"

        # FC 0xF6 / 0xF8: mightyZAP special
        if fc in (0xF6, 0xF8):
            self._last_request = None
            return f"→ {addr_str} | {fc_name}", "req_write"

        # Unknown FC
        hex_str = " ".join(f"{b:02X}" for b in data)
        return f"? {addr_str} | {fc_name} | {hex_str}", "unknown"


# =============================================================================
# Sniffer Panel
# =============================================================================

class SnifferPanel:
    """Um painel completo de sniffer RS485 com decodificação Modbus RTU."""

    def __init__(self, parent, title="RS485"):
        self.parent = parent
        self.serial_port = None
        self.running = False
        self.paused = False
        self.msg_count = 0
        self.byte_count = 0
        self.start_time = None
        self.decoder = ModbusDecoder()

        self.frame = ttk.LabelFrame(parent, text=title, padding=5)
        self.frame.pack(side="left", fill="both", expand=True, padx=(5, 2), pady=5)

        self._build_ui()

    def _build_ui(self):
        # --- Connection settings ---
        conn_frame = ttk.LabelFrame(self.frame, text="Conexão", padding=5)
        conn_frame.pack(fill="x", pady=(0, 2))

        row0 = ttk.Frame(conn_frame)
        row0.pack(fill="x")

        ttk.Label(row0, text="Porta:").pack(side="left", padx=2)
        self.port_var = tk.StringVar(value="/dev/ttyUSB0")
        ttk.Entry(row0, textvariable=self.port_var, width=18).pack(side="left", padx=2)

        ttk.Label(row0, text="Baud:").pack(side="left", padx=2)
        self.baud_var = tk.StringVar(value="57600")
        ttk.Combobox(row0, textvariable=self.baud_var, width=10,
                     values=["9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"]).pack(side="left", padx=2)

        self.btn_connect = ttk.Button(row0, text="▶ Conectar", command=self.toggle_connection)
        self.btn_connect.pack(side="right", padx=2)

        row1 = ttk.Frame(conn_frame)
        row1.pack(fill="x", pady=(2, 0))

        ttk.Label(row1, text="Data:").pack(side="left", padx=2)
        self.data_var = tk.StringVar(value="8")
        ttk.Combobox(row1, textvariable=self.data_var, width=3, values=["7", "8"]).pack(side="left", padx=2)

        ttk.Label(row1, text="Parity:").pack(side="left", padx=2)
        self.parity_var = tk.StringVar(value="N")
        ttk.Combobox(row1, textvariable=self.parity_var, width=5, values=["N", "E", "O"]).pack(side="left", padx=2)

        ttk.Label(row1, text="Stop:").pack(side="left", padx=2)
        self.stop_var = tk.StringVar(value="1")
        ttk.Combobox(row1, textvariable=self.stop_var, width=3, values=["1", "1.5", "2"]).pack(side="left", padx=2)

        # --- Display options ---
        opt_frame = ttk.LabelFrame(self.frame, text="Opções", padding=5)
        opt_frame.pack(fill="x", pady=2)

        orow0 = ttk.Frame(opt_frame)
        orow0.pack(fill="x")

        self.decode_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(orow0, text="Modbus Decode", variable=self.decode_var).pack(side="left", padx=3)

        self.hex_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(orow0, text="HEX", variable=self.hex_var).pack(side="left", padx=3)

        self.timestamp_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(orow0, text="Timestamp", variable=self.timestamp_var).pack(side="left", padx=3)

        self.autoscroll_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(orow0, text="Auto-scroll", variable=self.autoscroll_var).pack(side="left", padx=3)

        orow1 = ttk.Frame(opt_frame)
        orow1.pack(fill="x", pady=(2, 0))

        ttk.Label(orow1, text="Timeout (ms):").pack(side="left", padx=(3, 2))
        self.timeout_var = tk.StringVar(value="5")
        ttk.Entry(orow1, textvariable=self.timeout_var, width=5).pack(side="left", padx=2)

        ttk.Label(orow1, text="Filtro Slave ID:").pack(side="left", padx=(10, 2))
        self.filter_var = tk.StringVar(value="")
        ttk.Entry(orow1, textvariable=self.filter_var, width=8).pack(side="left", padx=2)
        ttk.Label(orow1, text="(vazio=todos)", font=("", 8)).pack(side="left")

        # --- Legend ---
        leg_frame = ttk.Frame(self.frame)
        leg_frame.pack(fill="x", pady=(2, 0))

        for color, label in [("#4ec9b0", "→ Read"), ("#dcdcaa", "→ Write"),
                              ("#6a9955", "← Response OK"), ("#f44747", "✖ Error/Exception"),
                              ("#6a6a6a", "HEX raw")]:
            box = tk.Label(leg_frame, text="  ", bg=color, width=2)
            box.pack(side="left", padx=(4, 0))
            tk.Label(leg_frame, text=label, font=("Courier", 8)).pack(side="left", padx=(1, 6))

        # --- Text area ---
        self.text = scrolledtext.ScrolledText(self.frame, wrap="word", font=("Courier", 10),
                                               bg="#1e1e1e", fg="#d4d4d4", insertbackground="white")
        self.text.pack(fill="both", expand=True, pady=2)

        # Tags for Modbus decode coloring
        self.text.tag_configure("timestamp", foreground="#569cd6")
        self.text.tag_configure("hex_raw", foreground="#6a6a6a")
        self.text.tag_configure("req_read", foreground="#4ec9b0")     # cyan — read request
        self.text.tag_configure("req_write", foreground="#dcdcaa")    # yellow — write request
        self.text.tag_configure("resp_ok", foreground="#6a9955")      # green — response OK
        self.text.tag_configure("resp_err", foreground="#f44747")     # red — error/exception
        self.text.tag_configure("unknown", foreground="#ce9178")      # orange — unknown
        self.text.tag_configure("info", foreground="#dcdcaa")
        self.text.tag_configure("error", foreground="#f44747")

        # --- Bottom controls ---
        bottom = ttk.Frame(self.frame)
        bottom.pack(fill="x", pady=(2, 0))

        ttk.Button(bottom, text="🗑 Limpar", command=self.clear_log).pack(side="left", padx=2)
        self.btn_pause = ttk.Button(bottom, text="⏸ Pausar", command=self.toggle_pause, state="disabled")
        self.btn_pause.pack(side="left", padx=2)
        ttk.Button(bottom, text="💾 Salvar", command=self.save_log).pack(side="left", padx=2)

        self.status_var = tk.StringVar(value="Desconectado")
        ttk.Label(bottom, textvariable=self.status_var).pack(side="right", padx=5)

    # --- Connection ---

    def toggle_connection(self):
        if self.running:
            self.stop()
        else:
            self.start()

    def start(self):
        port = self.port_var.get()
        baud = int(self.baud_var.get())
        bytesize = int(self.data_var.get())
        parity = self.parity_var.get()
        stopbits = float(self.stop_var.get())

        parity_map = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN, "O": serial.PARITY_ODD}
        stop_map = {1: serial.STOPBITS_ONE, 1.5: serial.STOPBITS_ONE_POINT_FIVE, 2: serial.STOPBITS_TWO}

        try:
            self.serial_port = serial.Serial(
                port=port, baudrate=baud, bytesize=bytesize,
                parity=parity_map[parity], stopbits=stop_map[stopbits], timeout=0,
            )
        except Exception as e:
            messagebox.showerror("Erro", f"Não foi possível abrir {port}:\n{e}")
            return

        self.running = True
        self.paused = False
        self.msg_count = 0
        self.byte_count = 0
        self.start_time = time.time()
        self.decoder = ModbusDecoder()
        self.btn_connect.config(text="⏹ Desconectar")
        self.btn_pause.config(state="normal")
        self.log(f"Conectado: {port} @ {baud} {bytesize}{parity}{stopbits}\n", "info")

        self.reader_thread = threading.Thread(target=self._read_loop, daemon=True)
        self.reader_thread.start()
        self._update_status()

    def stop(self):
        self.running = False
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        self.btn_connect.config(text="▶ Conectar")
        self.btn_pause.config(state="disabled")
        self.log("Desconectado.\n", "info")
        self.status_var.set("Desconectado")

    def toggle_pause(self):
        self.paused = not self.paused
        self.btn_pause.config(text="▶ Retomar" if self.paused else "⏸ Pausar")

    # --- Reading ---

    def _read_loop(self):
        timeout_s = max(1, int(self.timeout_var.get())) / 1000.0
        buf = bytearray()
        last_rx = time.time()

        while self.running:
            try:
                data = self.serial_port.read(256)
            except Exception as e:
                self.frame.after(0, lambda: self.log(f"Erro leitura: {e}\n", "error"))
                break

            now = time.time()
            if data:
                if buf and (now - last_rx) > timeout_s:
                    self._flush(buf)
                    buf = bytearray()
                buf.extend(data)
                last_rx = now
            else:
                if buf and (now - last_rx) > timeout_s:
                    self._flush(buf)
                    buf = bytearray()
                time.sleep(0.001)

        if buf:
            self._flush(buf)

    def _flush(self, data: bytearray):
        if self.paused:
            return

        # Slave ID filter
        filt = self.filter_var.get().strip()
        if filt and len(data) >= 1:
            try:
                filter_ids = {int(x.strip()) for x in filt.split(",")}
                if data[0] not in filter_ids and data[0] != 0:  # always show broadcast
                    return
            except ValueError:
                pass

        self.msg_count += 1
        self.byte_count += len(data)
        parts = []

        if self.timestamp_var.get():
            ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            parts.append(("timestamp", f"[{ts}] "))

        if self.decode_var.get():
            summary, tag = self.decoder.decode(bytes(data))
            parts.append((tag, summary))
            if self.hex_var.get():
                hex_str = " ".join(f"{b:02X}" for b in data)
                parts.append(("hex_raw", f"\n           {hex_str}"))
        else:
            parts.append(("info", f"({len(data):3d}B) "))
            if self.hex_var.get():
                hex_str = " ".join(f"{b:02X}" for b in data)
                parts.append(("unknown", hex_str))

        parts.append((None, "\n"))
        self.frame.after(0, lambda p=parts: self._append(p))

    def _append(self, parts):
        for tag, text in parts:
            if tag:
                self.text.insert("end", text, tag)
            else:
                self.text.insert("end", text)
        if self.autoscroll_var.get():
            self.text.see("end")

    # --- Utilities ---

    def log(self, msg, tag=None):
        if tag:
            self.text.insert("end", msg, tag)
        else:
            self.text.insert("end", msg)
        self.text.see("end")

    def clear_log(self):
        self.text.delete("1.0", "end")
        self.msg_count = 0
        self.byte_count = 0
        self.decoder = ModbusDecoder()

    def save_log(self):
        path = filedialog.asksaveasfilename(defaultextension=".log",
                                            filetypes=[("Log", "*.log"), ("Text", "*.txt"), ("All", "*.*")])
        if path:
            with open(path, "w") as f:
                f.write(self.text.get("1.0", "end"))
            self.log(f"Salvo em {path}\n", "info")

    def _update_status(self):
        if not self.running:
            return
        elapsed = time.time() - self.start_time
        self.status_var.set(
            f"{'⏸ PAUSADO | ' if self.paused else ''}"
            f"Msgs: {self.msg_count} | Bytes: {self.byte_count} | "
            f"{int(elapsed)}s"
        )
        self.frame.after(500, self._update_status)


# =============================================================================
# Main App
# =============================================================================

class DualRS485Sniffer:
    def __init__(self, root):
        self.root = root
        self.root.title("RS485 Modbus RTU Analyzer")
        self.root.geometry("1700x750")
        self.root.minsize(1200, 500)

        container = ttk.Frame(root)
        container.pack(fill="both", expand=True)

        self.left = SnifferPanel(container, title="🔵 Bus 1 (Primary)")
        self.left.port_var.set("/dev/ttyUSB0")

        self.right = SnifferPanel(container, title="🟠 Bus 2 (Sync)")
        self.right.port_var.set("/dev/ttyUSB1")


if __name__ == "__main__":
    root = tk.Tk()
    app = DualRS485Sniffer(root)
    root.mainloop()
