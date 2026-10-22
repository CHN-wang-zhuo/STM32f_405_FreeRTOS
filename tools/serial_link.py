"""Small serial adapter: pyserial when installed, native Win32 otherwise."""
import os


def open_serial(port, baud=115200):
    try:
        import serial
    except ImportError:
        if os.name != "nt":
            raise RuntimeError("Install pyserial: python -m pip install pyserial") from None
        return WindowsSerial(port, baud)
    return serial.Serial(port, baud, timeout=0.1, write_timeout=5)


class WindowsSerial:
    """Synchronous 8N1, no flow control; keeps Windows use dependency-free."""
    def __init__(self, port, baud):
        import ctypes as c
        from ctypes import wintypes as w
        self.c = c
        self.api = c.WinDLL("kernel32", use_last_error=True)

        class DCB(c.Structure):
            _fields_ = [("length", w.DWORD), ("baud", w.DWORD), ("flags", w.DWORD),
                        ("reserved", w.WORD), ("xon_limit", w.WORD), ("xoff_limit", w.WORD),
                        ("byte_size", w.BYTE), ("parity", w.BYTE), ("stop_bits", w.BYTE),
                        ("xon", c.c_char), ("xoff", c.c_char), ("error", c.c_char),
                        ("eof", c.c_char), ("event", c.c_char), ("reserved1", w.WORD)]

        class Timeouts(c.Structure):
            _fields_ = [("interval", w.DWORD), ("read_multiplier", w.DWORD),
                        ("read_constant", w.DWORD), ("write_multiplier", w.DWORD),
                        ("write_constant", w.DWORD)]

        self.api.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p,
                                          w.DWORD, w.DWORD, w.HANDLE]
        self.api.CreateFileW.restype = w.HANDLE
        self.api.GetCommState.argtypes = [w.HANDLE, c.POINTER(DCB)]
        self.api.SetCommState.argtypes = [w.HANDLE, c.POINTER(DCB)]
        self.api.SetCommTimeouts.argtypes = [w.HANDLE, c.POINTER(Timeouts)]
        self.api.ReadFile.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
        self.api.WriteFile.argtypes = self.api.ReadFile.argtypes
        self.api.PurgeComm.argtypes = [w.HANDLE, w.DWORD]
        self.api.CloseHandle.argtypes = [w.HANDLE]
        self.handle = self.api.CreateFileW("\\\\.\\" + port, 0xC0000000, 0, None, 3, 0, None)
        if self.handle == c.c_void_p(-1).value:
            raise c.WinError(c.get_last_error())
        try:
            dcb = DCB()
            dcb.length = c.sizeof(DCB)
            self._check(self.api.GetCommState(self.handle, c.byref(dcb)))
            dcb.baud, dcb.flags = baud, 1  # binary, no CTS/DSR/XON/XOFF flow control
            dcb.byte_size, dcb.parity, dcb.stop_bits = 8, 0, 0
            self._check(self.api.SetCommState(self.handle, c.byref(dcb)))
            timeouts = Timeouts(0xFFFFFFFF, 0, 100, 0, 5000)
            self._check(self.api.SetCommTimeouts(self.handle, c.byref(timeouts)))
            self._check(self.api.PurgeComm(self.handle, 0xF))
        except BaseException:
            self.close()
            raise

    def _check(self, value):
        if not value:
            raise self.c.WinError(self.c.get_last_error())

    def read(self, count=4096):
        from ctypes import wintypes as w
        buffer = self.c.create_string_buffer(count)
        actual = w.DWORD()
        self._check(self.api.ReadFile(self.handle, buffer, count, self.c.byref(actual), None))
        return buffer.raw[:actual.value]

    def write(self, data):
        from ctypes import wintypes as w
        buffer = self.c.create_string_buffer(data)
        actual = w.DWORD()
        self._check(self.api.WriteFile(self.handle, buffer, len(data), self.c.byref(actual), None))
        if actual.value != len(data):
            raise TimeoutError("Serial write did not complete")
        return actual.value

    def close(self):
        if self.handle is not None:
            self.api.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
