cmd_/home/zzzccc/MyScull/scull.ko := ld -r -m elf_x86_64  -z max-page-size=0x200000 -z noexecstack   --build-id  -T ./scripts/module-common.lds -o /home/zzzccc/MyScull/scull.ko /home/zzzccc/MyScull/scull.o /home/zzzccc/MyScull/scull.mod.o;  true
