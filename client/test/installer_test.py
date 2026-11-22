#!/usr/bin/env python3
"""Runs install/WotLK-ARPG-Setup.ps1 against a made-up Wow.exe and checks what it did.

No game file is involved: the test builds a file that has what the script
looks at (a PE header with the 3.3.5a timestamp, one section, the loader stub
and its DLL name at the right addresses) and random bytes everywhere else, so
that any write outside the 20-byte name field is noticed.

Usage:
    installer_test.py                 use every PowerShell found (powershell, pwsh)
    installer_test.py --shell pwsh    use this one
    installer_test.py --reference     run the same checks against the Python
                                      reference of the installer below instead
                                      (a test of the test; needs no PowerShell)

On Windows the .bat wrappers are tested too.
"""
import argparse
import os
import random
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
INSTALL_DIR = os.path.join(HERE, "..", "install")
SCRIPT = os.path.join(INSTALL_DIR, "WotLK-ARPG-Setup.ps1")

TIMESTAMP = 0x4C2452FE
IMAGE_BASE = 0x00400000
SECTION_RVA = 0x1000
SECTION_RAW = 0x400
FILE_SIZE = 0xE5200

SPLICE_VA = 0x0040B7D0
LOADER_VA = 0x004E5CB0
NAME_VA = 0x004E5CE0
NAME_SIZE = 20

SPLICE = bytes.fromhex("E9DBA40D00909090")
# The loader as awesome_wotlk's patcher writes it, and as St0ny's current patcher does:
# they differ only in how LoadLibraryA is called.
LOADER_HEAD = bytes.fromhex("B801000000A374B4B60068E05C4E00")
LOADER_TAIL = bytes.fromhex("558BECE8A110F2FFE9045BF2FF")
LOADER = LOADER_HEAD + bytes.fromhex("E81C68380083C404") + LOADER_TAIL
LOADER_VIA_IMPORT = LOADER_HEAD + bytes.fromhex("FF1548F29D009090") + LOADER_TAIL
STOCK_SPLICE = bytes.fromhex("558BECE898B5FFFF")
STOCK_LOADER_SITE = bytes.fromhex("558BEC5633F63935")

STOCK_NAME = b"AwesomeWotlkLib.dll"
OUR_NAME = b"WotLK-ARPG.dll"
BACKUP = "Wow.exe.before-WotLK-ARPG"


def offset(va):
    return va - IMAGE_BASE - SECTION_RVA + SECTION_RAW


def name_field(name):
    assert len(name) < NAME_SIZE
    return name + b"\0" * (NAME_SIZE - len(name))


def fake_wow(name=STOCK_NAME, timestamp=TIMESTAMP, loader=LOADER):
    """A file the setup script accepts (or, with other arguments, must refuse)."""
    data = bytearray(random.Random(12340).randbytes(FILE_SIZE))
    data[0:0x400] = bytes(0x400)
    data[0:2] = b"MZ"
    pe = 0x80
    struct.pack_into("<I", data, 0x3C, pe)
    data[pe:pe + 4] = b"PE\0\0"
    # machine, sections, timestamp, symbol table, symbols, optional header size, characteristics
    struct.pack_into("<HHIIIHH", data, pe + 4, 0x014C, 1, timestamp, 0, 0, 0xE0, 0x0102)
    optional = pe + 24
    struct.pack_into("<H", data, optional, 0x010B)
    struct.pack_into("<I", data, optional + 28, IMAGE_BASE)
    section = optional + 0xE0
    data[section:section + 8] = b".text\0\0\0"
    # virtual size, rva, raw size, raw pointer
    struct.pack_into("<IIII", data, section + 8, FILE_SIZE - SECTION_RAW, SECTION_RVA, FILE_SIZE - SECTION_RAW,
                     SECTION_RAW)
    if loader:
        data[offset(SPLICE_VA):offset(SPLICE_VA) + len(SPLICE)] = SPLICE
        data[offset(LOADER_VA):offset(LOADER_VA) + len(loader)] = loader
        data[offset(NAME_VA):offset(NAME_VA) + NAME_SIZE] = name_field(name)
    else:
        data[offset(SPLICE_VA):offset(SPLICE_VA) + len(STOCK_SPLICE)] = STOCK_SPLICE
        data[offset(LOADER_VA):offset(LOADER_VA) + len(STOCK_LOADER_SITE)] = STOCK_LOADER_SITE
    return bytes(data)


def with_name(data, name):
    patched = bytearray(data)
    patched[offset(NAME_VA):offset(NAME_VA) + NAME_SIZE] = name_field(name)
    return bytes(patched)


# ---------------------------------------------------------------------------
# What the setup script is meant to do, in Python. Used by --reference only.
# ---------------------------------------------------------------------------

def reference_setup(game_dir, uninstall=False):
    exe = os.path.join(game_dir, "Wow.exe")
    if not os.path.isfile(exe):
        return 1
    data = open(exe, "rb").read()
    if len(data) < 0x1000 or data[:2] != b"MZ":
        return 1
    pe = struct.unpack_from("<i", data, 0x3C)[0]
    if pe < 0x40 or pe + 0x400 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        return 1
    if struct.unpack_from("<I", data, pe + 8)[0] != TIMESTAMP:
        return 1
    if (data[offset(SPLICE_VA):offset(SPLICE_VA) + 5] != SPLICE[:5]
            or data[offset(LOADER_VA):offset(LOADER_VA) + len(LOADER)] not in (LOADER, LOADER_VIA_IMPORT)):
        return 1
    at = offset(NAME_VA)
    current = data[at:at + NAME_SIZE].split(b"\0")[0]
    wanted, other = (STOCK_NAME, OUR_NAME) if uninstall else (OUR_NAME, STOCK_NAME)
    if current == wanted:
        return 0
    if current != other:
        return 1
    if not uninstall:
        shutil.copyfile(exe, os.path.join(game_dir, BACKUP))
    with open(exe, "r+b") as file:
        file.seek(at)
        file.write(name_field(wanted))
    return 0


# ---------------------------------------------------------------------------
# Running the real thing
# ---------------------------------------------------------------------------

class Runner:
    def __init__(self, shell):
        self.shell = shell  # None: the Python reference
        self.label = shell or "python reference"

    def setup(self, game_dir, uninstall=False, script=SCRIPT, pass_dir=True):
        """Runs the setup script; returns (exit code, output)."""
        if self.shell is None:
            return reference_setup(game_dir, uninstall), ""
        command = [self.shell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script]
        if pass_dir:
            command += ["-GameDir", game_dir]
        if uninstall:
            command += ["-Uninstall"]
        done = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              cwd=tempfile.gettempdir(), timeout=300)
        return done.returncode, done.stdout.decode("utf-8", "replace")


class Results:
    def __init__(self):
        self.checks = 0
        self.failures = 0

    def check(self, ok, what, detail=""):
        self.checks += 1
        if not ok:
            self.failures += 1
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}")
        if not ok and detail:
            for line in detail.rstrip().splitlines():
                print(f"          | {line}")


def read(path):
    with open(path, "rb") as file:
        return file.read()


def write(path, data):
    with open(path, "wb") as file:
        file.write(data)


def differing_ranges(a, b):
    """Byte ranges where two equally long files differ, as text, for failure messages."""
    if len(a) != len(b):
        return f"lengths differ: {len(a)} and {len(b)}"
    ranges = []
    start = None
    for i in range(len(a)):
        if a[i] != b[i]:
            if start is None:
                start = i
        elif start is not None:
            ranges.append((start, i))
            start = None
    if start is not None:
        ranges.append((start, len(a)))
    return ", ".join(f"{s:#x}..{e:#x}" for s, e in ranges[:8]) or "identical"


def game_dir(root, name="WoW Gold Image (test)"):
    path = os.path.join(root, name)
    os.makedirs(path)
    return path


def test_install_and_uninstall(runner, results, root):
    print("install, install again, uninstall, uninstall again")
    folder = game_dir(root)
    exe = os.path.join(folder, "Wow.exe")
    backup = os.path.join(folder, BACKUP)
    original = fake_wow()
    installed = with_name(original, OUR_NAME)
    write(exe, original)

    code, output = runner.setup(folder)
    results.check(code == 0, "install succeeds", output)
    now = read(exe)
    results.check(now == installed, "only the 20-byte name field changed, to WotLK-ARPG.dll",
                  f"differs from the expected file at {differing_ranges(now, installed)}\n{output}")
    results.check(os.path.isfile(backup) and read(backup) == original, "the backup is the file as it was", output)

    code, output = runner.setup(folder)
    results.check(code == 0, "installing again succeeds", output)
    results.check(read(exe) == installed, "and changes nothing")
    results.check(read(backup) == original, "including the backup")

    code, output = runner.setup(folder, uninstall=True)
    results.check(code == 0, "uninstall succeeds", output)
    now = read(exe)
    results.check(now == original, "the file is byte for byte what it was",
                  f"differs from the original at {differing_ranges(now, original)}\n{output}")

    code, output = runner.setup(folder, uninstall=True)
    results.check(code == 0, "uninstalling again succeeds", output)
    results.check(read(exe) == original, "and changes nothing")


def test_other_loader_form(runner, results, root):
    print("a loader written by St0ny's current patcher (calls LoadLibraryA through the import table)")
    folder = game_dir(root, "import form")
    exe = os.path.join(folder, "Wow.exe")
    original = fake_wow(loader=LOADER_VIA_IMPORT)
    write(exe, original)
    code, output = runner.setup(folder)
    results.check(code == 0 and read(exe) == with_name(original, OUR_NAME), "install", output)
    code, output = runner.setup(folder, uninstall=True)
    results.check(code == 0 and read(exe) == original, "uninstall", output)


def test_refusals(runner, results, root):
    print("files it must refuse")
    damaged = bytearray(LOADER)
    damaged[len(LOADER_HEAD)] = 0x90  # neither of the two known ways to call LoadLibraryA
    cases = [
        ("another build of the game", fake_wow(timestamp=TIMESTAMP + 1)),
        ("a Wow.exe without the loader", fake_wow(loader=None)),
        ("a loader of an unknown shape", fake_wow(loader=bytes(damaged))),
        ("a loader that loads some other DLL", fake_wow(name=b"SomethingElse.dll")),
        ("a file that is not an executable", b"not an executable at all " * 400),
        ("a truncated file", fake_wow()[:0x800]),
    ]
    for index, (what, data) in enumerate(cases):
        for uninstall in (False, True):
            folder = game_dir(root, f"refuse {index} {'uninstall' if uninstall else 'install'}")
            exe = os.path.join(folder, "Wow.exe")
            write(exe, data)
            code, output = runner.setup(folder, uninstall=uninstall)
            mode = "uninstall" if uninstall else "install"
            results.check(code == 1, f"{what}: {mode} refuses", f"exit code {code}\n{output}")
            results.check(read(exe) == data, f"{what}: the file is untouched")
            results.check(not os.path.exists(os.path.join(folder, BACKUP)), f"{what}: no backup is left behind")

    folder = game_dir(root, "empty")
    code, output = runner.setup(folder)
    results.check(code == 1, "a folder without Wow.exe: refuses", f"exit code {code}\n{output}")
    code, output = runner.setup(os.path.join(root, "no such folder"))
    results.check(code == 1, "a folder that does not exist: refuses", f"exit code {code}\n{output}")


def test_awkward_paths(runner, results, root):
    print("folder names PowerShell could trip over")
    for name in ("brackets [x] here", "it's & 100% $HOME", "deep/er path"):
        folder = game_dir(root, name)
        exe = os.path.join(folder, "Wow.exe")
        original = fake_wow()
        write(exe, original)
        code, output = runner.setup(folder)
        results.check(code == 0 and read(exe) == with_name(original, OUR_NAME), f"install in '{name}'", output)
        code, output = runner.setup(folder, uninstall=True)
        results.check(code == 0 and read(exe) == original, f"uninstall in '{name}'", output)


def test_read_only(runner, results, root):
    if os.name != "nt" or runner.shell is None:
        return
    print("a read-only Wow.exe")
    folder = game_dir(root, "read only")
    exe = os.path.join(folder, "Wow.exe")
    original = fake_wow()
    write(exe, original)
    os.chmod(exe, stat.S_IREAD)
    code, output = runner.setup(folder)
    results.check(code == 1, "install fails cleanly", f"exit code {code}\n{output}")
    results.check(read(exe) == original, "and the file is untouched")
    os.chmod(exe, stat.S_IREAD | stat.S_IWRITE)
    backup = os.path.join(folder, BACKUP)
    if os.path.exists(backup):
        os.chmod(backup, stat.S_IREAD | stat.S_IWRITE)


def test_next_to_the_game(runner, results, root):
    """The way it is really used: the scripts sit next to Wow.exe."""
    if runner.shell is None:
        return
    print("scripts copied next to Wow.exe, no -GameDir")
    folder = game_dir(root, "WoW Working Folder")
    exe = os.path.join(folder, "Wow.exe")
    original = fake_wow()
    installed = with_name(original, OUR_NAME)
    write(exe, original)
    for name in os.listdir(INSTALL_DIR):
        shutil.copy(os.path.join(INSTALL_DIR, name), folder)
    script = os.path.join(folder, "WotLK-ARPG-Setup.ps1")

    code, output = runner.setup(folder, script=script, pass_dir=False)
    results.check(code == 0 and read(exe) == installed, "install finds Wow.exe beside the script", output)
    code, output = runner.setup(folder, uninstall=True, script=script, pass_dir=False)
    results.check(code == 0 and read(exe) == original, "uninstall too", output)

    if os.name == "nt" and runner.shell.lower().startswith("powershell"):
        print("the .bat wrappers (double-click)")
        for batch, expected, what in (("WotLK-ARPG-Install.bat", installed, "Install.bat installs"),
                                      ("WotLK-ARPG-Uninstall.bat", original, "Uninstall.bat uninstalls")):
            done = subprocess.run(["cmd", "/c", os.path.join(folder, batch)], stdin=subprocess.DEVNULL,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=tempfile.gettempdir(),
                                  timeout=300)
            output = done.stdout.decode("utf-8", "replace")
            results.check(read(exe) == expected, what, output)
            results.check("Done." in output and "CategoryInfo" not in output, "and reports success without errors",
                          output)


def run_all(runner):
    print(f"== setup script, run with: {runner.label} ==")
    results = Results()
    root = tempfile.mkdtemp(prefix="wotlk-arpg-setup-test-")
    try:
        test_install_and_uninstall(runner, results, root)
        test_other_loader_form(runner, results, root)
        test_refusals(runner, results, root)
        test_awkward_paths(runner, results, root)
        test_read_only(runner, results, root)
        test_next_to_the_game(runner, results, root)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print(f"{results.checks} checks, {results.failures} failed\n")
    return results.failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--shell", action="append", help="PowerShell executable to use (repeatable)")
    parser.add_argument("--reference", action="store_true", help="check the Python reference instead")
    arguments = parser.parse_args()

    if arguments.reference:
        runners = [Runner(None)]
    else:
        shells = arguments.shell or [s for s in ("powershell", "pwsh") if shutil.which(s)]
        if not shells:
            print("No PowerShell found (looked for powershell and pwsh). Nothing was tested.")
            return 2
        runners = [Runner(shell) for shell in shells]

    failures = sum(run_all(runner) for runner in runners)
    print("all setup script checks passed" if failures == 0 else f"{failures} setup script checks FAILED")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
