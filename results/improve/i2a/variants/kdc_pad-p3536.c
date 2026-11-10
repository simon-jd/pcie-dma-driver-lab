// SPDX-License-Identifier: GPL-2.0
/* Layout control for timing builds: 3536 bytes of text after the driver, never executed. */
asm(".pushsection .text\n\t.balign 16\n\t.skip 3536, 0xcc\n\t.popsection");
