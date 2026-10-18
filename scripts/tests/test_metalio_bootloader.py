import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "patch_metalio_bootloader.py"
SPEC = importlib.util.spec_from_file_location("patch_metalio_bootloader", SCRIPT)
patcher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(patcher)

STOCK = (
    Path.home()
    / ".platformio/packages/framework-arduinoespressif32-libs/esp32s3/bin/bootloader_dio_80m.elf"
)
CC = Path.home() / ".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc"
ESPTOOL = Path.home() / ".platformio/packages/tool-esptoolpy/esptool.py"
PENV = Path.home() / ".platformio/penv/bin/python"


def boot_output():
    text = (ROOT / "freeink-sdk/libs/hardware/BoardConfig/include/MetalioEInk4Board.h").read_text()
    shifts = {}
    for name in ("MAIN_POWER", "POWER_PULSE", "USB_MUX_SEL"):
        marker = f"constexpr uint16_t {name} = 1u << "
        line = next(item for item in text.splitlines() if marker in item)
        shifts[name] = 1 << int(line.split(marker, 1)[1].split(";", 1)[0].strip())
    return shifts["MAIN_POWER"] | shifts["POWER_PULSE"] | shifts["USB_MUX_SEL"]


class MetalioBootloaderTest(unittest.TestCase):
    def test_hold_level_matches_board_rails(self):
        source = (ROOT / "boot/metalio_power_hold.c").read_text()
        self.assertIn(f"#define HOLD_LEVEL {boot_output():#06x}u", source)

    @unittest.skipUnless(
        STOCK.is_file() and CC.is_file() and ESPTOOL.is_file() and PENV.is_file(), "local Xtensa bootloader tools"
    )
    def test_hook_runs_before_image_checksum(self):
        with tempfile.TemporaryDirectory() as temp:
            patched = Path(temp) / "bootloader.elf"
            image = Path(temp) / "bootloader.bin"
            entry = patcher.patch_bootloader(STOCK, ROOT / "boot/metalio_power_hold.c", str(CC), patched)
            elf = patcher.Elf(patched)
            segment = next(item for item in elf.loads() if item[2] == patcher.SEGMENT_VADDR)
            _index, file_off, vaddr, filesz, _memsz = segment
            hook = struct.unpack_from("<I", elf.data, file_off + patcher.HOOK_OFFSET)[0]
            self.assertEqual(hook, entry)
            self.assertGreaterEqual(entry, vaddr)
            self.assertLess(entry, vaddr + filesz)
            patcher.write_bootloader_image(str(ESPTOOL), patched, image, "dio", "80m", "16MB", python=str(PENV))
            self.assertEqual(image.read_bytes()[0], 0xE9)


if __name__ == "__main__":
    unittest.main()
