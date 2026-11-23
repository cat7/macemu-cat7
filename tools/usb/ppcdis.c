/* ppcdis FILE START END : disassemble big-endian PowerPC code from a raw file */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "dis-asm.h"
int print_insn_big_powerpc(bfd_vma, struct disassemble_info *);
static unsigned char *buf; static long len;
static int rd(bfd_vma a, bfd_byte *m, int n, struct disassemble_info *i)
{ if ((long)(a + n) > len) return -1; for (int k = 0; k < n; k++) m[k] = buf[a + k]; return 0; }
static void err(int s, bfd_vma a, struct disassemble_info *i) { printf("<err %llx>", (unsigned long long)a); }
static void pa(bfd_vma a, struct disassemble_info *i) { printf("%#llx", (unsigned long long)a); }
int main(int c, char **v) {
  FILE *f = fopen(v[1], "rb"); fseek(f, 0, SEEK_END); len = ftell(f); rewind(f);
  buf = malloc(len); fread(buf, 1, len, f);
  long s = strtol(v[2], 0, 0), e = strtol(v[3], 0, 0);
  struct disassemble_info info; INIT_DISASSEMBLE_INFO(info, stdout, (fprintf_function)fprintf);
  info.read_memory_func = rd; info.memory_error_func = err; info.print_address_func = pa;
  info.endian = BFD_ENDIAN_BIG; info.mach = 0;
  for (long a = s; a < e; a += 4) {
    printf("%6lx: %02x%02x%02x%02x  ", a, buf[a], buf[a+1], buf[a+2], buf[a+3]);
    print_insn_big_powerpc(a, &info); printf("\n");
  }
  return 0;
}
