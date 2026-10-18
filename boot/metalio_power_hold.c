#include <stdint.h>

// 引导程序在校验应用镜像之前调用。此时只能碰寄存器和 ROM，不能链接 libc。
// TCA9555 上电默认全是输入，松手电源就掉。先写输出锁存，再把保持脚改成输出。

#define ROM_DELAY_US ((void (*)(uint32_t))0x40000600)
#define ROM_PAD_SELECT ((void (*)(uint32_t))0x40001b0c)
#define ROM_PAD_PULLUP ((void (*)(uint32_t))0x40001b00)

#define GPIO_OUT1_W1TC 0x60004018u
#define GPIO_ENABLE1_W1TS 0x60004030u
#define GPIO_ENABLE1_W1TC 0x60004034u
#define GPIO_IN1 0x60004040u

#define PIN_SDA 41u
#define PIN_SCL 42u
#define EXPANDER_WRITE 0x40u
#define REG_OUTPUT 2u
#define REG_CONFIG 6u
// MAIN_POWER | POWER_PULSE | USB_MUX_SEL，与 MetalioEInk4Board.h 的 BOOT_OUTPUT 一致。
#define HOLD_LEVEL 0x0841u

static void write_reg(const uint32_t address, const uint32_t value) { *(volatile uint32_t*)address = value; }

static uint32_t read_reg(const uint32_t address) { return *(volatile uint32_t*)address; }

static uint32_t bank_bit(const uint32_t pin) { return 1u << (pin - 32u); }

static void pause(void) { ROM_DELAY_US(5); }

static void release_line(const uint32_t pin) { write_reg(GPIO_ENABLE1_W1TC, bank_bit(pin)); }

static void drive_low(const uint32_t pin) {
  write_reg(GPIO_OUT1_W1TC, bank_bit(pin));
  write_reg(GPIO_ENABLE1_W1TS, bank_bit(pin));
}

static void set_line(const uint32_t pin, const int released) {
  if (released) release_line(pin);
  else drive_low(pin);
}

static void i2c_start(void) {
  set_line(PIN_SDA, 1);
  set_line(PIN_SCL, 1);
  pause();
  set_line(PIN_SDA, 0);
  pause();
  set_line(PIN_SCL, 0);
  pause();
}

static void i2c_stop(void) {
  set_line(PIN_SDA, 0);
  pause();
  set_line(PIN_SCL, 1);
  pause();
  set_line(PIN_SDA, 1);
  pause();
}

static int write_byte(const uint8_t value) {
  for (int bit = 7; bit >= 0; --bit) {
    set_line(PIN_SDA, (value >> bit) & 1);
    pause();
    set_line(PIN_SCL, 1);
    pause();
    set_line(PIN_SCL, 0);
  }
  set_line(PIN_SDA, 1);
  pause();
  set_line(PIN_SCL, 1);
  pause();
  const int ack = (read_reg(GPIO_IN1) & bank_bit(PIN_SDA)) == 0;
  set_line(PIN_SCL, 0);
  pause();
  return ack;
}

static int write16(const uint8_t reg, const uint16_t value) {
  i2c_start();
  const int ok = write_byte(EXPANDER_WRITE) && write_byte(reg) && write_byte((uint8_t)value) &&
                 write_byte((uint8_t)(value >> 8));
  i2c_stop();
  return ok;
}

__attribute__((section(".text.metalio_boot_hold"), used)) void metalio_boot_hold(void) {
  ROM_PAD_SELECT(PIN_SDA);
  ROM_PAD_SELECT(PIN_SCL);
  ROM_PAD_PULLUP(PIN_SDA);
  ROM_PAD_PULLUP(PIN_SCL);
  release_line(PIN_SDA);
  release_line(PIN_SCL);
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (write16(REG_OUTPUT, HOLD_LEVEL) && write16(REG_CONFIG, (uint16_t)~HOLD_LEVEL)) return;
  }
}
