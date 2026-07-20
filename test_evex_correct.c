#include <stdio.h>
#include <stdint.h>
#include "fadec.h"

int main() {
  uint8_t buf1[] = {0x62, 0xF1, 0x7C, 0x48, 0x58, 0xC0};
  uint8_t buf2[] = {0x62, 0xF2, 0x7C, 0x48, 0xDB, 0xC0};
  uint8_t buf3[] = {0x62, 0x71, 0x7C, 0x48, 0x58, 0xC0};
  FdInstr instr;
  int res;

  printf("Test VADDPS 62 F1 7C 48 58 C0\n");
  res = fd_decode(buf1, sizeof(buf1), 64, 0, &instr);
  if (res > 0)
    printf("  OK decoded %d bytes type=%d\n", res, instr.type);
  else
    printf("  FAILED res=%d\n", res);

  printf("Test 0F38 DB 62 F2 7C 48 DB C0\n");
  res = fd_decode(buf2, sizeof(buf2), 64, 0, &instr);
  if (res > 0)
    printf("  OK decoded %d bytes type=%d\n", res, instr.type);
  else
    printf("  FAILED res=%d\n", res);

  printf("Test VADDPD W1 62 71 7C 48 58 C0\n");
  res = fd_decode(buf3, sizeof(buf3), 64, 0, &instr);
  if (res > 0)
    printf("  OK decoded %d bytes type=%d\n", res, instr.type);
  else
    printf("  FAILED res=%d\n", res);

  return 0;
}
