# ============================================================
# SaylterOS Makefile (bare-metal x86)
# ============================================================

.PHONY: all build run clean

NASM        := nasm
QEMU        := qemu-system-i386
BUILD_DIR   := build
SRC_BOOT    := src/boot

BOOT_BIN    := $(BUILD_DIR)/boot.bin
STAGE2_BIN  := $(BUILD_DIR)/stage2.bin
OS_IMG      := $(BUILD_DIR)/saylter.img

all: build

# --- 编译 Stage 1 (MBR) ---
$(BOOT_BIN): $(SRC_BOOT)/boot.asm
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<
	@echo "[NASM] boot.asm -> boot.bin ($(shell wc -c < $@) bytes)"

# --- 编译 Stage 2 ---
$(STAGE2_BIN): $(SRC_BOOT)/stage2.asm
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<
	@echo "[NASM] stage2.asm -> stage2.bin ($(shell wc -c < $@) bytes)"

# --- 拼接成 .img ---
$(OS_IMG): $(BOOT_BIN) $(STAGE2_BIN)
	cat $(BOOT_BIN) $(STAGE2_BIN) > $@
	@echo "[IMG ] saylter.img built ($(shell wc -c < $@) bytes)"

build: $(OS_IMG)

# --- 用 QEMU 启动 ---
run: build
	$(QEMU) -drive format=raw,file=$(OS_IMG),if=floppy

# --- 清理 ---
clean:
	rm -f $(BOOT_BIN) $(STAGE2_BIN) $(OS_IMG)
	@echo "[CLEAN] build artifacts removed"
