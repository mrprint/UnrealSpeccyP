#!/usr/bin/env python3
# Fails if a Windows executable imports a function that Windows 7 does not have.
#
# The sdl2_desktop build is meant to run on Windows 7 and later (see WINDOWS_TARGET_WIN7 in
# build/cmake/CMakeLists.txt). _WIN32_WINNT only hides the declarations of newer functions from
# OUR code; the statically linked runtime and third-party code can still import them - the
# MSVC STL of toolset 14.50+, for one, calls functions of Windows 8 directly. Such an exe
# starts on Windows 10 and dies on Windows 7 with "entry point not found". This catches that in CI.
#
# Usage: check_win7_imports.py FILE.exe [FILE.exe ...]
# Pure Python 3, no dependencies; reads PE32 and PE32+, import and delay-load tables.
#
# The list below is of functions that are KNOWN to be newer than Windows 7. It cannot be
# complete - a clean result does not replace starting the program on a real Windows 7.

import struct
import sys

NEWER_THAN_WIN7 = {
    # Windows 8
    "GetSystemTimePreciseAsFileTime": "8", "WaitOnAddress": "8", "WakeByAddressSingle": "8",
    "WakeByAddressAll": "8", "CreateFile2": "8", "CopyFile2": "8", "GetOverlappedResultEx": "8",
    "PathCchCanonicalize": "8", "PathCchCanonicalizeEx": "8", "PathCchCombine": "8",
    "PathCchCombineEx": "8", "PathCchAppend": "8", "PathCchAppendEx": "8", "PathCchRemoveFileSpec": "8",
    "PathCchAddBackslash": "8", "PathCchRemoveBackslash": "8", "PathCchFindExtension": "8",
    "DiscardVirtualMemory": "8", "OfferVirtualMemory": "8", "ReclaimVirtualMemory": "8",
    "VirtualAllocFromApp": "8", "VirtualProtectFromApp": "8", "MapViewOfFileFromApp": "8",
    "CreateFileMappingFromApp": "8", "OpenFileMappingFromApp": "8",
    "SetProcessMitigationPolicy": "8", "GetProcessMitigationPolicy": "8",
    "GetCurrentThreadStackLimits": "8", "GetCurrentPackageFullName": "8", "GetCurrentPackageId": "8",
    "RoInitialize": "8", "RoUninitialize": "8", "RoGetActivationFactory": "8",
    "WindowsCreateString": "8", "WindowsDeleteString": "8", "SetThreadpoolTimerEx": "8",
    # Windows 8.1
    "SetProcessDpiAwareness": "8.1", "GetProcessDpiAwareness": "8.1", "GetDpiForMonitor": "8.1",
    # Windows 10
    "SetThreadDescription": "10", "GetThreadDescription": "10", "QueryUnbiasedInterruptTimePrecise": "10",
    "GetDpiForWindow": "10", "GetDpiForSystem": "10", "SetProcessDpiAwarenessContext": "10",
    "SetThreadDpiAwarenessContext": "10", "GetThreadDpiAwarenessContext": "10",
    "GetWindowDpiAwarenessContext": "10", "AreDpiAwarenessContextsEqual": "10",
    "IsValidDpiAwarenessContext": "10", "GetAwarenessFromDpiAwarenessContext": "10",
    "GetSystemMetricsForDpi": "10", "AdjustWindowRectExForDpi": "10", "EnableNonClientDpiScaling": "10",
    "SystemParametersInfoForDpi": "10", "GetDpiFromDpiAwarenessContext": "10",
}


def read_imports(data):
    """Yields (dll, function) for every named import of a PE file, delay-load ones included."""
    if data[:2] != b"MZ":
        raise ValueError("not a PE file (no MZ)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file (no PE signature)")
    nsec, = struct.unpack_from("<H", data, pe + 6)
    optsize, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    if magic == 0x10B:            # PE32
        ndirs_off, dirs_off, ptr = opt + 92, opt + 96, 4
        image_base, = struct.unpack_from("<I", data, opt + 28)
    elif magic == 0x20B:          # PE32+
        ndirs_off, dirs_off, ptr = opt + 108, opt + 112, 8
        image_base, = struct.unpack_from("<Q", data, opt + 24)
    else:
        raise ValueError("unknown optional header magic 0x%X" % magic)
    ndirs, = struct.unpack_from("<I", data, ndirs_off)
    sections = []
    for i in range(nsec):
        off = opt + optsize + 40 * i
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((va, max(vsize, rawsize), rawptr))

    def rva2off(rva):
        for va, size, rawptr in sections:
            if va <= rva < va + size:
                return rawptr + rva - va
        raise ValueError("RVA 0x%X is outside every section" % rva)

    def cstr(off):
        end = data.index(b"\0", off)
        return data[off:end].decode("latin-1")

    def thunks(rva):
        off, ordinal_flag = rva2off(rva), 1 << (ptr * 8 - 1)
        while True:
            v, = struct.unpack_from("<Q" if ptr == 8 else "<I", data, off)
            if v == 0:
                return
            if not v & ordinal_flag:                         # by name: hint (2 bytes) + name
                yield cstr(rva2off(v & 0x7FFFFFFF) + 2)
            off += ptr

    def directory(index):
        if index >= ndirs:
            return 0, 0
        return struct.unpack_from("<II", data, dirs_off + 8 * index)

    rva, size = directory(1)                                  # import table
    if rva:
        off = rva2off(rva)
        while True:
            oft, _, _, name, ft = struct.unpack_from("<IIIII", data, off)
            if not (oft or name or ft):
                break
            for fn in thunks(oft or ft):
                yield cstr(rva2off(name)), fn
            off += 20
    rva, size = directory(13)                                 # delay-load table
    if rva:
        off = rva2off(rva)
        while True:
            attrs, name, _, _, int_rva, _, _, _ = struct.unpack_from("<IIIIIIII", data, off)
            if not (name or int_rva):
                break
            # bit 0 of the attributes: the fields are RVAs (every modern linker); otherwise virtual addresses
            rva_of = (lambda r: r) if attrs & 1 else (lambda r: r - image_base)
            for fn in thunks(rva_of(int_rva)):
                yield cstr(rva2off(rva_of(name))), fn
            off += 32


def main(paths):
    if not paths:
        print("usage: check_win7_imports.py FILE.exe [FILE.exe ...]")
        return 2
    failed = False
    for path in paths:
        with open(path, "rb") as f:
            data = f.read()
        imports = list(read_imports(data))
        bad = sorted({(dll, fn, NEWER_THAN_WIN7[fn]) for dll, fn in imports if fn in NEWER_THAN_WIN7})
        print("%s: %d imports from %d DLLs" % (path, len(imports), len({d.lower() for d, _ in imports})))
        for dll, fn, ver in bad:
            print("  ERROR: %s imports %s (Windows %s or later)" % (dll, fn, ver))
        failed = failed or bool(bad)
    if failed:
        print("The program would not start on Windows 7. Find what pulls these in (linker /VERBOSE, "
              "dumpbin /imports, or the toolset: MSVC 14.50+ cannot target Windows 7).")
        return 1
    print("OK: nothing newer than Windows 7 among the imports checked.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
