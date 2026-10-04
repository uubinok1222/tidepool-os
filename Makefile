# Makefile - Tidepool OS: x86_64 Multiboot2 + GRUB
#
# Targets:
#   make             -> build/kernel.elf
#   make iso         -> tidepool.iso (bootable ISO)
#   make run         -> chạy QEMU x86_64 graphical
#   make run-debug   -> chạy QEMU x86_64 với QEMU monitor
#   make clean       -> xóa build artifacts

NASM ?= nasm
CXX  ?= g++
LD   ?= ld

BUILD := build
ISO   := tidepool.iso

# Compiler flags: freestanding, no stdlib, no exceptions, optimize for size
CXXFLAGS := -std=c++17 -O2 -m64 -Wall -Wextra \
            -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
            -fno-pic -fno-pie -mno-red-zone \
            -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
            -fno-asynchronous-unwind-tables -fno-use-cxa-atexit -fno-threadsafe-statics

LDFLAGS  := -nostdlib -static -z max-page-size=0x1000 -T linker.ld

all: $(BUILD)/kernel.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot.asm | $(BUILD)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/kernel.o: kernel.cpp font.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(BUILD)/boot.o $(BUILD)/kernel.o linker.ld
	$(LD) $(LDFLAGS) -o $@ $(BUILD)/boot.o $(BUILD)/kernel.o

iso: $(BUILD)/kernel.elf
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/boot/grub
	cp $(BUILD)/kernel.elf $(BUILD)/iso/boot/kernel.elf
	printf '%s\n' \
	  'set timeout=0' 'set default=0' 'insmod all_video' '' \
	  'menuentry "Tidepool OS" {' '  multiboot2 /boot/kernel.elf' '  boot' '}' \
	  > $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/iso

run: iso
	qemu-system-x86_64 \
	  -cdrom $(ISO) \
	  -m 512M \
	  -smp 1 \
	  -vga std \
	  -display gtk \
	  -rtc base=localtime

run-debug: iso
	qemu-system-x86_64 \
	  -cdrom $(ISO) \
	  -m 512M \
	  -smp 1 \
	  -vga std \
	  -display gtk \
	  -rtc base=localtime \
	  -monitor stdio

clean:
	rm -rf $(BUILD) $(ISO)

.PHONY: all iso run run-debug clean
