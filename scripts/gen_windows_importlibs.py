"""Genera import libraries MSVC (.lib) a partir de los DLLs instalados de OBS."""
import pefile
import subprocess
import glob
import os
import sys

OBS_BIN = r"C:\Program Files\obs-studio\bin\64bit"


def find_lib_exe():
    candidates = glob.glob(
        r"C:\Program Files\Microsoft Visual Studio\**\lib.exe",
        recursive=True,
    )
    for c in candidates:
        if r"HostX64\x64" in c:
            return c
    return None


def gen_importlib(dll_path, lib_path, lib_exe):
    dll_name = os.path.splitext(os.path.basename(dll_path))[0]
    def_path = dll_path.replace(".dll", ".def")

    pe = pefile.PE(dll_path)
    if not hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        print(f"  WARN: {dll_name}.dll no tiene exports, se omite.")
        return

    with open(def_path, "w") as f:
        f.write(f"LIBRARY {dll_name}\nEXPORTS\n")
        for sym in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            if sym.name:
                f.write(f"  {sym.name.decode()}\n")

    result = subprocess.run(
        [lib_exe, f"/def:{def_path}", f"/out:{lib_path}", "/machine:x64", "/nologo"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"  ERROR: lib.exe falló:\n{result.stdout}\n{result.stderr}")
        sys.exit(1)

    print(f"  Generado: {lib_path}")


def main():
    lib_exe = find_lib_exe()
    if not lib_exe:
        sys.exit("ERROR: lib.exe (MSVC) no encontrado en Visual Studio.")
    print(f"Usando: {lib_exe}\n")

    for dll_name in ["obs", "obs-frontend-api"]:
        dll_path = os.path.join(OBS_BIN, dll_name + ".dll")
        lib_path = os.path.join(OBS_BIN, dll_name + ".lib")

        if not os.path.exists(dll_path):
            print(f"WARN: {dll_path} no existe, se omite.")
            continue

        if os.path.exists(lib_path):
            print(f"Ya existe: {lib_path}")
            continue

        print(f"Generando {dll_name}.lib ...")
        gen_importlib(dll_path, lib_path, lib_exe)


if __name__ == "__main__":
    main()
