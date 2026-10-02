OO_PS4_TOOLCHAIN ?= $(HOME)/OO_PS4_TOOLCHAIN

TARGET := mc_lan_proxy

CXX := clang++
LD  := ld.lld

CXXFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 \
            -fno-exceptions -fno-rtti -std=c++17 -c \
            -isysroot $(OO_PS4_TOOLCHAIN) \
            -isystem $(OO_PS4_TOOLCHAIN)/include \
            -isystem $(OO_PS4_TOOLCHAIN)/include/c++/v1

LDFLAGS := -m elf_x86_64 -pie --script $(OO_PS4_TOOLCHAIN)/link.x \
           --eh-frame-hdr --allow-multiple-definition \
           -L$(OO_PS4_TOOLCHAIN)/lib

LIBS := -lc -lc++ -lkernel -lSceNet

# Utilise crtprx.o s'il existe, sinon crtlib.o
CRT := $(firstword $(wildcard $(OO_PS4_TOOLCHAIN)/lib/crtprx.o $(OO_PS4_TOOLCHAIN)/lib/crtlib.o))

all: $(TARGET).sprx

main.o: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o main.o

# main.o AVANT le crt : nos module_start/module_stop sont prioritaires
$(TARGET).oelf: main.o
	@echo "CRT utilise : $(CRT)"
	$(LD) $(LDFLAGS) main.o $(CRT) $(LIBS) -o $(TARGET).oelf

$(TARGET).sprx: $(TARGET).oelf
	$(OO_PS4_TOOLCHAIN)/bin/linux/create-fself -in=$(TARGET).oelf \
	  --out=$(TARGET).out --lib=$(TARGET).sprx --paid 0x3800000000000011

clean:
	rm -f *.o *.oelf *.out *.sprx
