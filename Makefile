OO_PS4_TOOLCHAIN ?= $(HOME)/OO_PS4_TOOLCHAIN

Target := mc_lan_proxy.sprx
Compiler := clang++
Flags := -O2 -target x86_64-scei-ps4 -fPIC -shared \
         -I$(OO_PS4_TOOLCHAIN)/include \
         -I$(OO_PS4_TOOLCHAIN)/include/c++/v1 \
         -L$(OO_PS4_TOOLCHAIN)/lib \
         -lSceLibcInternal -lkernel -lSceNet

all:
	$(Compiler) $(Flags) main.cpp -o $(Target)
