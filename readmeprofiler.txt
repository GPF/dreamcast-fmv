export EXCLUDE_FUNCS="is_power_of_2,load_header,ring_inc,total_to_unique_frame,wait_exit,init_pvr,psTimer"

export KOS_CFLAGS="-O3 -freorder-blocks-algorithm=simple -fipa-pta -fno-PIC -fno-PIE -fbuiltin -ffp-contract=fast -m4-single -ml -mfsrra -mfsca -ffunction-sections -fdata-sections -matomic-model=soft-imask -ftls-model=local-exec -DDREAMCAST -D__DREAMCAST__=1 -I/opt/toolchains/dc/kos/include -I/opt/toolchains/dc/kos/kernel/arch/dreamcast/include -I/opt/toolchains/dc/kos/addons/include/ -I/opt/toolchains/dc/kos/addons/include/dreamcast -I/opt/toolchains/dc/kos/../kos-ports/include -D_arch_dreamcast -D_arch_sub_pristine -Wall -g -finstrument-functions -finstrument-functions-exclude-function-list=${EXCLUDE_FUNCS}"

export KOS_LDFLAGS="-O3 -freorder-blocks-algorithm=simple -fipa-pta -fno-PIC -fno-PIE -fbuiltin -ffp-contract=fast -m4-single -ml -mfsrra -mfsca -ffunction-sections -fdata-sections -matomic-model=soft-imask -ftls-model=local-exec -DDREAMCAST -D__DREAMCAST__=1 -I/opt/toolchains/dc/kos/include -I/opt/toolchains/dc/kos/kernel/arch/dreamcast/include -I/opt/toolchains/dc/kos/addons/include/ -I/opt/toolchains/dc/kos/addons/include/dreamcast -I/opt/toolchains/dc/kos/../kos-ports/include -D_arch_dreamcast -D_arch_sub_pristine -Wall -g -finstrument-functions -m4-single -ml -Wl,--gc-sections -T/opt/toolchains/dc/kos/utils/ldscripts/shlelf.xc -nostdlib -L/opt/toolchains/dc/kos/lib/dreamcast -L/opt/toolchains/dc/kos/addons/lib/dreamcast -L/opt/toolchains/dc/kos/../kos-ports/lib"

python3 dctrace.py fmv_play.elf 

dot -Tpng graph.dot -o graph.png