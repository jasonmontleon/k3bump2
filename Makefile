# Cross build with clang + lld (any riscv64 toolchain works: the program is freestanding).
CC = clang --target=riscv64-linux-gnu
CFLAGS = -march=rv64gc -mabi=lp64d -mno-relax -O1 -ffreestanding -fno-pic -fno-stack-protector -Wall -Wextra
LDFLAGS = -nostdlib -static -fuse-ld=lld -Wl,--no-relax -Wl,-z,norelro -Wl,--build-id=none
k3bump2: k3bump2.c k3bump2_asm.S snap.h snap.bin extra.h extra.bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ k3bump2.c k3bump2_asm.S
# snap.h/snap.bin: pages and registers from the memory dump;  extra.h/extra.bin: python3 mkextra.py DUMPDIR LIBRARY
