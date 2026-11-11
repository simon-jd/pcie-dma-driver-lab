## Claims

| where | claim | arm | s1 | s2 | holds |
|---|---|---|---|---|---|
| README | 4 KiB one at a time 0.99x | `zc4k-qd1` | 0.994 (2/7) yes | 0.991 (2/7) yes | yes |
| README | NOP one at a time unchanged (176.1k vs 175.4k) | `nop-qd1` | 0.998 (2/7) yes | 1.004 (4/7) yes | yes |
| README | 32 in flight 1.10x | `zc4k-qd32` | 1.075 (7/7) yes | 1.106 (7/7) yes | yes |
| README | 32 batched 1.02x | `zc4k-qd32-batch` | 1.020 (6/7) yes | 1.019 (6/7) yes | yes |
| README | three threads 1.00x (iE), 0.98x (iD) | `zc4k-t3-qd8` | 0.990 (2/7) yes | 0.988 (3/7) yes | yes |
| README | three threads batched 0.99x | `zc4k-t3-qd8-batch` | 0.991 (1/7) yes | 0.987 (3/7) yes | yes |
| README | one file NOP 1.39x | `nop-t3-qd8-sharedfile` | 1.431 (7/7) yes | 1.448 (7/7) yes | yes |
| README | one file 4 KiB 1.28x | `zc4k-t3-qd8-sharedfile` | 1.253 (7/7) yes | 1.289 (7/7) yes | yes |
| README | one IOThread: NOP one at a time 1.21x | `nop-qd1` | 1.194 (7/7) yes | 1.217 (7/7) yes | yes |
| README | one IOThread: 4 KiB one at a time 1.17x | `zc4k-qd1` | 1.159 (7/7) yes | 1.167 (7/7) yes | yes |
| README | one IOThread: 32 in flight 0.93x | `zc4k-qd32` | 0.916 (0/7) yes | 0.936 (1/7) yes | yes |
| README | one IOThread: 32 batched 1.07x | `zc4k-qd32-batch` | 1.048 (7/7) yes | 1.043 (7/7) yes | yes |
| README | one IOThread: three threads 1.38x | `zc4k-t3-qd8` | 1.356 (7/7) yes | 1.357 (7/7) yes | yes |
| README | one IOThread: three threads batched 1.50x | `zc4k-t3-qd8-batch` | 1.487 (7/7) yes | 1.480 (7/7) yes | yes |
| README | one IOThread: one file NOP 1.10x | `nop-t3-qd8-sharedfile` | 1.173 (7/7) size differs | 1.161 (7/7) size differs | size differs |
| README | one IOThread: one file 4 KiB 1.07x | `zc4k-t3-qd8-sharedfile` | 1.095 (7/7) yes | 1.088 (7/7) yes | yes |
| README | strict IOMMU, fixed 4 KiB one at a time 1.00x | `fixed4k-qd1` | 1.006 (4/7) yes | 0.988 (3/7) yes | yes |
| README | strict IOMMU, fixed 4 KiB 32 batched 1.01x | `fixed4k-qd32-batch` | 1.033 (5/7) no | 1.016 (6/7) yes | no |
| README | loss: bounce 4 KiB one at a time 0.99x (iDfixed) | `bounce4k-qd1` | 0.988 (3/7) yes | 1.008 (4/7) no | no |
| design 2 | r1 vs o3r at 32 in flight 1.004, 1.002 | `zc4k-qd32` | 0.984 (2/7) yes | 0.992 (1/7) yes | yes |
| design 2 | r1 ties o3r within 0.5%: 4 KiB one at a time | `zc4k-qd1` | 1.001 (4/7) yes | 1.003 (4/7) yes | yes |
| design 2 | r1 ties o3r within 0.5%: 32 batched | `zc4k-qd32-batch` | 1.011 (5/7) yes | 0.989 (1/7) yes | yes |
| design 2 | r1 ties o3r within 0.5%: three threads | `zc4k-t3-qd8` | 0.998 (2/7) yes | 1.005 (4/7) yes | yes |
| design 2 | r1 vs o3r one file NOP -3.2% (i2b) | `nop-t3-qd8-sharedfile` | 0.994 (3/7) no | 1.004 (5/7) no | no |
| design 2 | r1 vs o3r one file 4 KiB +1.5% (i2b) | `zc4k-t3-qd8-sharedfile` | 1.000 (4/7) yes | 1.013 (4/7) yes | yes |
| design 2 | r1 vs base 32 in flight 1.015, 1.008 (5 of 7) | `zc4k-qd32` | 1.005 (4/7) yes | 1.020 (4/7) yes | yes |
| design 2 | r1 vs base one file 4 KiB 1.034, 1.022 (7 of 7) | `zc4k-t3-qd8-sharedfile` | 1.020 (5/7) no | 1.024 (6/7) yes | no |
| design 4 | one IOThread vs main loop: NOP one at a time 1.210 | `nop-qd1` | 1.195 (7/7) yes | 1.213 (7/7) yes | yes |
| design 4 | one IOThread vs main loop: 4 KiB one at a time 1.178 | `zc4k-qd1` | 1.176 (7/7) yes | 1.173 (7/7) yes | yes |
| design 4 | one IOThread vs main loop: 32 in flight 0.846 (0.86x and 0.85x) | `zc4k-qd32` | 0.862 (0/7) yes | 0.850 (0/7) yes | yes |
| design 4 | one IOThread vs main loop: 32 batched 1.020 | `zc4k-qd32-batch` | 1.018 (6/7) yes | 1.028 (6/7) yes | yes |
| design 4 | one IOThread vs main loop: three threads 1.342 (1.36x, 1.34x) | `zc4k-t3-qd8` | 1.364 (7/7) yes | 1.360 (7/7) yes | yes |
| design 4 | one IOThread vs main loop: three threads batched 1.516 | `zc4k-t3-qd8-batch` | 1.521 (7/7) yes | 1.501 (7/7) yes | yes |
| design 4 | one IOThread vs main loop: one file NOP 0.790 | `nop-t3-qd8-sharedfile` | 0.802 (0/7) yes | 0.798 (0/7) yes | yes |
| design 4 | one IOThread vs main loop: one file 4 KiB 0.837 (0.880 in iD) | `zc4k-t3-qd8-sharedfile` | 0.860 (0/7) yes | 0.844 (0/7) yes | yes |
| design 4 | three IOThreads vs main loop: NOP one at a time 1.001 | `nop-qd1` | 1.000 (4/7) yes | 1.007 (6/7) yes | yes |
| design 4 | three IOThreads vs main loop: 4 KiB one at a time 1.011 | `zc4k-qd1` | 1.003 (5/7) yes | 1.002 (4/7) yes | yes |
| design 4 | three IOThreads vs main loop: 32 in flight 0.573 | `zc4k-qd32` | 0.583 (0/7) yes | 0.573 (0/7) yes | yes |
| design 4 | three IOThreads vs main loop: 32 batched 0.997 | `zc4k-qd32-batch` | 0.977 (1/7) yes | 0.986 (1/7) yes | yes |
| design 4 | three IOThreads vs main loop: three threads 0.869 (lost 13%) | `zc4k-t3-qd8` | 0.862 (0/7) yes | 0.861 (0/7) yes | yes |
| design 4 | three IOThreads vs main loop: three threads batched 1.380 | `zc4k-t3-qd8-batch` | 1.380 (7/7) yes | 1.361 (7/7) yes | yes |
| design 4 | three IOThreads vs main loop: one file NOP 0.720 | `nop-t3-qd8-sharedfile` | 0.711 (0/7) yes | 0.702 (0/7) yes | yes |
| design 4 | three IOThreads vs main loop: one file 4 KiB 0.734 | `zc4k-t3-qd8-sharedfile` | 0.740 (0/7) yes | 0.726 (0/7) yes | yes |
| design 5 | strict IOMMU fixed 4 KiB 32 batched 1.048 (iCfixed), 1.012 (iDfixed) | `fixed4k-qd32-batch` | 1.033 (5/7) no | 1.016 (6/7) yes | no |
| design 5 | before/after 32 in flight 1.10x and 1.08x | `zc4k-qd32` | 1.075 (7/7) yes | 1.106 (7/7) yes | yes |
| design 5 | before/after one file NOP 1.39x and 1.45x | `nop-t3-qd8-sharedfile` | 1.431 (7/7) yes | 1.448 (7/7) yes | yes |
| design 5 | before/after one file 4 KiB 1.28x and 1.26x | `zc4k-t3-qd8-sharedfile` | 1.253 (7/7) yes | 1.289 (7/7) yes | yes |
| design 5 | one IOThread: three threads 1.38x | `zc4k-t3-qd8` | 1.356 (7/7) yes | 1.357 (7/7) yes | yes |
| design 5 | one IOThread: batched three threads 1.50x | `zc4k-t3-qd8-batch` | 1.487 (7/7) yes | 1.480 (7/7) yes | yes |
| design 5 | one IOThread: 32 in flight 0.93x | `zc4k-qd32` | 0.916 (0/7) yes | 0.936 (1/7) yes | yes |
| design 5 | one at a time and three threads on own files unchanged (0.98 to 1.01) | `nop-qd1` | 0.998 (2/7) yes | 1.004 (4/7) yes | yes |
| design 5 | one at a time and three threads on own files unchanged (0.98 to 1.01) | `zc4k-qd1` | 0.994 (2/7) yes | 0.991 (2/7) yes | yes |
| design 5 | one at a time and three threads on own files unchanged (0.98 to 1.01) | `bounce4k-qd1` | 1.002 (4/7) yes | 1.009 (4/7) yes | yes |
| design 5 | one at a time and three threads on own files unchanged (0.98 to 1.01) | `zc64k-qd1` | 0.962 (2/7) yes | 0.961 (1/7) yes | yes; a loss by rule 5 |
| design 5 | one at a time and three threads on own files unchanged (0.98 to 1.01) | `zc4k-t3-qd8` | 0.990 (2/7) yes | 0.988 (3/7) yes | yes |
| design 5 | one at a time and three threads on own files unchanged (0.98 to 1.01) | `zc4k-t3-qd8-batch` | 0.991 (1/7) yes | 0.987 (3/7) yes | yes |
| design 5 | one IOThread vs main: one at a time 1.17x-1.21x | `nop-qd1` | 1.194 (7/7) yes | 1.217 (7/7) yes | yes |
| design 5 | one IOThread vs main: one at a time 1.17x-1.21x | `zc4k-qd1` | 1.159 (7/7) yes | 1.167 (7/7) yes | yes |
| design 5 | one IOThread vs main: one at a time 1.17x-1.21x | `bounce4k-qd1` | 1.157 (7/7) yes | 1.162 (7/7) yes | yes |
| design 5 | one IOThread vs main: one at a time 1.17x-1.21x | `zc64k-qd1` | 1.083 (6/7) no | 1.066 (6/7) no | no |
| design 4 | one IOThread vs main loop: one command at a time 1.15-1.21x | `nop-qd1` | 1.195 (7/7) yes | 1.213 (7/7) yes | yes |
| design 4 | one IOThread vs main loop: one command at a time 1.15-1.21x | `zc4k-qd1` | 1.176 (7/7) yes | 1.173 (7/7) yes | yes |
| design 4 | one IOThread vs main loop: one command at a time 1.15-1.21x | `bounce4k-qd1` | 1.148 (7/7) yes | 1.153 (7/7) yes | yes |
| design 4 | one IOThread vs main loop: one command at a time 1.15-1.21x | `zc64k-qd1` | 1.088 (7/7) no | 1.094 (7/7) no | no |
| design 4 | one IOThread vs main loop: shared file 0.79-0.88x | `nop-t3-qd8-sharedfile` | 0.802 (0/7) yes | 0.798 (0/7) yes | yes |
| design 4 | one IOThread vs main loop: shared file 0.79-0.88x | `zc4k-t3-qd8-sharedfile` | 0.860 (0/7) yes | 0.844 (0/7) yes | yes |
| design 5 | strict IOMMU: every fixed arm within 1% or better | `fixed4k-qd1` | 1.006 (4/7) yes | 0.988 (3/7) yes | yes |
| design 5 | strict IOMMU: every fixed arm within 1% or better | `fixed64k-qd1` | 1.009 (4/7) yes | 1.006 (4/7) yes | yes |
| design 5 | strict IOMMU: every fixed arm within 1% or better | `fixed4k-qd32-batch` | 1.033 (5/7) yes | 1.016 (6/7) yes | yes |
| design 5 | strict IOMMU: every fixed arm within 1% or better | `fixed64k-qd8-batch` | 0.979 (3/7) yes | 0.998 (3/7) yes | yes |
| README | one shared file 1.28x-1.39x (what changed; losses: instead of 1.28-1.39x) | `nop-t3-qd8-sharedfile` | 1.431 (7/7) no | 1.448 (7/7) no | no |
| README | one shared file 1.28x-1.39x (what changed; losses: instead of 1.28-1.39x) | `zc4k-t3-qd8-sharedfile` | 1.253 (7/7) no | 1.289 (7/7) yes | no |
| README | one IOThread: one file 1.07-1.10x (losses) | `nop-t3-qd8-sharedfile` | 1.173 (7/7) no | 1.161 (7/7) no | no |
| README | one IOThread: one file 1.07-1.10x (losses) | `zc4k-t3-qd8-sharedfile` | 1.095 (7/7) yes | 1.088 (7/7) yes | yes |
| README, design 2 | registered-buffer move took back most of the 1-4%: share of main's loss against o3 that r1c took back, (r1c/o3 - main/o3) / (1 - main/o3), more than half | `zc4k-qd32` | main 0.978 (1/7), r1c 0.984 (2/7): 0.30 no | main 0.972 (1/7), r1c 0.992 (1/7): 0.72 yes | no |

## Paired tables (median ratio new/base over the pairs, pairs won)

### final vs main, main loop (`compare-improve-vs-main.json`)

| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |
|---|---|---|---|---|---|---|
| NOP one at a time | 177.0 | 176.6 | 0.998 (2/7) | 175.8 | 176.0 | 1.004 (4/7) |
| 4 KiB one at a time | 151.4 | 150.4 | 0.994 (2/7) | 152.1 | 151.1 | 0.991 (2/7) |
| bounce 4 KiB one at a time | 139.3 | 139.9 | 1.002 (4/7) | 138.8 | 138.2 | 1.009 (4/7) |
| 64 KiB one at a time | 74.0 | 71.7 | 0.962 (2/7) | 74.3 | 71.8 | 0.961 (1/7) |
| 4 in flight | 402.7 | 398.5 | 0.991 (2/7) | 402.0 | 399.6 | 0.994 (1/7) |
| 32 in flight | 569.8 | 610.1 | 1.075 (7/7) | 564.8 | 617.8 | 1.106 (7/7) |
| 8 batched | 474.5 | 473.0 | 1.003 (4/7) | 473.3 | 474.4 | 1.002 (5/7) |
| 32 batched | 684.4 | 702.3 | 1.020 (6/7) | 682.9 | 695.8 | 1.019 (6/7) |
| 32 in flight, coalescing | 767.0 | 809.2 | 1.057 (5/7) | 771.4 | 818.9 | 1.060 (7/7) |
| three threads | 952.5 | 944.8 | 0.990 (2/7) | 945.6 | 944.3 | 0.988 (3/7) |
| three threads batched | 846.0 | 833.2 | 0.991 (1/7) | 857.3 | 841.6 | 0.987 (3/7) |
| one file, NOP | 512.8 | 745.2 | 1.431 (7/7) | 513.9 | 743.9 | 1.448 (7/7) |
| one file, 4 KiB | 485.2 | 616.9 | 1.253 (7/7) | 480.8 | 626.2 | 1.289 (7/7) |

### final with one IOThread vs main (`compare-improve-vs-main-iothread1.json`)

| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |
|---|---|---|---|---|---|---|
| NOP one at a time | 177.0 | 211.3 | 1.194 (7/7) | 175.8 | 213.4 | 1.217 (7/7) |
| 4 KiB one at a time | 151.4 | 176.9 | 1.159 (7/7) | 152.1 | 177.7 | 1.167 (7/7) |
| bounce 4 KiB one at a time | 139.3 | 160.3 | 1.157 (7/7) | 138.8 | 160.7 | 1.162 (7/7) |
| 64 KiB one at a time | 74.0 | 79.6 | 1.083 (6/7) | 74.3 | 80.1 | 1.066 (6/7) |
| 4 in flight | 402.7 | 450.5 | 1.123 (7/7) | 402.0 | 448.1 | 1.111 (7/7) |
| 32 in flight | 569.8 | 525.7 | 0.916 (0/7) | 564.8 | 527.0 | 0.936 (1/7) |
| 8 batched | 474.5 | 504.4 | 1.065 (7/7) | 473.3 | 511.3 | 1.081 (7/7) |
| 32 batched | 684.4 | 716.7 | 1.048 (7/7) | 682.9 | 713.1 | 1.043 (7/7) |
| 32 in flight, coalescing | 767.0 | 841.6 | 1.102 (6/7) | 771.4 | 845.5 | 1.092 (7/7) |
| three threads | 952.5 | 1283.1 | 1.356 (7/7) | 945.6 | 1283.2 | 1.357 (7/7) |
| three threads batched | 846.0 | 1257.7 | 1.487 (7/7) | 857.3 | 1267.3 | 1.480 (7/7) |
| one file, NOP | 512.8 | 601.4 | 1.173 (7/7) | 513.9 | 593.9 | 1.161 (7/7) |
| one file, 4 KiB | 485.2 | 530.1 | 1.095 (7/7) | 480.8 | 527.8 | 1.088 (7/7) |

### final with three IOThreads vs main (`compare-improve-vs-main-iothreads.json`)

| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |
|---|---|---|---|---|---|---|
| NOP one at a time | 177.0 | 177.5 | 1.002 (4/7) | 175.8 | 177.5 | 1.013 (5/7) |
| 4 KiB one at a time | 151.4 | 150.8 | 0.994 (2/7) | 152.1 | 150.8 | 1.002 (4/7) |
| bounce 4 KiB one at a time | 139.3 | 156.1 | 1.126 (7/7) | 138.8 | 157.5 | 1.135 (7/7) |
| 64 KiB one at a time | 74.0 | 73.8 | 0.998 (3/7) | 74.3 | 73.8 | 0.996 (3/7) |
| 4 in flight | 402.7 | 348.9 | 0.872 (0/7) | 402.0 | 353.4 | 0.879 (0/7) |
| 32 in flight | 569.8 | 356.3 | 0.621 (0/7) | 564.8 | 354.1 | 0.629 (0/7) |
| 8 batched | 474.5 | 461.2 | 0.971 (2/7) | 473.3 | 464.7 | 0.980 (2/7) |
| 32 batched | 684.4 | 683.4 | 0.994 (2/7) | 682.9 | 686.9 | 1.003 (5/7) |
| 32 in flight, coalescing | 767.0 | 600.0 | 0.781 (0/7) | 771.4 | 598.6 | 0.777 (0/7) |
| three threads | 952.5 | 813.0 | 0.850 (1/7) | 945.6 | 814.0 | 0.866 (0/7) |
| three threads batched | 846.0 | 1148.0 | 1.357 (7/7) | 857.3 | 1146.4 | 1.340 (7/7) |
| one file, NOP | 512.8 | 530.2 | 1.018 (4/7) | 513.9 | 522.1 | 1.017 (7/7) |
| one file, 4 KiB | 485.2 | 454.0 | 0.934 (0/7) | 480.8 | 454.5 | 0.942 (0/7) |

### final, one IOThread vs its main loop (`compare-improve-vs-improve-iothread1.json`)

| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |
|---|---|---|---|---|---|---|
| NOP one at a time | 176.6 | 211.3 | 1.195 (7/7) | 176.0 | 213.4 | 1.213 (7/7) |
| 4 KiB one at a time | 150.4 | 176.9 | 1.176 (7/7) | 151.1 | 177.7 | 1.173 (7/7) |
| bounce 4 KiB one at a time | 139.9 | 160.3 | 1.148 (7/7) | 138.2 | 160.7 | 1.153 (7/7) |
| 64 KiB one at a time | 71.7 | 79.6 | 1.088 (7/7) | 71.8 | 80.1 | 1.094 (7/7) |
| 4 in flight | 398.5 | 450.5 | 1.131 (7/7) | 399.6 | 448.1 | 1.125 (7/7) |
| 32 in flight | 610.1 | 525.7 | 0.862 (0/7) | 617.8 | 527.0 | 0.850 (0/7) |
| 8 batched | 473.0 | 504.4 | 1.067 (7/7) | 474.4 | 511.3 | 1.071 (7/7) |
| 32 batched | 702.3 | 716.7 | 1.018 (6/7) | 695.8 | 713.1 | 1.028 (6/7) |
| 32 in flight, coalescing | 809.2 | 841.6 | 1.038 (5/7) | 818.9 | 845.5 | 1.038 (6/7) |
| three threads | 944.8 | 1283.1 | 1.364 (7/7) | 944.3 | 1283.2 | 1.360 (7/7) |
| three threads batched | 833.2 | 1257.7 | 1.521 (7/7) | 841.6 | 1267.3 | 1.501 (7/7) |
| one file, NOP | 745.2 | 601.4 | 0.802 (0/7) | 743.9 | 593.9 | 0.798 (0/7) |
| one file, 4 KiB | 616.9 | 530.1 | 0.860 (0/7) | 626.2 | 527.8 | 0.844 (0/7) |

### final, three IOThreads vs its main loop (`compare-improve-vs-improve-iothreads.json`)

| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |
|---|---|---|---|---|---|---|
| NOP one at a time | 176.6 | 177.5 | 1.000 (4/7) | 176.0 | 177.5 | 1.007 (6/7) |
| 4 KiB one at a time | 150.4 | 150.8 | 1.003 (5/7) | 151.1 | 150.8 | 1.002 (4/7) |
| bounce 4 KiB one at a time | 139.9 | 156.1 | 1.125 (7/7) | 138.2 | 157.5 | 1.137 (7/7) |
| 64 KiB one at a time | 71.7 | 73.8 | 1.013 (5/7) | 71.8 | 73.8 | 1.014 (5/7) |
| 4 in flight | 398.5 | 348.9 | 0.878 (0/7) | 399.6 | 353.4 | 0.886 (0/7) |
| 32 in flight | 610.1 | 356.3 | 0.583 (0/7) | 617.8 | 354.1 | 0.573 (0/7) |
| 8 batched | 473.0 | 461.2 | 0.968 (0/7) | 474.4 | 464.7 | 0.973 (2/7) |
| 32 batched | 702.3 | 683.4 | 0.977 (1/7) | 695.8 | 686.9 | 0.986 (1/7) |
| 32 in flight, coalescing | 809.2 | 600.0 | 0.744 (0/7) | 818.9 | 598.6 | 0.732 (0/7) |
| three threads | 944.8 | 813.0 | 0.862 (0/7) | 944.3 | 814.0 | 0.861 (0/7) |
| three threads batched | 833.2 | 1148.0 | 1.380 (7/7) | 841.6 | 1146.4 | 1.361 (7/7) |
| one file, NOP | 745.2 | 530.2 | 0.711 (0/7) | 743.9 | 522.1 | 0.702 (0/7) |
| one file, 4 KiB | 616.9 | 454.0 | 0.740 (0/7) | 626.2 | 454.5 | 0.726 (0/7) |

### final vs o3 (`compare-improve-vs-o3.json`)

| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |
|---|---|---|---|---|---|---|
| NOP one at a time | 177.6 | 176.6 | 1.001 (4/7) | 175.9 | 176.0 | 0.996 (3/7) |
| 4 KiB one at a time | 150.8 | 150.4 | 0.994 (2/7) | 151.1 | 151.1 | 1.000 (3/7) |
| bounce 4 KiB one at a time | 139.7 | 139.9 | 0.999 (2/7) | 139.1 | 138.2 | 0.995 (2/7) |
| 64 KiB one at a time | 73.2 | 71.7 | 1.004 (4/7) | 73.4 | 71.8 | 0.975 (3/7) |
| 4 in flight | 401.2 | 398.5 | 0.993 (1/7) | 402.5 | 399.6 | 0.994 (1/7) |
| 32 in flight | 584.6 | 610.1 | 1.052 (7/7) | 585.9 | 617.8 | 1.064 (7/7) |
| 8 batched | 478.1 | 473.0 | 0.995 (0/7) | 479.5 | 474.4 | 0.988 (3/7) |
| 32 batched | 687.8 | 702.3 | 1.021 (5/7) | 698.1 | 695.8 | 1.001 (4/7) |
| 32 in flight, coalescing | 767.4 | 809.2 | 1.054 (5/7) | 770.3 | 818.9 | 1.069 (7/7) |
| three threads | 954.3 | 944.8 | 0.990 (2/7) | 958.4 | 944.3 | 0.984 (2/7) |
| three threads batched | 847.4 | 833.2 | 0.988 (2/7) | 834.4 | 841.6 | 1.009 (5/7) |
| one file, NOP | 524.8 | 745.2 | 1.421 (7/7) | 527.4 | 743.9 | 1.406 (7/7) |
| one file, 4 KiB | 490.4 | 616.9 | 1.245 (7/7) | 490.9 | 626.2 | 1.275 (7/7) |

### r1c vs o3 (`compare-r1c-vs-o3.json`)

| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |
|---|---|---|---|---|---|---|
| NOP one at a time | 177.6 | 177.3 | 1.008 (4/7) | 175.9 | 178.0 | 1.010 (6/7) |
| 4 KiB one at a time | 150.8 | 151.5 | 1.001 (4/7) | 151.1 | 151.6 | 1.003 (4/7) |
| bounce 4 KiB one at a time | 139.7 | 139.6 | 0.999 (3/7) | 139.1 | 138.8 | 1.001 (4/7) |
| 64 KiB one at a time | 73.2 | 73.9 | 0.995 (3/7) | 73.4 | 72.8 | 0.988 (1/7) |
| 4 in flight | 401.2 | 402.0 | 1.004 (5/7) | 402.5 | 404.3 | 1.004 (5/7) |
| 32 in flight | 584.6 | 571.7 | 0.984 (2/7) | 585.9 | 579.5 | 0.992 (1/7) |
| 8 batched | 478.1 | 476.5 | 0.993 (3/7) | 479.5 | 474.9 | 1.001 (4/7) |
| 32 batched | 687.8 | 696.1 | 1.011 (5/7) | 698.1 | 689.8 | 0.989 (1/7) |
| 32 in flight, coalescing | 767.4 | 769.3 | 0.999 (2/7) | 770.3 | 773.3 | 1.003 (5/7) |
| three threads | 954.3 | 941.7 | 0.998 (2/7) | 958.4 | 956.0 | 1.005 (4/7) |
| three threads batched | 841.4 | 851.3 | 1.009 (5/6) | 834.4 | 843.4 | 1.003 (5/7) |
| one file, NOP | 524.8 | 522.5 | 0.994 (3/7) | 527.4 | 528.9 | 1.004 (5/7) |
| one file, 4 KiB | 490.4 | 495.6 | 1.000 (4/7) | 490.9 | 497.3 | 1.013 (4/7) |

### r1c vs main (`compare-r1c-vs-main.json`)

| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |
|---|---|---|---|---|---|---|
| NOP one at a time | 177.0 | 177.3 | 1.007 (4/7) | 175.8 | 178.0 | 1.023 (5/7) |
| 4 KiB one at a time | 151.4 | 151.5 | 1.007 (4/7) | 152.1 | 151.6 | 0.992 (3/7) |
| bounce 4 KiB one at a time | 139.3 | 139.6 | 1.003 (4/7) | 138.8 | 138.8 | 1.008 (4/7) |
| 64 KiB one at a time | 74.0 | 73.9 | 0.993 (1/7) | 74.3 | 72.8 | 0.975 (0/7) |
| 4 in flight | 402.7 | 402.0 | 1.000 (3/7) | 402.0 | 404.3 | 0.999 (3/7) |
| 32 in flight | 569.8 | 571.7 | 1.005 (4/7) | 564.8 | 579.5 | 1.020 (4/7) |
| 8 batched | 474.5 | 476.5 | 0.999 (3/7) | 473.3 | 474.9 | 1.012 (5/7) |
| 32 batched | 684.4 | 696.1 | 1.012 (5/7) | 682.9 | 689.8 | 1.010 (5/7) |
| 32 in flight, coalescing | 767.0 | 769.3 | 1.002 (4/7) | 771.4 | 773.3 | 1.008 (4/7) |
| three threads | 952.5 | 941.7 | 0.999 (3/7) | 945.6 | 956.0 | 1.003 (6/7) |
| three threads batched | 839.9 | 851.3 | 1.013 (5/6) | 857.3 | 843.4 | 0.980 (2/7) |
| one file, NOP | 512.8 | 522.5 | 1.025 (5/7) | 513.9 | 528.9 | 1.029 (6/7) |
| one file, 4 KiB | 485.2 | 495.6 | 1.020 (5/7) | 480.8 | 497.3 | 1.024 (6/7) |

### main vs o3 (`compare-main-vs-o3.json`)

| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |
|---|---|---|---|---|---|---|
| NOP one at a time | 177.6 | 177.0 | 1.003 (5/7) | 175.9 | 175.8 | 0.992 (3/7) |
| 4 KiB one at a time | 150.8 | 151.4 | 0.995 (3/7) | 151.1 | 152.1 | 1.013 (6/7) |
| bounce 4 KiB one at a time | 139.7 | 139.3 | 0.993 (2/7) | 139.1 | 138.8 | 1.000 (4/7) |
| 64 KiB one at a time | 73.2 | 74.0 | 1.003 (4/7) | 73.4 | 74.3 | 1.017 (7/7) |
| 4 in flight | 401.2 | 402.7 | 1.004 (5/7) | 402.5 | 402.0 | 1.000 (4/7) |
| 32 in flight | 584.6 | 569.8 | 0.978 (1/7) | 585.9 | 564.8 | 0.972 (1/7) |
| 8 batched | 478.1 | 474.5 | 0.979 (2/7) | 479.5 | 473.3 | 0.982 (1/7) |
| 32 batched | 687.8 | 684.4 | 0.991 (2/7) | 698.1 | 682.9 | 0.982 (1/7) |
| 32 in flight, coalescing | 767.4 | 767.0 | 0.993 (2/7) | 770.3 | 771.4 | 1.005 (5/7) |
| three threads | 954.3 | 952.5 | 0.990 (2/7) | 958.4 | 945.6 | 0.992 (1/7) |
| three threads batched | 847.4 | 846.0 | 1.007 (4/7) | 834.4 | 857.3 | 1.029 (6/7) |
| one file, NOP | 524.8 | 512.8 | 0.977 (2/7) | 527.4 | 513.9 | 0.980 (0/7) |
| one file, 4 KiB | 490.4 | 485.2 | 0.985 (3/7) | 490.9 | 480.8 | 0.997 (1/7) |

### final vs main, fixed plan, no IOMMU (`compare-improve-vs-main.json`)

| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |
|---|---|---|---|---|---|---|
| 4 KiB one at a time | 152.0 | 141.2 | 0.949 (2/6) | 150.9 | 149.0 | 0.980 (2/7) |
| bounce 4 KiB one at a time | 137.6 | 134.1 | 0.988 (3/7) | 138.4 | 137.0 | 1.008 (4/7) |
| fixed 4 KiB one at a time | 155.5 | 156.9 | 1.013 (6/7) | 157.7 | 157.3 | 0.998 (3/7) |
| 64 KiB one at a time | 73.7 | 72.1 | 0.993 (3/7) | 73.6 | 73.0 | 1.005 (5/7) |
| bounce 64 KiB one at a time | 45.5 | 45.6 | 1.001 (4/7) | 45.7 | 45.7 | 0.998 (3/7) |
| fixed 64 KiB one at a time | 79.2 | 78.8 | 0.989 (2/7) | 79.8 | 78.7 | 0.998 (2/7) |
| 32 batched | 679.5 | 681.7 | 1.008 (4/7) | 675.1 | 695.4 | 1.012 (6/7) |
| bounce 4 KiB 32 batched | 633.6 | 635.8 | 1.012 (4/7) | 595.0 | 608.8 | 1.023 (6/7) |
| fixed 4 KiB 32 batched | 806.4 | 810.6 | 1.011 (5/7) | 810.5 | 817.2 | 1.017 (6/7) |
| 64 KiB 8 batched | 106.4 | 106.6 | 1.013 (5/7) | 103.7 | 110.3 | 1.063 (5/7) |
| bounce 64 KiB 8 batched | 71.4 | 73.3 | 1.021 (6/7) | 72.7 | 73.0 | 1.001 (4/7) |
| fixed 64 KiB 8 batched | 117.8 | 119.1 | 1.010 (5/7) | 122.6 | 124.0 | 1.012 (5/7) |

### final vs main, fixed plan, strict IOMMU (`compare-improve-vs-main-iommu-strict.json`)

| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |
|---|---|---|---|---|---|---|
| 4 KiB one at a time | 68.8 | 69.2 | 1.011 (5/7) | 70.1 | 70.3 | 1.004 (6/7) |
| bounce 4 KiB one at a time | 129.7 | 131.4 | 0.994 (3/7) | 131.8 | 131.4 | 1.003 (4/7) |
| fixed 4 KiB one at a time | 147.0 | 145.8 | 1.006 (4/7) | 147.1 | 145.3 | 0.988 (3/7) |
| 64 KiB one at a time | 28.5 | 28.1 | 1.053 (4/7) | 28.3 | 28.2 | 0.998 (3/7) |
| bounce 64 KiB one at a time | 41.1 | 41.3 | 1.011 (4/7) | 42.0 | 41.4 | 0.987 (2/7) |
| fixed 64 KiB one at a time | 66.6 | 67.2 | 1.009 (4/7) | 67.2 | 67.0 | 1.006 (4/7) |
| 32 batched | 101.4 | 100.9 | 1.008 (4/7) | 103.9 | 105.2 | 1.016 (5/7) |
| bounce 4 KiB 32 batched | 528.2 | 551.5 | 1.033 (6/7) | 545.7 | 556.7 | 1.020 (7/7) |
| fixed 4 KiB 32 batched | 671.2 | 694.2 | 1.033 (5/7) | 690.5 | 701.6 | 1.016 (6/7) |
| 64 KiB 8 batched | 36.1 | 35.1 | 1.015 (5/7) | 37.3 | 38.1 | 1.006 (6/7) |
| bounce 64 KiB 8 batched | 61.0 | 63.2 | 1.021 (6/7) | 64.2 | 64.4 | 0.994 (3/7) |
| fixed 64 KiB 8 batched | 99.9 | 101.1 | 0.979 (3/7) | 103.5 | 102.9 | 0.998 (3/7) |

## Losses of the final code against main in both sessions (median < 0.985, at most 2 of 7 won)

- noiommu/noiommu ab `zc64k-qd1`: 0.962 (2/7), 0.961 (1/7)
- noiommu/iothread1 ab `zc4k-qd32`: 0.916 (0/7), 0.936 (1/7)
- noiommu/noiommu fixed `zc4k-qd1`: 0.949 (2/6), 0.980 (2/7)

## Per-command counters, final build (summary.md medians)

| config | arm | k cmds/s | SQ db | MSI-X | vCPU us | main util | host CPU us | session |
|---|---|---|---|---|---|---|---|---|
| noiommu | nop-qd1 | 176.6 | 1.00 | 1.00 | 5.67 | 0.61 | 9.21 | s1 |
| noiommu | zc4k-qd1 | 150.4 | 1.00 | 1.00 | 6.67 | 0.60 | 10.80 | s1 |
| noiommu | zc4k-qd32 | 610.1 | 0.24 | 0.47 | 1.47 | 0.86 | 2.91 | s1 |
| noiommu | zc4k-qd32-batch | 702.3 | 0.03 | 0.06 | 1.43 | 0.53 | 2.20 | s1 |
| noiommu | zc4k-t3-qd8 | 944.8 | 0.15 | 0.29 | 2.75 | 0.92 | 3.74 | s1 |
| noiommu | zc4k-t3-qd8-batch | 833.2 | 0.12 | 0.25 | 3.24 | 0.83 | 4.25 | s1 |
| noiommu | nop-t3-qd8-sharedfile | 745.2 | 0.40 | 0.58 | 3.76 | 0.82 | 4.88 | s1 |
| noiommu | zc4k-t3-qd8-sharedfile | 616.9 | 0.42 | 0.69 | 4.07 | 0.94 | 5.61 | s1 |
| iothread1 | nop-qd1 | 211.3 | 1.00 | 1.00 | 4.75 | 0.00 | 6.85 | s1 |
| iothread1 | zc4k-qd1 | 176.9 | 1.00 | 1.00 | 5.67 | 0.00 | 8.31 | s1 |
| iothread1 | zc4k-qd32 | 525.7 | 0.48 | 0.73 | 1.88 | 0.00 | 3.33 | s1 |
| iothread1 | zc4k-qd32-batch | 716.7 | 0.03 | 0.06 | 1.40 | 0.00 | 2.10 | s1 |
| iothread1 | zc4k-t3-qd8 | 1283.1 | 0.18 | 0.33 | 2.34 | 0.00 | 3.12 | s1 |
| iothread1 | zc4k-t3-qd8-batch | 1257.7 | 0.13 | 0.25 | 2.39 | 0.00 | 3.16 | s1 |
| iothread1 | nop-t3-qd8-sharedfile | 601.4 | 0.86 | 0.96 | 4.94 | 0.00 | 6.27 | s1 |
| iothread1 | zc4k-t3-qd8-sharedfile | 530.1 | 0.91 | 0.99 | 5.59 | 0.00 | 7.29 | s1 |
| iothreads | nop-qd1 | 177.5 | 1.00 | 1.00 | 5.65 | 0.00 | 8.15 | s1 |
| iothreads | zc4k-qd1 | 150.8 | 1.00 | 1.00 | 6.67 | 0.00 | 9.73 | s1 |
| iothreads | zc4k-qd32 | 356.3 | 0.73 | 0.97 | 2.82 | 0.00 | 4.88 | s1 |
| iothreads | zc4k-qd32-batch | 683.4 | 0.03 | 0.06 | 1.47 | 0.00 | 2.26 | s1 |
| iothreads | zc4k-t3-qd8 | 813.0 | 0.77 | 1.00 | 3.69 | 0.00 | 6.05 | s1 |
| iothreads | zc4k-t3-qd8-batch | 1148.0 | 0.13 | 0.25 | 2.61 | 0.00 | 3.75 | s1 |
| iothreads | nop-t3-qd8-sharedfile | 530.2 | 0.89 | 0.96 | 5.60 | 0.00 | 7.79 | s1 |
| iothreads | zc4k-t3-qd8-sharedfile | 454.0 | 0.96 | 1.00 | 6.54 | 0.00 | 9.43 | s1 |
| noiommu | nop-qd1 | 176.0 | 1.00 | 1.00 | 5.69 | 0.61 | 9.24 | s2 |
| noiommu | zc4k-qd1 | 151.1 | 1.00 | 1.00 | 6.64 | 0.61 | 10.71 | s2 |
| noiommu | zc4k-qd32 | 617.8 | 0.24 | 0.47 | 1.46 | 0.86 | 2.89 | s2 |
| noiommu | zc4k-qd32-batch | 695.8 | 0.03 | 0.06 | 1.43 | 0.52 | 2.20 | s2 |
| noiommu | zc4k-t3-qd8 | 944.3 | 0.14 | 0.29 | 2.75 | 0.92 | 3.75 | s2 |
| noiommu | zc4k-t3-qd8-batch | 841.6 | 0.13 | 0.25 | 3.21 | 0.84 | 4.22 | s2 |
| noiommu | nop-t3-qd8-sharedfile | 743.9 | 0.40 | 0.58 | 3.76 | 0.82 | 4.87 | s2 |
| noiommu | zc4k-t3-qd8-sharedfile | 626.2 | 0.40 | 0.68 | 4.02 | 0.94 | 5.53 | s2 |
| iothread1 | nop-qd1 | 213.4 | 1.00 | 1.00 | 4.70 | 0.00 | 6.81 | s2 |
| iothread1 | zc4k-qd1 | 177.7 | 1.00 | 1.00 | 5.65 | 0.00 | 8.29 | s2 |
| iothread1 | zc4k-qd32 | 527.0 | 0.48 | 0.73 | 1.88 | 0.00 | 3.32 | s2 |
| iothread1 | zc4k-qd32-batch | 713.1 | 0.03 | 0.06 | 1.40 | 0.00 | 2.10 | s2 |
| iothread1 | zc4k-t3-qd8 | 1283.2 | 0.18 | 0.33 | 2.34 | 0.00 | 3.12 | s2 |
| iothread1 | zc4k-t3-qd8-batch | 1267.3 | 0.13 | 0.25 | 2.37 | 0.00 | 3.13 | s2 |
| iothread1 | nop-t3-qd8-sharedfile | 593.9 | 0.87 | 0.96 | 5.00 | 0.00 | 6.30 | s2 |
| iothread1 | zc4k-t3-qd8-sharedfile | 527.8 | 0.91 | 0.99 | 5.61 | 0.00 | 7.32 | s2 |
| iothreads | nop-qd1 | 177.5 | 1.00 | 1.00 | 5.65 | 0.00 | 8.14 | s2 |
| iothreads | zc4k-qd1 | 150.8 | 1.00 | 1.00 | 6.67 | 0.00 | 9.85 | s2 |
| iothreads | zc4k-qd32 | 354.1 | 0.71 | 0.96 | 2.83 | 0.00 | 4.93 | s2 |
| iothreads | zc4k-qd32-batch | 686.9 | 0.03 | 0.06 | 1.47 | 0.00 | 2.25 | s2 |
| iothreads | zc4k-t3-qd8 | 814.0 | 0.77 | 1.00 | 3.69 | 0.00 | 6.02 | s2 |
| iothreads | zc4k-t3-qd8-batch | 1146.4 | 0.12 | 0.25 | 2.62 | 0.00 | 3.74 | s2 |
| iothreads | nop-t3-qd8-sharedfile | 522.1 | 0.90 | 0.97 | 5.69 | 0.00 | 7.83 | s2 |
| iothreads | zc4k-t3-qd8-sharedfile | 454.5 | 0.97 | 1.00 | 6.53 | 0.00 | 9.38 | s2 |

s1: main loop, batched/unbatched three threads 833.2/944.8 = 0.882; one file/three files 616.9/944.8 = 0.653; one IOThread batched/unbatched 1257.7/1283.1 = 0.980

s2: main loop, batched/unbatched three threads 841.6/944.3 = 0.891; one file/three files 626.2/944.3 = 0.663; one IOThread batched/unbatched 1267.3/1283.2 = 0.988

s1: vCPUs busy at three threads (median `vcpu_util` of `summary.json`, of 3): noiommu 2.54, iothread1 2.94, iothreads 2.94

s2: vCPUs busy at three threads (median `vcpu_util` of `summary.json`, of 3): noiommu 2.54, iothread1 2.94, iothreads 2.94

## One command at a time, per boot (k cmds/s of each accepted window, in boot order)

- s1 ab `zc64k-qd1` (slow: below 0.95 of main's median 74.0):
  - improve iothread1: 79.6, 79.9, 78.1, 78.8, 79.7, 72.2, 80.4; slow - of 7
  - improve noiommu: 73.7, 67.2, 71.7, 74.2, 68.0, 64.0, 73.9; slow 3 of 7
  - main noiommu: 73.0, 74.4, 74.5, 72.7, 73.1, 74.4, 74.0; slow 0 of 7
  - o3 noiommu: 73.2, 73.0, 74.7, 74.0, 67.5, 74.2, 67.6; slow 2 of 7
  - r1c noiommu: 67.0, 68.9, 74.0, 74.0, 67.2, 74.3, 73.9; slow 3 of 7
- s1 ab `zc4k-qd1` (slow: below 0.95 of main's median 151.4):
  - improve iothread1: 167.8, 177.7, 173.0, 174.3, 177.7, 176.9, 176.9; slow - of 7
  - improve noiommu: 150.9, 151.2, 146.7, 151.8, 149.8, 147.4, 150.4; slow 0 of 7
  - main noiommu: 151.8, 150.0, 151.4, 147.7, 150.0, 152.7, 153.4; slow 0 of 7
  - o3 noiommu: 149.7, 151.9, 154.8, 154.9, 150.8, 149.3, 149.7; slow 0 of 7
  - r1c noiommu: 151.5, 151.5, 152.5, 148.7, 151.0, 151.3, 152.4; slow 0 of 7
- s1 fixed `zc4k-qd1` (slow: below 0.95 of main's median 152.0):
  - improve noiommu: 150.5, 152.1, 137.4, 143.2, 139.3, 124.5, 139.0; slow 5 of 7
  - main noiommu: 150.2, 152.0, 152.6, 152.1, 145.5, 151.9; slow 0 of 6
- s2 ab `zc64k-qd1` (slow: below 0.95 of main's median 74.3):
  - improve iothread1: 80.7, 80.3, 80.1, 80.5, 78.6, 72.8, 75.7; slow - of 7
  - improve noiommu: 73.8, 67.2, 66.8, 73.3, 71.8, 67.3, 73.9; slow 3 of 7
  - main noiommu: 74.2, 74.8, 75.1, 73.1, 74.7, 74.3, 74.2; slow 0 of 7
  - o3 noiommu: 72.5, 74.6, 73.8, 68.7, 73.6, 73.4, 66.8; slow 2 of 7
  - r1c noiommu: 70.5, 73.7, 68.0, 67.0, 72.8, 72.8, 73.3; slow 3 of 7
- s2 ab `zc4k-qd1` (slow: below 0.95 of main's median 152.1):
  - improve iothread1: 177.7, 178.2, 178.1, 177.6, 174.6, 179.5, 174.6; slow - of 7
  - improve noiommu: 151.1, 151.9, 145.0, 152.0, 151.1, 143.7, 151.1; slow 1 of 7
  - main noiommu: 149.6, 153.3, 152.6, 149.3, 155.6, 150.8, 152.1; slow 0 of 7
  - o3 noiommu: 148.6, 151.3, 151.1, 152.0, 152.8, 144.2, 150.2; slow 1 of 7
  - r1c noiommu: 148.5, 151.7, 152.7, 151.6, 155.9, 149.6, 150.1; slow 0 of 7
- s2 fixed `zc4k-qd1` (slow: below 0.95 of main's median 150.9):
  - improve noiommu: 150.8, 140.9, 150.6, 125.0, 141.0, 150.1, 149.0; slow 3 of 7
  - main noiommu: 150.7, 151.1, 150.9, 149.3, 151.4, 145.7, 152.1; slow 0 of 7
