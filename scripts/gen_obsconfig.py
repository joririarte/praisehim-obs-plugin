"""Genera obsconfig.h en <obs_src>/libobs/ a partir del template del source de OBS.

obsconfig.h es un archivo generado por CMake durante el build de OBS.
Para compilar un plugin sin compilar OBS completo, este script lo genera
leyendo las versiones del CMakeLists.txt del source clonado y procesando
el template como lo haría configure_file() de CMake.

Uso:
    python scripts/gen_obsconfig.py <ruta/obs-studio>
"""
import re
import os
import sys


# ── Helpers CMake ─────────────────────────────────────────────────────────────

def parse_cmake_var(content, var_name):
    """Extrae el valor de set(VAR valor) o set(VAR "valor")."""
    patterns = [
        rf'set\s*\(\s*{re.escape(var_name)}\s+"([^"]+)"',
        rf'set\s*\(\s*{re.escape(var_name)}\s+([^\s\)]+)',
    ]
    for pattern in patterns:
        m = re.search(pattern, content)
        if m:
            return m.group(1).strip()
    return None


def cmake_is_truthy(val):
    """Reproduce la lógica de if() de CMake: 0/OFF/FALSE/NO/NOTFOUND/"" son falsy."""
    if val is None:
        return False
    return str(val).upper() not in ("0", "OFF", "FALSE", "NO", "NOTFOUND", "")


def substitute_vars(text, subs):
    """Reemplaza @VAR@ por su valor en subs (vacío si no está definido)."""
    for var, val in subs.items():
        text = text.replace(f"@{var}@", str(val) if val is not None else "")
    # Cualquier @VAR@ que quede sin sustituir → cadena vacía
    text = re.sub(r'@[A-Za-z_][A-Za-z0-9_]*@', "", text)
    return text


def process_cmake_template(template, subs):
    """
    Procesa un template de configure_file() de CMake:
      - #cmakedefine01 VAR  → #define VAR 0|1
      - #cmakedefine VAR [RESTO]  → #define VAR [RESTO] | /* #undef VAR */
      - @VAR@  → valor de VAR
    """
    out = []
    for line in template.splitlines(keepends=True):
        stripped = line.strip()

        # #cmakedefine01 VAR
        m = re.match(r'#cmakedefine01\s+(\w+)', stripped)
        if m:
            var = m.group(1)
            bit = "1" if cmake_is_truthy(subs.get(var)) else "0"
            out.append(f"#define {var} {bit}\n")
            continue

        # #cmakedefine VAR [RESTO]
        m = re.match(r'#cmakedefine\s+(\w+)(.*)', stripped)
        if m:
            var = m.group(1)
            rest = m.group(2).strip()
            if cmake_is_truthy(subs.get(var)):
                rest_sub = substitute_vars(rest, subs).strip()
                if rest_sub:
                    out.append(f"#define {var} {rest_sub}\n")
                else:
                    out.append(f"#define {var}\n")
            else:
                out.append(f"/* #undef {var} */\n")
            continue

        # Sustitución normal @VAR@
        out.append(substitute_vars(line, subs))

    return "".join(out)


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    obs_src = sys.argv[1] if len(sys.argv) > 1 else r"D:\a\_temp\obs-studio"

    if not os.path.isdir(obs_src):
        sys.exit(f"ERROR: directorio OBS no encontrado: {obs_src}")

    # ── Leer versión OBS del CMakeLists.txt raíz ──────────────────────────────
    cmake_root_path = os.path.join(obs_src, "CMakeLists.txt")
    with open(cmake_root_path, encoding="utf-8") as f:
        cmake_root = f.read()

    m = re.search(r'project\s*\([^)]*VERSION\s+(\d+)\.(\d+)\.(\d+)', cmake_root)
    if m:
        obs_major, obs_minor, obs_patch = m.group(1), m.group(2), m.group(3)
    else:
        obs_major = parse_cmake_var(cmake_root, "OBS_VERSION_MAJOR") or "32"
        obs_minor = parse_cmake_var(cmake_root, "OBS_VERSION_MINOR") or "1"
        obs_patch = parse_cmake_var(cmake_root, "OBS_VERSION_PATCH") or "2"

    obs_version = f"{obs_major}.{obs_minor}.{obs_patch}"

    # ── Leer LIBOBS_API version ───────────────────────────────────────────────
    api_major = obs_major
    api_minor = "0"
    api_patch = "0"

    for path in [
        os.path.join(obs_src, "libobs", "CMakeLists.txt"),
        cmake_root_path,
    ]:
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            content = f.read()
        v = parse_cmake_var(content, "LIBOBS_API_MAJOR_VER")
        if v:
            api_major = v
            api_minor = parse_cmake_var(content, "LIBOBS_API_MINOR_VER") or "0"
            api_patch = parse_cmake_var(content, "LIBOBS_API_PATCH_VER") or "0"
            break

    # OBS_RELEASE_CANDIDATE y OBS_BETA son 0 en releases estables → cmake falsy
    subs = {
        "OBS_VERSION":            obs_version,
        "OBS_VERSION_CANONICAL":  obs_version,
        "OBS_RELEASE_CANDIDATE":  "0",   # falsy → /* #undef */
        "OBS_BETA":               "0",   # falsy → /* #undef */
        "OBS_BUILD_NUMBER":       "0",
        "LIBOBS_API_MAJOR_VER":   api_major,
        "LIBOBS_API_MINOR_VER":   api_minor,
        "LIBOBS_API_PATCH_VER":   api_patch,
    }

    # ── Buscar template en el source de OBS ───────────────────────────────────
    template = None
    for tp in [
        os.path.join(obs_src, "cmake", "obsconfig.h.in"),
        os.path.join(obs_src, "cmake", "obs-config.h.in"),
        os.path.join(obs_src, "libobs",  "obsconfig.h.in"),
    ]:
        if os.path.exists(tp):
            with open(tp, encoding="utf-8") as f:
                template = f.read()
            print(f"Usando template: {tp}")
            break

    if template:
        content = process_cmake_template(template, subs)
    else:
        print("Template no encontrado, generando desde cero.")
        content = (
            "#pragma once\n\n"
            f'#define OBS_VERSION "{obs_version}"\n'
            f'#define OBS_VERSION_CANONICAL "{obs_version}"\n'
            "/* #undef OBS_RELEASE_CANDIDATE */\n"
            "/* #undef OBS_BETA */\n"
            "#define OBS_BUILD_NUMBER 0\n\n"
            f"#define LIBOBS_API_MAJOR_VER {api_major}\n"
            f"#define LIBOBS_API_MINOR_VER {api_minor}\n"
            f"#define LIBOBS_API_PATCH_VER {api_patch}\n"
            "#define LIBOBS_API_VER"
            " ((LIBOBS_API_MAJOR_VER << 24)"
            " | (LIBOBS_API_MINOR_VER << 16)"
            " | LIBOBS_API_PATCH_VER)\n"
        )

    out_path = os.path.join(obs_src, "libobs", "obsconfig.h")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(content)

    print(f"Generado: {out_path}")
    print(f"  OBS_VERSION       = {obs_version}")
    print(f"  LIBOBS_API_VER    = {api_major}.{api_minor}.{api_patch}")


if __name__ == "__main__":
    main()
