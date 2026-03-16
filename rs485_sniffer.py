#!/usr/bin/env python3
"""RS485 Bus Sniffer — GUI para monitorar tráfego em barramento RS485."""

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


class RS485Sniffer:
    def __init__(self, root):
        self.root = root
        self.root.title("RS485 Bus Sniffer")
        self.root.geometry("900x650")
        self.root.minsize(700, 500)

        self.serial_port = None
        self.running = False
        self.paused = False
        self.msg_count = 0
        self.byte_count = 0
        self.start_time = None

        self._build_ui()

    def _build_ui(self):
        # --- Top: Connection settings ---
        conn_frame = ttk.LabelFrame(self.root, text="Conexão", padding=5)
        conn_frame.pack(fill="x", padx=5, pady=(5, 2))

        ttk.Label(conn_frame, text="Porta:").grid(row=0, column=0, padx=2)
        self.port_var = tk.StringVar(value="/dev/ttyUSB0")
        ttk.Entry(conn_frame, textvariable=self.port_var, width=18).grid(row=0, column=1, padx=2)

        ttk.Label(conn_frame, text="Baud:").grid(row=0, column=2, padx=2)
        self.baud_var = tk.StringVar(value="115200")
        baud_combo = ttk.Combobox(conn_frame, textvariable=self.baud_var, width=10,
                                  values=["9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"])
        baud_combo.grid(row=0, column=3, padx=2)

        ttk.Label(conn_frame, text="Data:").grid(row=0, column=4, padx=2)
        self.data_var = tk.StringVar(value="8")
        ttk.Combobox(conn_frame, textvariable=self.data_var, width=3, values=["7", "8"]).grid(row=0, column=5, padx=2)

        ttk.Label(conn_frame, text="Parity:").grid(row=0, column=6, padx=2)
        self.parity_var = tk.StringVar(value="N")
        ttk.Combobox(conn_frame, textvariable=self.parity_var, width=5,
                     values=["N", "E", "O"]).grid(row=0, column=7, padx=2)

        ttk.Label(conn_frame, text="Stop:").grid(row=0, column=8, padx=2)
        self.stop_var = tk.StringVar(value="1")
        ttk.Combobox(conn_frame, textvariable=self.stop_var, width=3, values=["1", "1.5", "2"]).grid(row=0, column=9, padx=2)

        self.btn_connect = ttk.Button(conn_frame, text="▶ Conectar", command=self.toggle_connection)
        self.btn_connect.grid(row=0, column=10, padx=(10, 2))

        # --- Display options ---
        opt_frame = ttk.LabelFrame(self.root, text="Opções", padding=5)
        opt_frame.pack(fill="x", padx=5, pady=2)

        self.hex_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(opt_frame, text="HEX", variable=self.hex_var).pack(side="left", padx=5)

        self.ascii_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(opt_frame, text="ASCII", variable=self.ascii_var).pack(side="left", padx=5)

        self.timestamp_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(opt_frame, text="Timestamp", variable=self.timestamp_var).pack(side="left", padx=5)

        self.newline_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(opt_frame, text="Quebra por silêncio (3.5 char)", variable=self.newline_var).pack(side="left", padx=5)

        ttk.Label(opt_frame, text="Timeout (ms):").pack(side="left", padx=(15, 2))
        self.timeout_var = tk.StringVar(value="10")
        ttk.Entry(opt_frame, textvariable=self.timeout_var, width=5).pack(side="left", padx=2)

        self.autoscroll_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(opt_frame, text="Auto-scroll", variable=self.autoscroll_var).pack(side="left", padx=5)

        # --- Main: Text area ---
        self.text = scrolledtext.ScrolledText(self.root, wrap="word", font=("Courier", 10),
                                               bg="#1e1e1e", fg="#d4d4d4", insertbackground="white")
        self.text.pack(fill="both", expand=True, padx=5, pady=2)
        self.text.tag_configure("timestamp", foreground="#569cd6")
        self.text.tag_configure("hex", foreground="#ce9178")
        self.text.tag_configure("ascii", foreground="#6a9955")
        self.text.tag_configure("info", foreground="#dcdcaa")
        self.text.tag_configure("error", foreground="#f44747")

        # --- Bottom: Controls + Status ---
        bottom_frame = ttk.Frame(self.root)
        bottom_frame.pack(fill="x", padx=5, pady=(2, 5))

        ttk.Button(bottom_frame, text="🗑 Limpar", command=self.clear_log).pack(side="left", padx=2)
        self.btn_pause = ttk.Button(bottom_frame, text="⏸ Pausar", command=self.toggle_pause, state="disabled")
        self.btn_pause.pack(side="left", padx=2)
        ttk.Button(bottom_frame, text="💾 Salvar", command=self.save_log).pack(side="left", padx=2)

        self.status_var = tk.StringVar(value="Desconectado")
        ttk.Label(bottom_frame, textvariable=self.status_var).pack(side="right", padx=5)

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
                port=port,
                baudrate=baud,
                bytesize=bytesize,
                parity=parity_map[parity],
                stopbits=stop_map[stopbits],
                timeout=0,
            )
        except Exception as e:
            messagebox.showerror("Erro", f"Não foi possível abrir {port}:\n{e}")
            return

        self.running = True
        self.paused = False
        self.msg_count = 0
        self.byte_count = 0
        self.start_time = time.time()
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

    def _read_loop(self):
        """Thread de leitura. Agrupa bytes por silêncio no barramento."""
        timeout_s = max(1, int(self.timeout_var.get())) / 1000.0
        buf = bytearray()
        last_rx = time.time()

        while self.running:
            try:
                data = self.serial_port.read(256)
            except Exception as e:
                self.root.after(0, lambda: self.log(f"Erro leitura: {e}\n", "error"))
                break

            now = time.time()

            if data:
                # Se houve silêncio e já tinha dados no buffer, flush
                if buf and (now - last_rx) > timeout_s:
                    self._flush(buf)
                    buf = bytearray()
                buf.extend(data)
                last_rx = now
            else:
                # Sem dados — flush se timeout
                if buf and (now - last_rx) > timeout_s:
                    self._flush(buf)
                    buf = bytearray()
                time.sleep(0.001)

        # Flush restante
        if buf:
            self._flush(buf)

    def _flush(self, data: bytearray):
        """Formata e envia um frame capturado para a tela."""
        if self.paused:
            return

        self.msg_count += 1
        self.byte_count += len(data)

        parts = []

        if self.timestamp_var.get():
            ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            parts.append(("timestamp", f"[{ts}] "))

        parts.append(("info", f"({len(data):3d}B) "))

        if self.hex_var.get():
            hex_str = " ".join(f"{b:02X}" for b in data)
            parts.append(("hex", hex_str))

        if self.ascii_var.get():
            ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in data)
            parts.append(("ascii", f"  |{ascii_str}|"))

        parts.append((None, "\n"))

        self.root.after(0, lambda p=parts: self._append(p))

    def _append(self, parts):
        for tag, text in parts:
            if tag:
                self.text.insert("end", text, tag)
            else:
                self.text.insert("end", text)
        if self.autoscroll_var.get():
            self.text.see("end")

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
            f"Msgs: {self.msg_count}  |  Bytes: {self.byte_count}  |  "
            f"Tempo: {int(elapsed)}s"
        )
        self.root.after(500, self._update_status)


if __name__ == "__main__":
    root = tk.Tk()
    app = RS485Sniffer(root)
    root.mainloop()
