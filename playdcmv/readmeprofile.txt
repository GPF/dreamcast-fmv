
export KOS_CFLAGS='-O0 -g -finstrument-functions  -fno-inline -fno-optimize-sibling-calls -fno-omit-frame-pointer -m4-single -ml -mfsrra -mfsca -ffunction-sections -fdata-sections -matomic-model=soft-imask -ftls-model=local-exec -D__DREAMCAST__ -I/opt/toolchains/dc/kos/include -I/opt/toolchains/dc/kos/kernel/arch/dreamcast/include -I/opt/toolchains/dc/kos/addons/include -I/opt/toolchains/dc/kos/../kos-ports/include -D_arch_dreamcast -D_arch_sub_pristine -Wall'

export KOS_LDFLAGS='-O0 -g -finstrument-functions  -fno-optimize-sibling-calls -fno-inline -fno-omit-frame-pointer -m4-single -ml -mfsrra -mfsca -ffunction-sections -fdata-sections -matomic-model=soft-imask -ftls-model=local-exec -Wl,--gc-sections -T/opt/toolchains/dc/kos/utils/ldscripts/shlelf.xc -nodefaultlibs -L/opt/toolchains/dc/kos/lib/dreamcast -L/opt/toolchains/dc/kos/addons/lib/dreamcast -L/opt/toolchains/dc/kos/../kos-ports/lib -D__DREAMCAST__ -I/opt/toolchains/dc/kos/include -I/opt/toolchains/dc/kos/kernel/arch/dreamcast/include -I/opt/toolchains/dc/kos/addons/include -I/opt/toolchains/dc/kos/../kos-ports/include -D_arch_dreamcast -D_arch_sub_pristine -Wall'

python3 dctrace.py fmv_play.elf 

dot -Tpng graph.dot -o graph.png
