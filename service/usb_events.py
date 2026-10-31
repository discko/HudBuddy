"""macOS IOKit USB arrival/removal notifications; never opens a serial port.

ABI declarations follow IOKitLib.h / CoreFoundation headers in the macOS SDK.
The run loop lives on one thread; the bridge consumes queued state transitions.
"""
from __future__ import annotations

from collections import deque
import ctypes as C
import logging
import sys
import threading

LOG = logging.getLogger("hudbuddy.usb")


class USBEventError(RuntimeError):
    pass


class Presence:
    """Registry IDs survive removal even when USB properties are no longer readable."""
    def __init__(self, vid: int, pid: int, serial: str):
        self.vid, self.pid, self.serial = vid, pid, serial.upper()
        self.event = threading.Event()
        self.lock = threading.Lock()
        self.registry_ids: set[int] = set()
        self.changes: deque[bool] = deque()

    def add(self, registry_id: int, vid: int | None, pid: int | None, serial: str | None) -> None:
        if (vid, pid, (serial or "").upper()) != (self.vid, self.pid, self.serial):
            return
        with self.lock:
            if registry_id in self.registry_ids:
                return
            was_online = bool(self.registry_ids)
            self.registry_ids.add(registry_id)
            LOG.debug("target USB registryID=%d attached", registry_id)
            if not was_online:
                self.changes.append(True)
                self.event.set()

    def remove(self, registry_id: int) -> None:
        with self.lock:
            was_online = bool(self.registry_ids)
            self.registry_ids.discard(registry_id)
            if was_online and not self.registry_ids:
                LOG.debug("target USB registryID=%d removed", registry_id)
                self.changes.append(False)
                self.event.set()

    def take_changes(self) -> list[bool]:
        with self.lock:
            changes = list(self.changes)
            self.changes.clear()
            self.event.clear()
            return changes

    def ids(self) -> set[int]:
        with self.lock:
            return self.registry_ids.copy()


class USBMonitor:
    def __init__(self, vid: int, pid: int, serial: str):
        self.presence = Presence(vid, pid, serial)
        self.event = self.presence.event
        self.ready = threading.Event()
        self.stopping = threading.Event()
        self.lifecycle_lock = threading.Lock()
        self.error: Exception | None = None
        self.thread: threading.Thread | None = None
        self.run_loop = None
        self.callbacks = []  # CFUNCTYPE callbacks must live as long as notifications.

    def _load(self) -> None:
        if sys.platform != "darwin":
            raise USBEventError("USB event monitoring requires macOS IOKit")
        self.io = C.CDLL("/System/Library/Frameworks/IOKit.framework/IOKit")
        self.cf = C.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        self.callback_type = C.CFUNCTYPE(None, C.c_void_p, C.c_uint32)
        declarations = [
            (self.io, "IONotificationPortCreate", [C.c_uint32], C.c_void_p),
            (self.io, "IONotificationPortDestroy", [C.c_void_p], None),
            (self.io, "IONotificationPortGetRunLoopSource", [C.c_void_p], C.c_void_p),
            (self.io, "IOServiceMatching", [C.c_char_p], C.c_void_p),
            (self.io, "IOServiceAddMatchingNotification", [C.c_void_p, C.c_char_p, C.c_void_p,
                                                       self.callback_type, C.c_void_p, C.POINTER(C.c_uint32)], C.c_int),
            (self.io, "IOIteratorNext", [C.c_uint32], C.c_uint32),
            (self.io, "IOObjectRelease", [C.c_uint32], C.c_int),
            (self.io, "IORegistryEntryGetRegistryEntryID", [C.c_uint32, C.POINTER(C.c_uint64)], C.c_int),
            (self.io, "IORegistryEntryCreateCFProperty", [C.c_uint32, C.c_void_p, C.c_void_p, C.c_uint32], C.c_void_p),
            (self.cf, "CFStringCreateWithCString", [C.c_void_p, C.c_char_p, C.c_uint32], C.c_void_p),
            (self.cf, "CFStringGetCString", [C.c_void_p, C.c_void_p, C.c_long, C.c_uint32], C.c_bool),
            (self.cf, "CFNumberGetValue", [C.c_void_p, C.c_long, C.c_void_p], C.c_bool),
            (self.cf, "CFGetTypeID", [C.c_void_p], C.c_ulong),
            (self.cf, "CFStringGetTypeID", [], C.c_ulong),
            (self.cf, "CFNumberGetTypeID", [], C.c_ulong),
            (self.cf, "CFRelease", [C.c_void_p], None),
            (self.cf, "CFRunLoopGetCurrent", [], C.c_void_p),
            (self.cf, "CFRunLoopAddSource", [C.c_void_p, C.c_void_p, C.c_void_p], None),
            (self.cf, "CFRunLoopRemoveSource", [C.c_void_p, C.c_void_p, C.c_void_p], None),
            (self.cf, "CFRunLoopRun", [], None),
            (self.cf, "CFRunLoopStop", [C.c_void_p], None),
            (self.cf, "CFRunLoopWakeUp", [C.c_void_p], None),
        ]
        for library, name, args, result in declarations:
            function = getattr(library, name)
            function.argtypes, function.restype = args, result
        self.mode = C.c_void_p.in_dll(self.cf, "kCFRunLoopDefaultMode").value

    def _property(self, service: int, name: str) -> str | int | None:
        key = self.cf.CFStringCreateWithCString(None, name.encode(), 0x08000100)
        if not key:
            raise USBEventError("failed to allocate IOKit property key")
        try:
            value = self.io.IORegistryEntryCreateCFProperty(service, key, None, 0)
        finally:
            self.cf.CFRelease(key)
        if not value:
            return None
        try:
            kind = self.cf.CFGetTypeID(value)
            if kind == self.cf.CFStringGetTypeID():
                buffer = C.create_string_buffer(1024)
                if self.cf.CFStringGetCString(value, buffer, len(buffer), 0x08000100):
                    return buffer.value.decode("utf-8")
            elif kind == self.cf.CFNumberGetTypeID():
                number = C.c_int64()
                if self.cf.CFNumberGetValue(value, 4, C.byref(number)):  # kCFNumberSInt64Type
                    return number.value
            return None
        finally:
            self.cf.CFRelease(value)

    def _drain(self, iterator: int, removed: bool) -> None:
        while True:
            service = self.io.IOIteratorNext(iterator)
            if not service:
                break  # Exhaustion also rearms this notification.
            try:
                registry_id = C.c_uint64()
                if self.io.IORegistryEntryGetRegistryEntryID(service, C.byref(registry_id)):
                    continue
                if removed:
                    # Do not re-read idVendor/serial: termination can remove properties.
                    self.presence.remove(registry_id.value)
                else:
                    serial = self._property(service, "USB Serial Number") or self._property(service, "kUSBSerialNumberString")
                    self.presence.add(registry_id.value, self._property(service, "idVendor"),
                                      self._property(service, "idProduct"), serial if isinstance(serial, str) else None)
            finally:
                self.io.IOObjectRelease(service)

    def _callback(self, iterator: int, removed: bool) -> None:
        try:
            self._drain(iterator, removed)
        except Exception as error:
            self.error = error
            self.event.set()
            self.cf.CFRunLoopStop(self.run_loop)

    def _thread_main(self) -> None:
        port = source = None
        iterators = []
        try:
            self._load()
            self.run_loop = self.cf.CFRunLoopGetCurrent()
            port = self.io.IONotificationPortCreate(0)  # kIOMainPortDefault
            if not port:
                raise USBEventError("IONotificationPortCreate failed")
            source = self.io.IONotificationPortGetRunLoopSource(port)
            if not source:
                raise USBEventError("IONotificationPortGetRunLoopSource failed")
            self.cf.CFRunLoopAddSource(self.run_loop, source, self.mode)
            # HostDevice is current macOS; Device covers legacy drivers. Presence
            # coalesces duplicate reports and emits only physical online/offline edges.
            for class_name in (b"IOUSBHostDevice", b"IOUSBDevice"):
                for notification, removed in ((b"IOServiceTerminate", True), (b"IOServiceFirstMatch", False)):
                    callback = self.callback_type(lambda _, iterator, removed=removed: self._callback(iterator, removed))
                    self.callbacks.append(callback)
                    matching = self.io.IOServiceMatching(class_name)
                    if not matching:
                        raise USBEventError("IOServiceMatching failed")
                    iterator = C.c_uint32()
                    # IOKit consumes matching, including on registration failure.
                    result = self.io.IOServiceAddMatchingNotification(port, notification, matching, callback, None, C.byref(iterator))
                    if result:
                        raise USBEventError(f"IOServiceAddMatchingNotification failed: {result:#x}")
                    iterators.append(iterator.value)
                    self._drain(iterator.value, removed)  # Detect devices already plugged in; arm notification.
            self.ready.set()
            if not self.stopping.is_set():
                self.cf.CFRunLoopRun()
        except Exception as error:
            self.error = error
            self.event.set()
        finally:
            self.ready.set()
            for iterator in iterators:
                self.io.IOObjectRelease(iterator)
            with self.lifecycle_lock:
                if source:
                    self.cf.CFRunLoopRemoveSource(self.run_loop, source, self.mode)
                if port:
                    self.io.IONotificationPortDestroy(port)
                self.run_loop = None

    def start(self) -> None:
        self.thread = threading.Thread(target=self._thread_main, name="hudbuddy-usb", daemon=True)
        self.thread.start()
        if not self.ready.wait(5):
            self.close()
            raise USBEventError("IOKit notification startup timed out")
        self._raise_error()

    def _raise_error(self) -> None:
        if self.error:
            raise USBEventError(f"IOKit USB monitor failed: {self.error}") from self.error

    def take_changes(self) -> list[bool]:
        self._raise_error()
        return self.presence.take_changes()

    def close(self) -> None:
        self.stopping.set()
        with self.lifecycle_lock:
            if self.run_loop:
                self.cf.CFRunLoopStop(self.run_loop)
                self.cf.CFRunLoopWakeUp(self.run_loop)
        if self.thread:
            self.thread.join(timeout=5)

