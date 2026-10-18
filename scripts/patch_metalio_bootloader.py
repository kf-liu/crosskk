"""Latch Metalio power inside the second-stage bootloader, before image checksum.

The stock ESP32-S3 bootloader has a null after-init hook. This script compiles
a tiny I2C hold routine into the unused gap before the next IRAM segment and
points that hook at it. A short press then only has to outlast the ROM loading
the bootloader, not the application checksum.
"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HOOK_BYTES = bytes.fromhex("8195ff1658008193ffe00800")
SEGMENT_VADDR = 0x403C8700
HOOK_OFFSET = 4
CALL_SITE_OFFSET = 0x18
PT_LOAD = 1

HOLD_LD = """
ENTRY(metalio_boot_hold)
MEMORY {{ iram (x) : ORIGIN = {origin:#x}, LENGTH = {length:#x} }}
SECTIONS {{
  .text : {{
    KEEP(*(.text.metalio_boot_hold))
    *(.literal .literal.* .text .text.*)
  }} > iram
  .xt.prop 0 : {{ *(.xt.prop*) }}
  .xt.lit 0 : {{ *(.xt.lit*) }}
  .xtensa.info 0 : {{ *(.xtensa.info*) }}
  /DISCARD/ : {{ *(.eh_frame*) *(.comment) *(.note*) *(.debug*) }}
}}
"""


class Elf:
    def __init__(self, path):
        self.path = Path(path)
        self.data = bytearray(self.path.read_bytes())
        if self.data[:4] != b"\x7fELF" or self.data[4] != 1 or self.data[5] != 1:
            raise RuntimeError(f"{self.path} is not a little-endian ELF32")
        self.phoff, self.shoff = struct.unpack_from("<II", self.data, 28)
        self.phentsize, self.phnum = struct.unpack_from("<HH", self.data, 42)
        self.shentsize, self.shnum, self.shstrndx = struct.unpack_from("<HHH", self.data, 46)

    def phdr(self, index):
        off = self.phoff + index * self.phentsize
        return struct.unpack_from("<IIIIIIII", self.data, off)

    def loads(self):
        found = []
        for index in range(self.phnum):
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = self.phdr(index)
            if p_type == PT_LOAD:
                found.append((index, p_offset, p_vaddr, p_filesz, p_memsz))
        return found

    def set_load_size(self, index, filesz, memsz):
        off = self.phoff + index * self.phentsize
        struct.pack_into("<II", self.data, off + 16, filesz, memsz)

    def sections(self):
        found = []
        for index in range(self.shnum):
            off = self.shoff + index * self.shentsize
            sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from("<IIIIII", self.data, off)
            found.append((index, sh_addr, sh_offset, sh_size))
        return found

    def set_section_size(self, index, size):
        off = self.shoff + index * self.shentsize
        struct.pack_into("<I", self.data, off + 20, size)

    def symbol(self, name):
        symtab = None
        strtabs = {}
        for index in range(self.shnum):
            off = self.shoff + index * self.shentsize
            _name, sh_type, _flags, _addr, sh_offset, sh_size, sh_link, _info, _align, sh_entsize = struct.unpack_from(
                "<IIIIIIIIII", self.data, off
            )
            if sh_type == 2:
                symtab = (sh_offset, sh_size, sh_entsize, sh_link)
            elif sh_type == 3:
                strtabs[index] = (sh_offset, sh_size)
        if not symtab or symtab[3] not in strtabs:
            raise RuntimeError(f"{self.path} has no symbol table")
        sym_off, sym_size, sym_entsize, str_index = symtab
        str_off, str_size = strtabs[str_index]
        strings = self.data[str_off : str_off + str_size]
        for cursor in range(0, sym_size, sym_entsize):
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
                "<IIIBBH", self.data, sym_off + cursor
            )
            end = strings.find(b"\0", st_name)
            if strings[st_name:end].decode() == name:
                return st_value, st_size
        raise RuntimeError(f"{self.path} is missing {name}")


def compile_hold(cc, source, origin, length, output):
    link_script = output.with_suffix(".ld")
    link_script.write_text(HOLD_LD.format(origin=origin, length=length), encoding="utf-8")
    subprocess.check_call(
        [
            cc,
            "-Os",
            "-std=c11",
            "-ffreestanding",
            "-fno-builtin",
            "-fno-jump-tables",
            "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables",
            "-mlongcalls",
            "-mtext-section-literals",
            "-nostdlib",
            "-nostartfiles",
            f"-Wl,-T,{link_script}",
            "-o",
            str(output),
            str(source),
        ]
    )


def hold_image(hold_elf):
    elf = Elf(hold_elf)
    for index in range(elf.shnum):
        off = elf.shoff + index * elf.shentsize
        _name, sh_type = struct.unpack_from("<II", elf.data, off)
        if sh_type in (4, 9):
            raise RuntimeError("power-hold routine still has relocations")
    address, _size = elf.symbol("metalio_boot_hold")
    for _index, sh_addr, sh_offset, sh_size in elf.sections():
        if sh_addr <= address < sh_addr + sh_size and sh_size:
            return sh_addr, elf.data[sh_offset : sh_offset + sh_size], address
    raise RuntimeError("power-hold text section was not linked")


def patch_bootloader(stock_path, source, cc, dest_elf):
    stock = Elf(stock_path)
    loads = stock.loads()
    segment = next((item for item in loads if item[2] == SEGMENT_VADDR), None)
    if segment is None:
        raise RuntimeError("bootloader is missing the IRAM segment at 0x403c8700")
    index, file_off, vaddr, filesz, memsz = segment
    later = [item for item in loads if item[1] > file_off]
    if not later:
        raise RuntimeError("bootloader has no segment after the hook IRAM")
    gap_end = min(item[1] for item in later)
    start, _ = stock.symbol("call_start_cpu0")
    site = file_off + (start + CALL_SITE_OFFSET - vaddr)
    if stock.data[site : site + len(HOOK_BYTES)] != HOOK_BYTES:
        raise RuntimeError("bootloader after-init hook site changed; refuse to patch")
    hook_at = file_off + HOOK_OFFSET
    if struct.unpack_from("<I", stock.data, hook_at)[0] != 0:
        raise RuntimeError("bootloader after-init hook is already set")

    origin = (vaddr + filesz + 3) & ~3
    room = gap_end - (file_off + (origin - vaddr))
    if room < 16:
        raise RuntimeError("bootloader IRAM gap is too small for the power hold")
    compile_hold(cc, source, origin, room, dest_elf.with_suffix(".hold.elf"))
    text_addr, text, entry = hold_image(dest_elf.with_suffix(".hold.elf"))
    if text_addr != origin or not (origin <= entry < origin + len(text)):
        raise RuntimeError("power-hold entry is outside the linked gap")
    new_filesz = (origin + len(text)) - vaddr
    if file_off + new_filesz > gap_end:
        raise RuntimeError("power-hold routine does not fit in the bootloader gap")

    patched = bytearray(stock.data)
    patched[file_off + (origin - vaddr) : file_off + new_filesz] = text
    struct.pack_into("<I", patched, hook_at, entry)
    dest = Elf(stock_path)
    dest.data = patched
    dest.set_load_size(index, new_filesz, max(memsz, new_filesz))
    for section_index, sh_addr, _sh_offset, sh_size in dest.sections():
        if sh_addr == vaddr and sh_size == filesz:
            dest.set_section_size(section_index, new_filesz)
    dest_elf.write_bytes(dest.data)
    return entry


def write_bootloader_image(esptool, elf_path, image_path, flash_mode, flash_freq, flash_size, python=None):
    tool = [python or sys.executable, esptool] if str(esptool).endswith(".py") else [esptool]
    subprocess.check_call(
        [
            *tool,
            "--chip",
            "esp32s3",
            "elf2image",
            "--flash-mode",
            flash_mode,
            "--flash-freq",
            flash_freq,
            "--flash-size",
            flash_size,
            "-o",
            str(image_path),
            str(elf_path),
        ]
    )


def install(env):
    if not str(env.get("PIOENV", "")).startswith("metalio_eink4"):
        return

    def _patch(target, source, env):
        stock = Path(str(source[0]))
        image = Path(str(target[0]))
        work = image.parent / "metalio-power-hold"
        work.mkdir(parents=True, exist_ok=True)
        patched = work / "bootloader.elf"
        cc = env.subst("$CC")
        patch_bootloader(stock, Path(env["PROJECT_DIR"]) / "boot" / "metalio_power_hold.c", cc, patched)
        mode = env["__get_board_flash_mode"](env)
        freq = env["__get_board_f_image"](env)
        size = env.BoardConfig().get("upload.flash_size", "16MB")
        esptool = env.subst("$OBJCOPY").strip('"')
        write_bootloader_image(esptool, patched, image, mode, freq, size)

    env.AddPostAction("$BUILD_DIR/bootloader.bin", _patch)
    env.Depends("$BUILD_DIR/bootloader.bin", env["PROJECT_DIR"] + "/boot/metalio_power_hold.c")
    env.Depends("$BUILD_DIR/bootloader.bin", env["PROJECT_DIR"] + "/scripts/patch_metalio_bootloader.py")


try:
    Import("env")  # noqa: F821 -- PlatformIO injects this.
except NameError:
    pass
else:
    install(env)  # noqa: F821
