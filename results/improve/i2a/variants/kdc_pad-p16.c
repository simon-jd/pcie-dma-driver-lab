// SPDX-License-Identifier: GPL-2.0
/* Layout control for timing builds: 16 bytes of text after the driver, never executed. */
asm(".pushsection .text\n\t.balign 16\n\t.skip 16, 0xcc\n\t.popsection");
