# ppcdis: standalone big-endian PowerPC disassembler built from SheepShaver's
# binutils-derived kpx_cpu/ppc-dis.c
KPX = ../../SheepShaver/src/kpx_cpu

ppcdis: ppcdis.c $(KPX)/ppc-dis.c
	$(CC) -w -O1 -Iinc -I$(KPX) -o $@ ppcdis.c $(KPX)/ppc-dis.c

clean:
	rm -f ppcdis
