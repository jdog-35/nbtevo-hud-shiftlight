#!/bin/zsh
# Builds the head-unit tools into out/ (ARMv7, QNX 6.5, freestanding LD_PRELOAD libraries), with clang and ld.lld only,
# the way st1ng's nbtevo-carplay-advanced build.sh does. Empty stand-ins give the outputs the DT_NEEDED entries of the
# real libraries; the symbols resolve on the head unit at run time.
set -e
cd "$(dirname "$0")"
CF=(-target armv7-none-eabi -march=armv7-a -mfloat-abi=soft -ffreestanding -fPIC -Os -mno-unaligned-access
    -fvisibility=hidden -fno-unwind-tables -fno-asynchronous-unwind-tables -nostdlib
    -Wall -Wextra -Werror -Wno-incompatible-library-redeclaration -Wno-builtin-requires-header)
LLD=${LLD:-$(command -v ld.lld)} || { echo "ld.lld not found (brew install lld)"; exit 1; }
LD=($LLD -shared -z max-page-size=4096 -z common-page-size=4096 -z norelro -z noseparate-code --no-rosegment
    --hash-style=sysv --no-eh-frame-hdr -z nognustack --allow-shlib-undefined)
mkdir -p out/stubs
echo > out/stubs/empty.c
clang "${CF[@]}" -x c -c out/stubs/empty.c -o out/stubs/empty.o
"${LD[@]}" -soname libc.so.3 -o out/stubs/libc.so.3 out/stubs/empty.o
"${LD[@]}" -soname libsocket.so.3 -o out/stubs/libsocket.so.3 out/stubs/empty.o

# libslrate.so: the MOST rpm rate test. libsocket is linked on purpose: the DSI service broker uses it without listing it.
clang "${CF[@]}" -c src/slrate.c -o out/slrate.o
"${LD[@]}" -soname libslrate.so -o out/libslrate.so out/slrate.o -Lout/stubs -l:libsocket.so.3 -l:libc.so.3

# Every import must be a libc.so.3 function we declared; anything else (an __aeabi_ helper, a division routine) would
# only fail on the head unit.
ALLOW=" dlopen dlsym dlerror pthread_create usleep clock_gettime write snprintf getenv memcpy memset _exit "
bad=0
for s in $(nm -D -u out/libslrate.so | awk '{print $NF}'); do
  [[ "$ALLOW" == *" $s "* ]] || { echo "unexpected import: $s"; bad=1; }
done
exported=$(nm -D --defined-only out/libslrate.so | awk '{print $NF}' | grep -v -E '^(_end|_edata|__bss_start)$' || true)
[ -z "$exported" ] || { echo "unexpected exports:"; echo "$exported"; bad=1; }
[ $bad = 0 ] || exit 1
ls -l out/libslrate.so
shasum -a 256 out/libslrate.so

# libshiftlight.so: the step 4 proof of concept (modes bar, clear, trace)
OBJS=()
for s in qnx dsi hud logic stages gate util shiftlight; do
  clang "${CF[@]}" -c src/$s.c -o out/$s.o
  OBJS+=(out/$s.o)
done
"${LD[@]}" -soname libshiftlight.so -o out/libshiftlight.so "${OBJS[@]}" -Lout/stubs -l:libsocket.so.3 -l:libc.so.3
ALLOW2="$ALLOW read open close signal kill getpid unlink rename time "
for s in $(nm -D -u out/libshiftlight.so | awk '{print $NF}'); do
  [[ "$ALLOW2" == *" $s "* ]] || { echo "unexpected import in libshiftlight.so: $s"; bad=1; }
done
exported=$(nm -D --defined-only out/libshiftlight.so | awk '{print $NF}' | grep -v -E '^(_end|_edata|__bss_start)$' || true)
[ -z "$exported" ] || { echo "unexpected exports in libshiftlight.so:"; echo "$exported"; bad=1; }
[ $bad = 0 ] || exit 1
ls -l out/libshiftlight.so
shasum -a 256 out/libshiftlight.so

# libShiftLight.so: the My Vehicle menu plugin (step 6), C++ like st1ng's libCarPlayMenu.so. The HMI frameworks are linked
# by soname only (stand-ins that define the bmw:: symbols the plugin imports, as st1ng's build.sh makes them); the real
# ones resolve them when HmiMain3D loads the plugin. PluginApp::post and ::cdsAdd... are looked up with dlsym at run time.
CXF=(-target armv7-none-eabi -march=armv7-a -mfloat-abi=soft -ffreestanding -fPIC -Os -mno-unaligned-access
     -fno-exceptions -fno-rtti -nostdlib -fvisibility=hidden -fno-unwind-tables -fno-asynchronous-unwind-tables
     -Wall -Wextra -Werror -Wno-incompatible-library-redeclaration -Wno-unused-parameter)
clang++ "${CXF[@]}" -x c++ -c src/slplugin.cpp -o out/slplugin.o
printf '.data\n.globl __exidx_end, __bss_start, _end, _edata, __exidx_start\n__exidx_end: __bss_start: _end: _edata: __exidx_start: .word 0\n.text\n' > out/stubs/fw.s
for s in $(nm -u out/slplugin.o | awk '{print $NF}' | grep '^_ZN3bmw'); do printf '.globl %s\n.type %s,%%function\n%s: bx lr\n' $s $s $s >> out/stubs/fw.s; done
clang -target armv7-none-eabi -march=armv7-a -mfloat-abi=soft -c out/stubs/fw.s -o out/stubs/fw.o
"${LD[@]}" -soname libAppFramework.A.so -o out/stubs/libAppFramework.A.so out/stubs/fw.o
"${LD[@]}" -soname libCoreFramework.A.so -o out/stubs/libCoreFramework.A.so out/stubs/empty.o
mkdir -p out/plugin  # not out/: the Mac's file system ignores case, and out/libshiftlight.so is the worker
"${LD[@]}" -soname libShiftLight.so -o out/plugin/libShiftLight.so out/slplugin.o -Lout/stubs -l:libAppFramework.A.so \
  -l:libCoreFramework.A.so -l:libc.so.3
# imports: the libc functions slplugin.cpp declares, and the five framework symbols st1ng's plugin uses on this car
ALLOW3=" malloc free memcpy memset open read write close unlink kill snprintf spawn waitpid sigfillset sigemptyset __get_errno_ptr usleep pthread_create dlopen dlsym clock_gettime "
FW3=" _ZN3bmw11BasicStringIcEC1EPKc _ZN3bmw11BasicStringIcED1Ev _ZN3bmw9PluginApp10addHandlerEjRKNS_8DelegateIbRKNS_6ParcelEvvvEE _ZN3bmw9PluginApp11updateModelEjb _ZN3bmw9PluginApp6onInitEv _ZN3bmw9PluginAppC1ERNS_11IDispatcherENS_11BasicStringIcEEjjNS_20RHMIInterfaceVersionERKNS_6BundleES8_ "
for s in $(nm -D -u out/plugin/libShiftLight.so | awk '{print $NF}'); do
  [[ "$ALLOW3$FW3" == *" $s "* ]] || { echo "unexpected import in libShiftLight.so: $s"; bad=1; }
done
exported=$(nm -D --defined-only out/plugin/libShiftLight.so | awk '{print $NF}' | grep -v -E '^(_end|_edata|__bss_start|__exidx_start|__exidx_end)$' || true)
[ "$exported" = "pluginAppInfo" ] || { echo "libShiftLight.so must export only pluginAppInfo:"; echo "$exported"; bad=1; }
[ $bad = 0 ] || exit 1
cp plugin/ShiftLight.manifest plugin/ShiftLight.xml out/plugin/
ls -l out/plugin/libShiftLight.so
shasum -a 256 out/plugin/libShiftLight.so out/plugin/ShiftLight.manifest out/plugin/ShiftLight.xml
nm -D -u out/libshiftlight.so | grep -q _ZN3bmw && { echo "out/libshiftlight.so is not the worker"; exit 1; }
python3 -I tests/test_plugin.py plugin/ShiftLight.xml plugin/ShiftLight.manifest src/slplugin_ids.h src/slplugin.cpp | tail -1

# host tests of the pure modules (the Mac's own compiler and libc)
cc -O1 -Wall -Wextra -Werror -Isrc tests/test_pure.c src/hud.c src/logic.c src/util.c -o out/test_pure
out/test_pure tests/data/obd-rev-blips.txt | tail -1
cc -O1 -Wall -Wextra -Werror -Isrc tests/test_gate.c src/gate.c -o out/test_gate
out/test_gate | tail -1
cc -O1 -Wall -Wextra -Werror -Isrc tests/test_stages.c src/stages.c src/hud.c src/logic.c src/util.c -o out/test_stages
out/test_stages tests/data/cd0-broadcast.txt tests/data/drive-2026-11-19.txt | tail -1
