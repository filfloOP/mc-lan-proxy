Target := mc_lan_proxy.sprx
Compiler := clang++
Flags := -O2 -target x86_64-scei-ps4 -fPIC -shared

all:
	$(Compiler) $(Flags) main.cpp -o $(Target)
