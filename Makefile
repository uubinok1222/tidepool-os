# Makefile - Tidepool OS: x86_64 Multiboot2 + GRUB bootable
#
# Targets:
#   make              -> build/kernel.elf (default)
#   make iso          -> tidepool.iso (bootable ISO)
#   make run          -> chạy QEMU x86_64 with graphical display
#   make run-debug    -> chạy QEMU x86_64 với QEMU monitor console
#   make clean        -> xóa build artifacts
#
# Performance optimization:
# - Kernel: x86_64 long mode, Multiboot2, freestanding C++
# - Renderer: Dirty Rectangles (only redraw changed areas)
# - Memory: ~5.4MB static buffers, no dynamic allocation
# - Size: compact, boot from 1MB

NASM ?= nasm
CXX  ?= g++
LD   ?= ld

BUILD := build
ISO   := tidepool.iso

# Compiler flags: freestanding kernel, no stdlib, no exceptions
# -O2: optimization for speed
# -m64: 64-bit code generation
# -ffreestanding: no libc assumptions
# -fno-exceptions: no C++ exceptions
# -fno-rtti: no runtime type information
# -fno-stack-protector: no stack canary
# -fno-pic/-fno-pie: position-dependent code (kernel runs at 1MB)
# -mno-red-zone: x86 ABI red zone not available in kernel
# -mno-mmx/-mno-sse*: no floating point ops
# -fno-asynchronous-unwind-tables: no unwind info
# -fno-use-cxa-atexit: no C++ exit handlers
# -fno-threadsafe-statics: single-threaded kernel

CXXFLAGS := -std=c++17 -O2 -m64 -Wall -Wextra \
            -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
            -fno-pic -fno-pie -mno-red-zone \
            -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
            -fno-asynchronous-unwind-tables -fno-use-cxa-atexit -fno-threadsafe-statics

# Linker script maps kernel at 1MB (GRUB loads here)
# -nostdlib: no standard library
# -static: static linking only
# -z max-page-size=0x1000: 4KB pages
LDFLAGS  := -nostdlib -static -z max-page-size=0x1000 -T linker.ld

# Default target: build kernel ELF
all: $(BUILD)/kernel.elf

# Create build directory
$(BUILD):
	mkdir -p $(BUILD)

# Assemble boot code (x86_64 assembly)
$(BUILD)/boot.o: boot.asm | $(BUILD)
	$(NASM) -f elf64 $< -o $@

# Compile kernel (C++)
$(BUILD)/kernel.o: kernel.cpp font.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link kernel ELF
$(BUILD)/kernel.elf: $(BUILD)/boot.o $(BUILD)/kernel.o linker.ld
	$(LD) $(LDFLAGS) -o $@ $(BUILD)/boot.o $(BUILD)/kernel.o

# Create bootable ISO via GRUB
iso: $(BUILD)/kernel.elf
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/boot/grub
	cp $(BUILD)/kernel.elf $(BUILD)/iso/boot/kernel.elf
	printf '%s\n' \
	  'set timeout=0' \
	  'set default=0' \
	  'insmod all_video' \
	  '' \
	  'menuentry "Tidepool OS" {' \
	  '  multiboot2 /boot/kernel.elf' \
	  '  boot' \
	  '}' \
	  > $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/iso

# Run in QEMU: x86_64 with graphical display
run: iso
	qemu-system-x86_64 \
	  -cdrom $(ISO) \
	  -m 512M \
	  -smp 1 \
	  -vga std \
	  -display gtk \
	  -rtc base=localtime

# Run in QEMU with debug console (QEMU monitor)
run-debug: iso
	qemu-system-x86_64 \
	  -cdrom $(ISO) \
	  -m 512M \
	  -smp 1 \
	  -vga std \
	  -display gtk \
	  -rtc base=localtime \
	  -monitor stdio

# Clean build artifacts
clean:
	rm -rf $(BUILD) $(ISO)

.PHONY: all iso run run-debug clean
