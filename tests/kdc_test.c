// SPDX-License-Identifier: GPL-2.0
/*
 * In-guest test client for /dev/kdcN (static binary, runs from the initramfs).
 * Every case logs "PASS <name>"; the first failed assertion logs
 * "FAIL <case>:<line>: <expression>" and exits non-zero. All structured output
 * goes to the kernel log (/dev/kmsg), so it reaches the serial console whole
 * and in order with kernel messages; scripts/run_vm.py parses it from there.
 *
 * Tests that provoke a kernel report (a DMAR fault, a driver recovery message)
 * bracket it with EXPECT_BEGIN/EXPECT_END and check through /dev/kmsg that the
 * report appeared. Tests that need the host (hot-unplug, a device that stops
 * responding) log "KDC_HOST <request>" and wait for the effect.
 */
#define _GNU_SOURCE
#include "kdc_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(struct kdc_submit) == 80, "submit ABI");
_Static_assert(sizeof(struct kdc_cancel) == 32, "cancel ABI");
_Static_assert(sizeof(struct kdc_completion) == 40, "completion ABI");
_Static_assert(sizeof(struct kdc_stats) == 320, "stats ABI");

static int kmsg_out = -1;
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	char buf[900];
	va_list ap;
	int n = snprintf(buf, sizeof(buf), "kdc_test: ");

	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	/* Without the newline the kernel keeps the record open for continuation
	 * and the console prints it only when the next message arrives. */
	buf[n++] = '\n';
	buf[n] = 0;
	if (kmsg_out >= 0 && write(kmsg_out, buf, n) > 0)
		return;
	fputs(buf, stdout);
}

static void release_device_knobs(void);
#define CHECK(x)                                                                         \
	do {                                                                             \
		if (!(x)) {                                                              \
			int e_ = errno;                                                  \
			say("FAIL %s:%d: %s (errno=%d %s)", __func__, __LINE__, #x, e_,  \
			    strerror(e_));                                               \
			fprintf(stderr, "FAIL %s:%d: %s (errno=%d %s)\n", __func__,      \
				__LINE__, #x, e_, strerror(e_));                         \
			fflush(stderr);                                                  \
			release_device_knobs();                                          \
			exit(1);                                                         \
		}                                                                        \
	} while (0)
#define ERR(expr, expected)                    \
	do {                                   \
		errno = 0;                     \
		CHECK((expr) == -1);           \
		CHECK(errno == (expected));    \
	} while (0)

#define PAGE 4096UL
#define SC_ABORTED 5
#define SC_DATA_ERROR 4
#define FAULT_STALL 1
#define FAULT_FATAL 2
#define FAULT_RESET_HANG 4
#define FAULT_STUCK 8
#define HOLD_MS 60000 /* timeout for commands a test holds device-owned on purpose */

static unsigned ncpus;
static uint32_t bounce_max; /* the driver's choice for device 0, read at start */

/* ---- helpers ---- */

/* On failure, un-stall the devices so that close() in exit() can finish. */
static void release_device_knobs(void)
{
	static const char *const knobs[] = { "fault", "service_ns" };
	char path[96];

	for (int dev = 0; dev < 3; dev++)
		for (unsigned k = 0; k < 2; k++) {
			snprintf(path, sizeof(path), "/sys/kernel/debug/kdc/kdc%d/%s", dev, knobs[k]);
			int fd = open(path, O_WRONLY);
			if (fd >= 0) {
				if (write(fd, "0\n", 2) != 2)
					fprintf(stderr, "could not reset %s\n", path);
				close(fd);
			}
		}
}

static uint32_t crc_table[256];
static void crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (c & 1 ? 0x82F63B78u : 0);
		crc_table[i] = c;
	}
}
static uint32_t crc32c(const void *data, size_t len, uint32_t seed)
{
	const uint8_t *p = data;
	uint32_t crc = ~seed;
	while (len--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void)
{
	uint64_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return rng_state = x;
}
static void fill_random(void *buf, size_t len, uint64_t seed)
{
	uint8_t *p = buf;
	uint64_t x = seed | 1;
	for (size_t i = 0; i < len; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		p[i] = (uint8_t)x;
	}
}

static void *map_pages(size_t pages)
{
	void *p = mmap(NULL, pages * PAGE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(p != MAP_FAILED);
	memset(p, 0, pages * PAGE);
	return p;
}

static void sleep_ms(long ms)
{
	struct timespec t = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
	while (nanosleep(&t, &t) && errno == EINTR)
		;
}

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void pin_cpu(unsigned cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	CHECK(sched_setaffinity(0, sizeof(set), &set) == 0);
}

static void unpin_cpu(void)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	for (unsigned i = 0; i < ncpus; i++)
		CPU_SET(i, &set);
	CHECK(sched_setaffinity(0, sizeof(set), &set) == 0);
}

static int open_dev(int index)
{
	char path[32];
	snprintf(path, sizeof(path), "/dev/kdc%d", index);
	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	CHECK(fd >= 0);
	return fd;
}

static void knob(int dev, const char *name, unsigned long value)
{
	char path[96], text[32];
	snprintf(path, sizeof(path), "/sys/kernel/debug/kdc/kdc%d/%s", dev, name);
	int fd = open(path, O_WRONLY);
	CHECK(fd >= 0);
	int n = snprintf(text, sizeof(text), "%lu\n", value);
	CHECK(write(fd, text, n) == n);
	close(fd);
}

static unsigned long read_knob(int dev, const char *name)
{
	char path[96], text[32] = { 0 };
	snprintf(path, sizeof(path), "/sys/kernel/debug/kdc/kdc%d/%s", dev, name);
	int fd = open(path, O_RDONLY);
	CHECK(fd >= 0);
	CHECK(read(fd, text, sizeof(text) - 1) > 0);
	close(fd);
	return strtoul(text, NULL, 10);
}

static struct kdc_stats stats(int fd)
{
	struct kdc_stats s;
	memset(&s, 0xcc, sizeof(s));
	CHECK(ioctl(fd, KDC_IOC_STATS, &s) == 0);
	CHECK(s.version == KDC_ABI_VERSION && s.size == sizeof(s));
	return s;
}

static struct kdc_submit req_epoch(uint64_t epoch, uint64_t id, const void *src, void *dst,
				   uint32_t len)
{
	struct kdc_submit s = { .version = KDC_ABI_VERSION, .size = sizeof(s), .id = id,
				.epoch = epoch, .src = (uintptr_t)src, .dst = (uintptr_t)dst,
				.length = len, .queue = KDC_QUEUE_AUTO };
	return s;
}

static struct kdc_submit req(int fd, uint64_t id, const void *src, void *dst, uint32_t len)
{
	struct kdc_submit s = { .version = KDC_ABI_VERSION, .size = sizeof(s), .id = id,
				.epoch = stats(fd).epoch, .src = (uintptr_t)src,
				.dst = (uintptr_t)dst, .length = len, .queue = KDC_QUEUE_AUTO };
	return s;
}

static int submit(int fd, struct kdc_submit *s)
{
	return ioctl(fd, KDC_IOC_SUBMIT, s);
}

static int cancel(int fd, uint64_t id, uint64_t epoch)
{
	struct kdc_cancel c = { .version = KDC_ABI_VERSION, .size = sizeof(c), .id = id,
				.epoch = epoch };
	return ioctl(fd, KDC_IOC_CANCEL, &c);
}

static bool wait_ready(int fd, int timeout_ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	int r = poll(&p, 1, timeout_ms);
	CHECK(r >= 0);
	CHECK(!(p.revents & (POLLERR | POLLNVAL)));
	return r == 1 && (p.revents & POLLIN);
}

static struct kdc_completion completion(int fd)
{
	struct kdc_completion c;
	CHECK(wait_ready(fd, 10000));
	CHECK(read(fd, &c, sizeof(c)) == sizeof(c));
	CHECK(c.reserved == 0);
	return c;
}

static void baseline(int fd)
{
	struct kdc_stats s = stats(fd);
	CHECK(!s.live && !s.staged && !s.pending && !s.device_owned && !s.ready);
	CHECK(!s.bytes && !s.pinned_pages && !s.resetting);
}

static void wait_device_owned(int fd, unsigned n)
{
	for (int i = 0; i < 5000; i++) {
		if (stats(fd).device_owned >= n)
			return;
		sleep_ms(1);
	}
	CHECK(!"device_owned never reached");
}

/* The device itself has fetched @n commands (not just the doorbell rung). */
static void wait_device_fetched(int dev, unsigned long n)
{
	for (int i = 0; i < 5000; i++) {
		if (read_knob(dev, "device_inflight") >= n)
			return;
		sleep_ms(1);
	}
	CHECK(!"device never fetched the command");
}

static void write_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY);
	CHECK(fd >= 0);
	CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
	close(fd);
}

static unsigned long read_ul(const char *path)
{
	char text[64] = { 0 };
	int fd = open(path, O_RDONLY);
	CHECK(fd >= 0);
	CHECK(read(fd, text, sizeof(text) - 1) > 0);
	close(fd);
	return strtoul(text, NULL, 10);
}

/* Driver parameters (built in, so under /sys/module/kdc). */
static void set_param(const char *name, unsigned long value)
{
	char path[96], text[32];
	snprintf(path, sizeof(path), "/sys/module/kdc/parameters/%s", name);
	snprintf(text, sizeof(text), "%lu\n", value);
	write_file(path, text);
}

#define DEFAULT_ABORT_MS 500
/* Tests that hold an abort inside a stalled device on purpose. */
static void hold_aborts(bool hold)
{
	set_param("abort_timeout_ms", hold ? HOLD_MS : DEFAULT_ABORT_MS);
}

/* Module-wide driver counters, across devices (debugfs kdc/). */
struct globals {
	unsigned long cmds, pinned, mapped, ranges;
};
static struct globals globals(void)
{
	struct globals g = { read_ul("/sys/kernel/debug/kdc/cmds"),
			     read_ul("/sys/kernel/debug/kdc/pinned_pages"),
			     read_ul("/sys/kernel/debug/kdc/mapped_sgts"),
			     read_ul("/sys/kernel/debug/kdc/dst_ranges") };
	return g;
}

static void globals_idle(void)
{
	struct globals g = globals();
	CHECK(g.cmds == 0 && g.pinned == 0 && g.mapped == 0 && g.ranges == 0);
}

/* ---- the kernel log: expected reports and host requests ---- */

static int kmsg_reader(void)
{
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	CHECK(fd >= 0);
	CHECK(lseek(fd, 0, SEEK_END) >= 0);
	return fd;
}

/* Waits for a record containing @needle that was logged after the reader opened. */
static bool kmsg_wait(int fd, const char *needle, int timeout_ms)
{
	char buf[8192];
	long end = now_ms() + timeout_ms;

	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = 0;
			char *msg = strchr(buf, ';');
			if (msg && strstr(msg, needle))
				return true;
			continue;
		}
		if (n < 0 && errno == EPIPE)
			continue; /* records were overwritten; keep reading */
		if (now_ms() > end)
			return false;
		sleep_ms(10);
	}
}

struct expect {
	const char *tag;
	int fd;
};

/* Report lines matching @regex are allowed until expect_end(); one must appear. */
static struct expect expect_begin(const char *tag, const char *regex)
{
	struct expect e = { tag, kmsg_reader() };
	say("EXPECT_BEGIN %s %s", tag, regex);
	return e;
}

static void expect_seen(struct expect *e, const char *needle)
{
	CHECK(kmsg_wait(e->fd, needle, 15000));
}

static void expect_end(struct expect *e)
{
	say("EXPECT_END %s", e->tag);
	close(e->fd);
}

static void host(const char *request)
{
	say("KDC_HOST %s", request);
}

/* ---- PCI functions by address ---- */

#define BDF0 "0000:01:00.0"
#define BDF1 "0000:02:00.0"

static bool bdf_present(const char *bdf)
{
	char path[128];
	struct stat st;
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s", bdf);
	return stat(path, &st) == 0;
}

/* /dev/kdcN index of the function at @bdf, or -1. */
static int kdc_index(const char *bdf)
{
	for (int i = 0; i < 8; i++) {
		char path[64], link[256];
		snprintf(path, sizeof(path), "/sys/class/kdc/kdc%d/device", i);
		ssize_t n = readlink(path, link, sizeof(link) - 1);
		if (n <= 0)
			continue;
		link[n] = 0;
		if (!strcmp(strrchr(link, '/') + 1, bdf))
			return i;
	}
	return -1;
}

static int wait_kdc_index(const char *bdf, int timeout_ms)
{
	char path[32];
	struct stat st;
	for (long end = now_ms() + timeout_ms; now_ms() < end; sleep_ms(10)) {
		int i = kdc_index(bdf);
		snprintf(path, sizeof(path), "/dev/kdc%d", i);
		if (i >= 0 && stat(path, &st) == 0)
			return i;
	}
	return -1;
}

static bool wait_absent(const char *bdf, int timeout_ms)
{
	for (long end = now_ms() + timeout_ms; now_ms() < end; sleep_ms(10))
		if (!bdf_present(bdf))
			return true;
	return false;
}

/* First register of BAR0, read through sysfs, bypassing the driver. */
static uint32_t bar0_id(const char *bdf)
{
	char path[128];
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", bdf);
	int fd = open(path, O_RDWR | O_SYNC);
	CHECK(fd >= 0);
	volatile uint32_t *regs = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(regs != MAP_FAILED);
	uint32_t id = regs[0];
	munmap((void *)regs, PAGE);
	close(fd);
	return id;
}

static bool iommu_active(void)
{
	struct stat st;
	return stat("/sys/class/iommu/dmar0", &st) == 0 && stat("/sys/kernel/iommu_groups/0", &st) == 0;
}

static struct kdc_stats wait_state(int fd, uint32_t state, int timeout_ms)
{
	struct kdc_stats s;
	for (long end = now_ms() + timeout_ms;; sleep_ms(5)) {
		s = stats(fd);
		if (s.dev_state == state || now_ms() > end)
			return s;
	}
}

/* ---- cases ---- */

static void device_info(void)
{
	int fd = open_dev(0);
	struct kdc_stats s = stats(fd);
	CHECK(s.nr_queues == 4 && s.queue_depth == 64);
	/* the default follows the DMA path: direct, or through a translating IOMMU */
	CHECK(s.bounce_max == (iommu_active() ? 65536u : 2048u) && s.max_length == KDC_MAX_LENGTH);
	CHECK(s.epoch == 1 && s.dev_open_files == 1);
	CHECK(s.dev_bad_completions == 0 && s.dev_errors == 0);
	baseline(fd);
	close(fd);
}

static void abi_validation(void)
{
	int fd = open_dev(0);
	char *buf = map_pages(4);
	struct kdc_submit ok = req(fd, 1, buf, buf + 2 * PAGE, 64), s;
	struct kdc_completion c;

	ERR(ioctl(fd, _IO('X', 1), 0), ENOTTY);
	ERR(ioctl(fd, _IOC(_IOC_WRITE, 'K', 0x20, sizeof(s) - 1), &s), ENOTTY);
	ERR(ioctl(fd, KDC_IOC_SUBMIT, (void *)1), EFAULT);
	ERR(ioctl(fd, KDC_IOC_STATS, (void *)1), EFAULT);
	s = ok; s.version = 1; ERR(submit(fd, &s), EPROTO);
	s = ok; s.size--; ERR(submit(fd, &s), EINVAL);
	s = ok; s.reserved[1] = 1; ERR(submit(fd, &s), EINVAL);
	s = ok; s.reserved0 = 1; ERR(submit(fd, &s), EINVAL);
	s = ok; s.reserved1 = 1; ERR(submit(fd, &s), EINVAL);
	s = ok; s.timeout_ms = KDC_MAX_TIMEOUT_MS + 1; ERR(submit(fd, &s), EINVAL);
	s = ok; s.flags = 0x100; ERR(submit(fd, &s), EINVAL);
	s = ok; s.flags = KDC_SUBMIT_BOUNCE | KDC_SUBMIT_ZEROCOPY; ERR(submit(fd, &s), EINVAL);
	s = ok; s.length = 0; ERR(submit(fd, &s), EINVAL);
	s = ok; s.length = KDC_MAX_LENGTH + 1; ERR(submit(fd, &s), EINVAL);
	s = ok; s.src = 0; ERR(submit(fd, &s), EINVAL);
	s = ok; s.dst = s.src + 63; ERR(submit(fd, &s), EINVAL);
	s = ok; s.queue = 4; ERR(submit(fd, &s), EINVAL);
	s = ok; s.flags = KDC_SUBMIT_NOP; ERR(submit(fd, &s), EINVAL);
	size_t span = (bounce_max + 1 + PAGE - 1) / PAGE * PAGE;
	char *big = map_pages(2 * span / PAGE);
	s = ok; s.src = (uintptr_t)big; s.dst = (uintptr_t)(big + span); s.length = bounce_max + 1;
	s.flags = KDC_SUBMIT_BOUNCE; ERR(submit(fd, &s), EINVAL); /* larger than the bounce buffer */
	munmap(big, 2 * span);
	s = ok; s.src = 0x1000; ERR(submit(fd, &s), EFAULT);
	s = ok; s.dst = 0x1000; s.flags = KDC_SUBMIT_ZEROCOPY; ERR(submit(fd, &s), EFAULT);
	s = ok; s.epoch++; ERR(submit(fd, &s), ESTALE);
	ERR(cancel(fd, 99, 1), ENOENT);
	struct kdc_cancel bad = { .version = 1, .size = sizeof(bad) };
	ERR(ioctl(fd, KDC_IOC_CANCEL, &bad), EPROTO);
	bad.version = KDC_ABI_VERSION; bad.reserved = 1;
	ERR(ioctl(fd, KDC_IOC_CANCEL, &bad), EINVAL);
	ERR(read(fd, &c, sizeof(c)), EAGAIN);
	ERR(read(fd, &c, sizeof(c) - 1), EINVAL);
	baseline(fd);
	/* duplicate ids are refused while the first is unread */
	knob(0, "service_ns", 50000000);
	s = ok; CHECK(submit(fd, &s) == 0);
	ERR(submit(fd, &s), EEXIST);
	knob(0, "service_ns", 0);
	CHECK(wait_ready(fd, 5000));
	/* a faulting read must not lose the completion */
	ERR(syscall(SYS_read, fd, (void *)1, sizeof(c)), EFAULT);
	c = completion(fd);
	CHECK(c.id == 1 && c.status == 0);
	baseline(fd);
	munmap(buf, 4 * PAGE);
	close(fd);
}

static void check_copy(int fd, size_t len, size_t src_off, size_t dst_off, uint32_t flags,
		       uint32_t seed)
{
	size_t pages = (len + 8192) / PAGE + 2;
	uint8_t *src = map_pages(pages), *dst = map_pages(pages);
	uint8_t *s_at = src + src_off, *d_at = dst + 64 + dst_off;
	static uint64_t id;

	fill_random(s_at, len, len * 31 + src_off);
	memset(dst, 0xEE, pages * PAGE);
	struct kdc_submit s = req(fd, ++id, s_at, d_at, (uint32_t)len);
	s.flags = flags;
	s.crc_seed = seed;
	CHECK(submit(fd, &s) == 0);
	struct kdc_completion c = completion(fd);
	CHECK(c.id == id && c.status == 0 && c.hw_status == 0);
	CHECK(c.length == len);
	CHECK(c.crc == crc32c(s_at, len, seed));
	CHECK(memcmp(d_at, s_at, len) == 0);
	for (size_t i = 0; i < 64 + dst_off; i++)
		CHECK(dst[i] == 0xEE);
	for (size_t i = 64 + dst_off + len; i < pages * PAGE; i++)
		CHECK(dst[i] == 0xEE);
	bool bounced = c.flags & KDC_COMPL_BOUNCE;
	if (flags & KDC_SUBMIT_BOUNCE)
		CHECK(bounced);
	else if (flags & KDC_SUBMIT_ZEROCOPY)
		CHECK(!bounced);
	else
		CHECK(bounced == (len <= bounce_max));
	munmap(src, pages * PAGE);
	munmap(dst, pages * PAGE);
}

static void data_integrity(void)
{
	static const size_t sizes[] = { 1, 3, 64, 4095, 4096, 4097, 8197, 65536, 200003,
					KDC_MAX_LENGTH };
	static const size_t offs[][2] = { { 0, 0 }, { 1, 3 }, { 4095, 2048 } };
	int fd = open_dev(0), cases = 0;

	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		for (unsigned o = 0; o < 3; o++) {
			size_t len = sizes[i];
			if (offs[o][0] + len > KDC_MAX_LENGTH + 4095)
				continue;
			check_copy(fd, len, offs[o][0], offs[o][1], 0, 0);
			check_copy(fd, len, offs[o][0], offs[o][1], KDC_SUBMIT_ZEROCOPY, 0x1234);
			cases += 2;
			if (len <= bounce_max) {
				check_copy(fd, len, offs[o][0], offs[o][1], KDC_SUBMIT_BOUNCE, 7);
				cases++;
			}
		}
	/* CRC chaining: crc(A || B) == crc(B, seed = crc(A)) through the device */
	uint8_t *a = map_pages(4), *d = map_pages(4);
	fill_random(a, 3 * PAGE, 99);
	struct kdc_submit s = req(fd, 500, a, d, 5000);
	CHECK(submit(fd, &s) == 0);
	struct kdc_completion c1 = completion(fd);
	s = req(fd, 501, a + 5000, d, 7000);
	s.crc_seed = c1.crc;
	CHECK(submit(fd, &s) == 0);
	CHECK(completion(fd).crc == crc32c(a, 12000, 0));
	baseline(fd);
	close(fd);
	say("  data_integrity: %d copies verified (dst bytes, guard bytes, CRC32C)", cases);
}

static void nop_and_batch_read(void)
{
	int fd = open_dev(0);
	struct kdc_completion cs[16];
	struct kdc_submit s = req(fd, 1, NULL, NULL, 0);

	CHECK(!wait_ready(fd, 0));
	s.flags = KDC_SUBMIT_NOP;
	CHECK(submit(fd, &s) == 0);
	struct kdc_completion c = completion(fd);
	CHECK(c.id == 1 && c.status == 0 && c.length == 0 && c.crc == 0);
	for (int i = 0; i < 8; i++) {
		s.id = 10 + i;
		CHECK(submit(fd, &s) == 0);
	}
	for (int i = 0; i < 2000 && stats(fd).ready < 8; i++)
		sleep_ms(1);
	CHECK(read(fd, cs, sizeof(cs)) == 8 * (ssize_t)sizeof(cs[0]));
	for (int i = 0; i < 8; i++)
		CHECK(cs[i].status == 0 && cs[i].id >= 10 && cs[i].id < 18);
	baseline(fd);
	close(fd);
}

static void queue_selection(void)
{
	int fd = open_dev(0);
	struct kdc_submit s = req(fd, 1, NULL, NULL, 0);
	unsigned seen = 0;
	struct kdc_stats before = stats(fd), after;

	s.flags = KDC_SUBMIT_NOP;
	for (uint16_t q = 0; q < 4; q++) {
		s.queue = q;
		s.id = 100 + q;
		CHECK(submit(fd, &s) == 0);
		CHECK(completion(fd).queue == q);
	}
	after = stats(fd);
	for (int q = 0; q < 4; q++)
		CHECK(after.queue_submitted[q] == before.queue_submitted[q] + 1);
	s.queue = KDC_QUEUE_AUTO;
	for (unsigned cpu = 0; cpu < ncpus; cpu++) {
		pin_cpu(cpu);
		s.id = 200 + cpu;
		CHECK(submit(fd, &s) == 0);
		uint16_t q = completion(fd).queue;
		s.id = 300 + cpu;
		CHECK(submit(fd, &s) == 0);
		CHECK(completion(fd).queue == q); /* same CPU, same queue */
		seen |= 1u << q;
	}
	unpin_cpu();
	CHECK((unsigned)__builtin_popcount(seen) == (ncpus < 4 ? ncpus : 4));
	baseline(fd);
	close(fd);
}

static void batch_flush(void)
{
	int fd = open_dev(0);
	uint8_t *src = map_pages(8), *dst = map_pages(8);
	struct kdc_stats st;

	fill_random(src, 8 * PAGE, 5);
	for (int i = 0; i < 8; i++) {
		struct kdc_submit s = req(fd, i + 1, src + i * PAGE, dst + i * PAGE, PAGE);
		s.flags = KDC_SUBMIT_BATCH | (i & 1 ? KDC_SUBMIT_ZEROCOPY : 0);
		s.queue = i % 4;
		CHECK(submit(fd, &s) == 0);
	}
	st = stats(fd);
	CHECK(st.staged == 8 && st.device_owned == 0 && st.live == 8);
	CHECK(!wait_ready(fd, 50)); /* the device has not been told */
	CHECK(ioctl(fd, KDC_IOC_FLUSH) == 0);
	for (int i = 0; i < 8; i++)
		CHECK(completion(fd).status == 0);
	CHECK(memcmp(src, dst, 8 * PAGE) == 0);
	baseline(fd);
	close(fd);
}

/*
 * Interrupt coalescing (debugfs "coalesce" = usecs << 16 | count) as seen in the
 * device's own counters: a flushed batch rings one doorbell and, once the count
 * is reached, raises one interrupt that reaps all of it; a lone command waits
 * for the delay.
 */
static void coalescing(void)
{
	int fd = open_dev(0);
	uint8_t *src = map_pages(8), *dst = map_pages(8);
	struct kdc_completion cs[8];

	fill_random(src, 8 * PAGE, 9);
	CHECK(read_knob(0, "coalesce") == 0);
	knob(0, "coalesce", 50000UL << 16 | 8);
	unsigned long db = read_knob(0, "device_doorbells"), cq = read_knob(0, "device_cq_doorbells");
	unsigned long irqs = read_knob(0, "device_irqs");
	for (int i = 0; i < 8; i++) {
		struct kdc_submit s = req(fd, i + 1, src + i * PAGE, dst + i * PAGE, PAGE);
		s.flags = KDC_SUBMIT_BATCH;
		s.queue = 1;
		CHECK(submit(fd, &s) == 0);
	}
	CHECK(ioctl(fd, KDC_IOC_FLUSH) == 0);
	for (int i = 0; i < 2000 && stats(fd).ready < 8; i++)
		sleep_ms(1);
	CHECK(read(fd, cs, sizeof(cs)) == sizeof(cs));
	for (int i = 0; i < 8; i++)
		CHECK(cs[i].status == 0);
	CHECK(memcmp(src, dst, 8 * PAGE) == 0);
	CHECK(read_knob(0, "device_doorbells") == db + 1);
	/* one interrupt: the count was reached before the delay ran out */
	CHECK(read_knob(0, "device_irqs") == irqs + 1);
	CHECK(read_knob(0, "device_cq_doorbells") == cq + 1);

	struct kdc_submit one = req(fd, 9, src, dst, 64);
	one.queue = 1;
	long t0 = now_ms();
	CHECK(submit(fd, &one) == 0);
	struct kdc_completion c = completion(fd);
	long dt = now_ms() - t0;
	CHECK(c.status == 0 && dt >= 45 && dt < 2000); /* held back by the 50 ms delay */
	CHECK(read_knob(0, "device_irqs") == irqs + 2);
	knob(0, "coalesce", 0);
	baseline(fd);
	close(fd);
	say("  coalescing: batch of 8 in 1 interrupt, single command waited %ld ms", dt);
}

static uint64_t pfn_of(const void *addr)
{
	uint64_t entry = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY);
	CHECK(fd >= 0);
	CHECK(pread(fd, &entry, 8, (off_t)((uintptr_t)addr / PAGE * 8)) == 8);
	close(fd);
	CHECK(entry >> 63); /* present */
	return entry & ((1ull << 55) - 1);
}

/*
 * The destination is unmapped by the process right after submit. The driver's
 * pin must keep those physical pages from being freed while the device owns
 * them, so fresh mappings cannot receive them and the device's late write
 * cannot land in memory that now belongs to someone else. Page frame numbers
 * from /proc/self/pagemap (the test runs as root) make this directly visible.
 */
static void pinned_page_not_recycled(void)
{
	enum { PAGES = 16, CANARIES = 64 };
	int fd = open_dev(0);
	uint8_t *canary[CANARIES];
	uint64_t dst_pfn[PAGES];
	unsigned reused = 0;

	pin_cpu(0); /* freed pages go to this CPU's free list; refill from the same CPU */
	uint8_t *src = map_pages(PAGES), *dst = map_pages(PAGES);
	fill_random(src, PAGES * PAGE, 77);
	for (int i = 0; i < PAGES; i++)
		dst_pfn[i] = pfn_of(dst + i * PAGE);
	knob(0, "fault", FAULT_STALL); /* the device holds the command until we let go */
	struct kdc_submit s = req(fd, 1, src, dst, PAGES * PAGE);
	s.flags = KDC_SUBMIT_ZEROCOPY;
	s.timeout_ms = HOLD_MS;
	CHECK(submit(fd, &s) == 0);
	wait_device_owned(fd, 1);
	CHECK(munmap(dst, PAGES * PAGE) == 0);
	for (int i = 0; i < CANARIES; i++) {
		canary[i] = mmap(NULL, PAGES * PAGE, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
		CHECK(canary[i] != MAP_FAILED);
		memset(canary[i], 0x5A, PAGES * PAGE);
		for (int p = 0; p < PAGES; p++) {
			uint64_t pfn = pfn_of(canary[i] + p * PAGE);
			for (int d = 0; d < PAGES; d++)
				reused += pfn == dst_pfn[d];
		}
	}
	if (reused)
		say("  pinned_page_not_recycled: %u destination pages handed out again", reused);
	CHECK(reused == 0); /* a page the device still owns was recycled */
	CHECK(stats(fd).device_owned == 1);
	knob(0, "fault", 0);
	struct kdc_completion c = completion(fd);
	CHECK(c.status == 0);
	for (int i = 0; i < CANARIES; i++) {
		for (size_t b = 0; b < PAGES * PAGE; b++)
			CHECK(canary[i][b] == 0x5A); /* device wrote into a recycled page */
		munmap(canary[i], PAGES * PAGE);
	}
	munmap(src, PAGES * PAGE);
	unpin_cpu();
	baseline(fd);
	close(fd);
}

static void cancel_driver_owned(void)
{
	int fd = open_dev(0);
	uint8_t *src = map_pages(32), *dst = map_pages(32);
	uint64_t epoch = stats(fd).epoch;

	/* staged: never reached the ring */
	struct kdc_submit s = req(fd, 7, src, dst, 16 * PAGE);
	s.flags = KDC_SUBMIT_BATCH | KDC_SUBMIT_ZEROCOPY;
	CHECK(submit(fd, &s) == 0);
	CHECK(stats(fd).pinned_pages == 32);
	CHECK(cancel(fd, 7, epoch) == 0);
	CHECK(stats(fd).pinned_pages == 0 && stats(fd).staged == 0);
	CHECK(cancel(fd, 7, epoch) == 0); /* already cancelled */
	ERR(cancel(fd, 7, epoch + 1), ENOENT);
	struct kdc_completion c = completion(fd);
	CHECK(c.id == 7 && c.status == -ECANCELED && !(c.flags & KDC_COMPL_ABORTED));
	ERR(cancel(fd, 7, epoch), ENOENT);
	CHECK(ioctl(fd, KDC_IOC_FLUSH) == 0);
	CHECK(!wait_ready(fd, 20));

	/* pending: the ring is full, so the 64th command waits in the driver */
	knob(0, "fault", FAULT_STALL);
	for (int i = 0; i < 64; i++) {
		/* in-flight destinations must not overlap (EADDRINUSE) */
		s = req(fd, 100 + i, src + (i % 16) * 64, dst + i * 128, 64);
		s.queue = 2;
		s.timeout_ms = HOLD_MS;
		CHECK(submit(fd, &s) == 0);
	}
	struct kdc_stats st = stats(fd);
	CHECK(st.pending == 1 && st.device_owned == 63);
	CHECK(cancel(fd, 163, epoch) == 0);
	c = completion(fd);
	CHECK(c.id == 163 && c.status == -ECANCELED && c.hw_status == 0);
	knob(0, "fault", 0);
	CHECK(ioctl(fd, KDC_IOC_RESET) == 0);
	for (int i = 0; i < 63; i++) {
		c = completion(fd);
		CHECK(c.status == 0 || c.status == -ECONNRESET);
	}
	baseline(fd);
	close(fd);
}

static void cancel_device_owned(void)
{
	int fd = open_dev(0);
	uint8_t *src = map_pages(16), *dst = map_pages(16);
	uint64_t epoch = stats(fd).epoch;

	hold_aborts(true);
	knob(0, "service_ns", 4000000000UL);
	unsigned long aborted = read_knob(0, "device_aborted");
	struct kdc_submit s = req(fd, 9, src, dst, 16 * PAGE);
	s.flags = KDC_SUBMIT_ZEROCOPY;
	s.timeout_ms = HOLD_MS;
	long t0 = now_ms();
	CHECK(submit(fd, &s) == 0);
	wait_device_owned(fd, 1);
	wait_device_fetched(0, 1);
	struct kdc_stats before = stats(fd);
	CHECK(before.pinned_pages == 32 && before.dev_mapped_sgts >= 2);
	knob(0, "fault", 1); /* hold the abort inside the device */
	errno = 0;
	CHECK(cancel(fd, 9, epoch) == -1 && errno == EINPROGRESS);
	sleep_ms(50);
	struct kdc_stats after = stats(fd);
	/* the device has not answered: nothing may be unpinned or unmapped yet */
	CHECK(after.pinned_pages == 32 && after.dev_mapped_sgts == before.dev_mapped_sgts);
	CHECK(after.device_owned == 1 && !wait_ready(fd, 0));
	ERR(cancel(fd, 9, epoch), EINPROGRESS);
	knob(0, "fault", 0);
	struct kdc_completion c = completion(fd);
	CHECK(now_ms() - t0 < 3500); /* answered by the abort, not the 4 s service time */
	CHECK(c.status == -ECANCELED && c.hw_status == SC_ABORTED && (c.flags & KDC_COMPL_ABORTED));
	CHECK(read_knob(0, "device_aborted") == aborted + 1);
	knob(0, "service_ns", 0);
	hold_aborts(false);
	baseline(fd);
	close(fd);
}

struct flusher {
	int fd;
	atomic_bool stop;
};
static void *flush_loop(void *arg)
{
	struct flusher *f = arg;
	while (!atomic_load(&f->stop))
		CHECK(ioctl(f->fd, KDC_IOC_FLUSH) == 0);
	return NULL;
}

static void cancel_race(void)
{
	enum { ROUNDS = 1500 };
	int fd = open_dev(0);
	uint8_t *src = map_pages(4), *dst = map_pages(4);
	unsigned outcome[4] = { 0 }; /* cancelled early, aborted by device, completed, lost */
	struct flusher f = { .fd = fd };
	pthread_t thread;

	fill_random(src, 4 * PAGE, 3);
	knob(0, "service_ns", 20000);
	CHECK(pthread_create(&thread, NULL, flush_loop, &f) == 0);
	for (int i = 0; i < ROUNDS; i++) {
		uint32_t len = 1 + rnd() % (3 * PAGE);
		struct kdc_submit s = req(fd, i + 1, src, dst, len);
		s.flags = (i & 1 ? KDC_SUBMIT_ZEROCOPY : 0) | (i % 3 == 0 ? KDC_SUBMIT_BATCH : 0);
		memset(dst, 0, len);
		CHECK(submit(fd, &s) == 0);
		for (volatile unsigned spin = rnd() % 3000; spin; spin--)
			;
		errno = 0;
		int r = cancel(fd, s.id, s.epoch), e = errno;
		struct kdc_completion c = completion(fd);
		CHECK(c.id == s.id);
		if (r == 0) {
			CHECK(c.status == -ECANCELED);
			outcome[c.hw_status == SC_ABORTED ? 1 : 0]++;
		} else {
			CHECK(r == -1 && (e == EINPROGRESS || e == EALREADY));
			CHECK(c.status == 0 || (e == EINPROGRESS && c.status == -ECANCELED));
			outcome[c.status == 0 ? 2 : 1]++;
		}
		if (c.status == 0) {
			CHECK(c.crc == crc32c(src, len, 0));
			CHECK(memcmp(src, dst, len) == 0);
		}
	}
	/*
	 * The mix above depends on emulation speed (the slow ASan QEMU almost
	 * never aborts). Hold each of these device-owned with the stall knob
	 * before cancelling, so every configuration takes the abort branch.
	 */
	unsigned held = 0;
	hold_aborts(true);
	for (int i = 0; i < 40; i++) {
		uint32_t len = 1 + rnd() % (3 * PAGE);
		struct kdc_submit s = req(fd, 100000 + i, src, dst, len);
		s.flags = i & 1 ? KDC_SUBMIT_ZEROCOPY : 0;
		s.timeout_ms = HOLD_MS;
		knob(0, "fault", FAULT_STALL);
		CHECK(submit(fd, &s) == 0);
		wait_device_owned(fd, 1);
		wait_device_fetched(0, 1); /* an abort before the fetch is ignored */
		errno = 0;
		CHECK(cancel(fd, s.id, s.epoch) == -1 && errno == EINPROGRESS);
		knob(0, "fault", 0);
		struct kdc_completion c = completion(fd);
		CHECK(c.id == s.id && c.status == -ECANCELED && c.hw_status == SC_ABORTED);
		held++;
	}
	hold_aborts(false);
	atomic_store(&f.stop, true);
	CHECK(pthread_join(thread, NULL) == 0);
	knob(0, "service_ns", 0);
	say("  cancel_race: %u cancelled before the doorbell, %u aborted by the device, "
	    "%u completed first; %u more aborted while held device-owned",
	    outcome[0], outcome[1], outcome[2], held);
	/* Every outcome above was checked for consistency; the race itself must
	 * have produced more than one kind. */
	CHECK(!!outcome[0] + !!outcome[1] + !!outcome[2] >= 2);
	CHECK(held == 40);
	baseline(fd);
	close(fd);
}

struct closer {
	int fd;
	atomic_bool done;
};
static void *close_thread(void *arg)
{
	struct closer *c = arg;
	CHECK(close(c->fd) == 0);
	atomic_store(&c->done, true);
	return NULL;
}

static void close_with_inflight(void)
{
	int monitor = open_dev(0), fd = open_dev(0);
	uint8_t *src = map_pages(64), *dst = map_pages(64);
	struct closer c = { .fd = fd };
	pthread_t thread;

	hold_aborts(true); /* here close waits for the device's own answer */
	knob(0, "fault", FAULT_STALL); /* engine hangs: commands stay device-owned, aborts wait */
	for (int i = 0; i < 4; i++) {
		struct kdc_submit s = req(fd, i + 1, src + i * 16 * PAGE, dst + i * 16 * PAGE,
					  16 * PAGE);
		s.flags = KDC_SUBMIT_ZEROCOPY;
		s.timeout_ms = HOLD_MS;
		CHECK(submit(fd, &s) == 0);
	}
	wait_device_owned(fd, 4);
	CHECK(pthread_create(&thread, NULL, close_thread, &c) == 0);
	sleep_ms(200);
	struct kdc_stats st = stats(monitor);
	/* close must not return while the device still owns pinned buffers */
	CHECK(!atomic_load(&c.done));
	CHECK(st.dev_pinned_pages == 128 && st.dev_open_files == 2);
	knob(0, "fault", 0);
	CHECK(pthread_join(thread, NULL) == 0);
	CHECK(atomic_load(&c.done));
	st = stats(monitor);
	CHECK(st.dev_pinned_pages == 0 && st.dev_mapped_sgts == 0 && st.dev_open_files == 1);
	hold_aborts(false);
	baseline(monitor);
	close(monitor);
}

struct resetter {
	int fd, result;
};
static void *reset_thread(void *arg)
{
	struct resetter *r = arg;
	r->result = ioctl(r->fd, KDC_IOC_RESET);
	return NULL;
}

static void reset_semantics(void)
{
	int fd = open_dev(0);
	uint8_t *src = map_pages(4), *dst = map_pages(4);
	struct resetter r = { .fd = fd, .result = -2 };
	pthread_t thread;

	hold_aborts(true);
	knob(0, "fault", FAULT_STALL);
	struct kdc_submit a = req(fd, 1, src, dst, 100), b = req(fd, 2, src, dst + PAGE, 100);
	a.timeout_ms = HOLD_MS;
	CHECK(submit(fd, &a) == 0);
	b.flags = KDC_SUBMIT_BATCH;
	CHECK(submit(fd, &b) == 0);
	wait_device_owned(fd, 1);
	wait_device_fetched(0, 1); /* an abort before the fetch is ignored */
	CHECK(pthread_create(&thread, NULL, reset_thread, &r) == 0);
	for (int i = 0; i < 2000 && !stats(fd).resetting; i++)
		sleep_ms(1);
	CHECK(stats(fd).resetting);
	struct kdc_submit c2 = a;
	c2.id = 3;
	ERR(submit(fd, &c2), EBUSY);
	struct kdc_completion c = completion(fd); /* the staged one is released at once */
	CHECK(c.id == 2 && c.status == -ECONNRESET);
	CHECK(stats(fd).device_owned == 1 && r.result == -2);
	knob(0, "fault", 0);
	CHECK(pthread_join(thread, NULL) == 0);
	CHECK(r.result == 0);
	c = completion(fd);
	CHECK(c.id == 1 && c.epoch == a.epoch && c.status == -ECONNRESET);
	ERR(submit(fd, &c2), ESTALE);
	c2.epoch = stats(fd).epoch;
	CHECK(c2.epoch == a.epoch + 1);
	CHECK(submit(fd, &c2) == 0);
	CHECK(completion(fd).status == 0);
	hold_aborts(false);
	baseline(fd);
	close(fd);
}

static void quotas(void)
{
	int fd = open_dev(0);
	/* distinct destinations: concurrent device writes must not overlap */
	uint8_t *src = map_pages(65), *dst = map_pages(8 * 65);
	struct kdc_submit s = req(fd, 0, NULL, NULL, 0);

	s.flags = KDC_SUBMIT_NOP | KDC_SUBMIT_BATCH;
	for (int i = 0; i < KDC_MAX_CMDS_PER_FILE; i++) {
		s.id = i + 1;
		CHECK(submit(fd, &s) == 0);
	}
	s.id = 1000;
	ERR(submit(fd, &s), ENOSPC);
	CHECK(ioctl(fd, KDC_IOC_RESET) == 0);
	for (int i = 0; i < KDC_MAX_CMDS_PER_FILE; i++)
		CHECK(completion(fd).status == -ECONNRESET);
	baseline(fd);
	/* 8 x 256 KiB fills the 4 MiB src+dst byte quota */
	for (int i = 0; i < 8; i++) {
		s = req(fd, i + 1, src, dst + i * 65 * PAGE, KDC_MAX_LENGTH);
		s.flags = KDC_SUBMIT_BATCH | KDC_SUBMIT_ZEROCOPY;
		CHECK(submit(fd, &s) == 0);
	}
	CHECK(stats(fd).bytes == KDC_FILE_BYTE_QUOTA && stats(fd).pinned_pages == 8 * 128);
	s.id = 99;
	s.length = 1;
	ERR(submit(fd, &s), ENOSPC);
	CHECK(ioctl(fd, KDC_IOC_RESET) == 0);
	for (int i = 0; i < 8; i++)
		CHECK(completion(fd).status == -ECONNRESET);
	baseline(fd);
	close(fd);
}

static void file_isolation(void)
{
	int a = open_dev(0), b = open_dev(0);
	uint8_t *src = map_pages(2), *dst = map_pages(4);
	struct kdc_submit s = req(a, 55, src, dst, 100);

	s.flags = KDC_SUBMIT_BATCH;
	CHECK(submit(a, &s) == 0);
	ERR(cancel(b, 55, s.epoch), ENOENT);
	s.flags = 0;
	s.dst = (uintptr_t)(dst + 2 * PAGE);
	CHECK(submit(b, &s) == 0); /* same id, different file */
	CHECK(completion(b).status == 0);
	CHECK(close(a) == 0); /* staged command released with the file */
	struct kdc_stats st = stats(b);
	CHECK(st.dev_open_files == 1 && st.dev_pinned_pages == 0);
	baseline(b);
	close(b);
}

struct worker {
	int fd, index, rounds;
	bool shared_fd;
};
static void *worker_main(void *arg)
{
	struct worker *w = arg;
	enum { DEPTH = 8, SLOT = 70000 };
	uint8_t *src = map_pages(DEPTH * 18), *dst = map_pages(DEPTH * 18);
	uint32_t len[DEPTH] = { 0 };
	uint64_t ids[DEPTH] = { 0 };
	uint64_t seed = 0x1234567ull * (w->index + 1);
	int inflight = 0, next = 0, done = 0;
	unsigned busy = 0; /* completions arrive out of order across queues */

	while (done < w->rounds) {
		while (inflight < DEPTH && next < w->rounds) {
			int slot = __builtin_ctz(~busy);
			seed = seed * 6364136223846793005ull + 1442695040888963407ull;
			len[slot] = 1 + (seed >> 33) % 65536;
			fill_random(src + slot * SLOT, len[slot], seed);
			struct kdc_submit s = req(w->fd,
						  (uint64_t)w->index << 32 | (uint64_t)next << 8 | slot,
						  src + slot * SLOT, dst + slot * SLOT, len[slot]);
			s.flags = (seed >> 7) % 3 == 0 ? KDC_SUBMIT_ZEROCOPY :
				  (seed >> 7) % 3 == 1 && len[slot] <= bounce_max ? KDC_SUBMIT_BOUNCE : 0;
			s.queue = (seed >> 11) % 5 == 4 ? KDC_QUEUE_AUTO : (seed >> 11) % 5;
			ids[slot] = s.id;
			CHECK(submit(w->fd, &s) == 0);
			busy |= 1u << slot;
			inflight++;
			next++;
		}
		struct kdc_completion c = completion(w->fd);
		int slot = (int)(c.id & 0xff);
		CHECK(slot < DEPTH && (busy & (1u << slot)));
		CHECK(c.id == ids[slot] && c.status == 0 && c.length == len[slot]);
		busy &= ~(1u << slot);
		CHECK(c.crc == crc32c(src + slot * SLOT, len[slot], 0));
		CHECK(memcmp(src + slot * SLOT, dst + slot * SLOT, len[slot]) == 0);
		inflight--;
		done++;
	}
	munmap(src, DEPTH * 18 * PAGE);
	munmap(dst, DEPTH * 18 * PAGE);
	return NULL;
}

static void concurrency(void)
{
	enum { THREADS = 4, ROUNDS = 250 };
	pthread_t t[THREADS];
	struct worker w[THREADS];

	for (int i = 0; i < THREADS; i++) {
		w[i] = (struct worker){ .fd = open_dev(0), .index = i + 1, .rounds = ROUNDS };
		CHECK(pthread_create(&t[i], NULL, worker_main, &w[i]) == 0);
	}
	for (int i = 0; i < THREADS; i++) {
		CHECK(pthread_join(t[i], NULL) == 0);
		baseline(w[i].fd);
		close(w[i].fd);
	}
	say("  concurrency: %d threads x %d verified copies, own files", THREADS, ROUNDS);
}

struct shared {
	int fd;
	atomic_int submitted;
};
static void *shared_submitter(void *arg)
{
	struct shared *sh = arg;
	static _Thread_local uint8_t src[256], dst[16][256];
	struct kdc_submit s = req(sh->fd, 0, src, dst[0], sizeof(src));
	for (int i = 0; i < 200; i++) {
		s.id = (uint64_t)atomic_fetch_add(&sh->submitted, 1) + 1;
		s.dst = (uintptr_t)dst[i % 16];
		memset(src, (int)s.id, sizeof(src));
		while (submit(sh->fd, &s) != 0) {
			/* 64 unread per file, or the slot's previous command is still
			 * in flight: both clear once the reader catches up */
			CHECK(errno == ENOSPC || errno == EADDRINUSE);
			sleep_ms(1);
		}
	}
	return NULL;
}

static void shared_file(void)
{
	enum { THREADS = 4, TOTAL = THREADS * 200 };
	int fd = open_dev(0);
	struct shared sh = { .fd = fd };
	static uint8_t seen[TOTAL + 1];
	pthread_t t[THREADS];

	knob(0, "service_ns", 50000);
	for (int i = 0; i < THREADS; i++)
		CHECK(pthread_create(&t[i], NULL, shared_submitter, &sh) == 0);
	for (int n = 0; n < TOTAL; n++) {
		struct kdc_completion c = completion(fd);
		CHECK(c.id >= 1 && c.id <= TOTAL && !seen[c.id]);
		CHECK(c.status == 0);
		seen[c.id] = 1;
	}
	for (int i = 0; i < THREADS; i++)
		CHECK(pthread_join(t[i], NULL) == 0);
	knob(0, "service_ns", 0);
	baseline(fd);
	close(fd);
}

static void ring_overflow_all_queues(void)
{
	enum { FILES = 4, PER = 64 };
	int fds[FILES];
	uint8_t *src = map_pages(16), *dst = map_pages(FILES * PER);
	unsigned max_pending = 0;

	fill_random(src, 16 * PAGE, 11);
	knob(0, "fault", FAULT_STALL); /* engine hangs while we submit: nothing completes */
	for (int f = 0; f < FILES; f++) {
		fds[f] = open_dev(0);
		for (int i = 0; i < PER; i++) {
			struct kdc_submit s = req(fds[f], i + 1, src + (i % 15) * PAGE,
						  dst + (f * PER + i) * PAGE, PAGE);
			s.queue = f < 2 ? 0 : f - 1; /* two files share queue 0 */
			s.flags = i & 1 ? KDC_SUBMIT_ZEROCOPY : 0;
			s.timeout_ms = HOLD_MS;
			CHECK(submit(fds[f], &s) == 0);
		}
		struct kdc_stats st = stats(fds[f]);
		if (st.pending > max_pending)
			max_pending = st.pending;
	}
	knob(0, "fault", 0);
	for (int f = 0; f < FILES; f++) {
		for (int i = 0; i < PER; i++) {
			struct kdc_completion c = completion(fds[f]);
			CHECK(c.status == 0);
			int idx = (int)c.id - 1;
			CHECK(memcmp(dst + (f * PER + idx) * PAGE, src + (idx % 15) * PAGE, PAGE) == 0);
		}
		baseline(fds[f]);
		close(fds[f]);
	}
	CHECK(max_pending == PER); /* the second file on queue 0 waited entirely in the driver */
}

static void second_device(void)
{
	int a = open_dev(0), b = open_dev(1);
	struct kdc_stats sa = stats(a), sb = stats(b);
	CHECK(sb.dev_open_files == 1 && sa.dev_open_files == 1);
	close(a);
	check_copy(b, 10000, 5, 7, 0, 0);
	check_copy(b, 100, 0, 0, 0, 0);
	CHECK(stats(b).dev_submitted == sb.dev_submitted + 2);
	baseline(b);
	close(b);
}

static void unbind_rebind(void)
{
	char link[256], *bdf;
	ssize_t n = readlink("/sys/class/kdc/kdc0/device", link, sizeof(link) - 1);
	struct stat st;

	CHECK(n > 0);
	link[n] = 0;
	bdf = strrchr(link, '/') + 1;
	write_file("/sys/bus/pci/drivers/kdc/unbind", bdf);
	CHECK(stat("/dev/kdc0", &st) != 0 && errno == ENOENT);
	write_file("/sys/bus/pci/drivers/kdc/bind", bdf);
	for (int i = 0; i < 1000 && stat("/dev/kdc0", &st) != 0; i++)
		sleep_ms(1);
	int fd = open_dev(0);
	check_copy(fd, 5000, 1, 2, 0, 0);
	check_copy(fd, 70000, 1, 2, 0, 0);
	baseline(fd);
	close(fd);
}

/* ---- timeouts and recovery ---- */

static struct kdc_submit held(int fd, uint64_t id, const void *src, void *dst, uint32_t len,
			      uint32_t flags, uint32_t timeout_ms, uint16_t queue)
{
	struct kdc_submit s = req(fd, id, src, dst, len);
	s.flags = flags;
	s.timeout_ms = timeout_ms;
	s.queue = queue;
	return s;
}

static void check_works(int dev)
{
	int fd = open_dev(dev);
	check_copy(fd, 5000, 3, 5, 0, 0);
	check_copy(fd, 70000, 1, 7, KDC_SUBMIT_ZEROCOPY, 0);
	baseline(fd);
	close(fd);
}

/* A slow command: the timeout's abort is honoured, no reset is needed. */
static void timeout_abort(void)
{
	int fd = open_dev(0);
	uint8_t *src = map_pages(16), *dst = map_pages(16);
	struct kdc_stats before = stats(fd);

	knob(0, "service_ns", 2000000000UL);
	struct expect e = expect_begin("timeout-abort", "timed out after");
	struct kdc_submit s = held(fd, 1, src, dst, 16 * PAGE, KDC_SUBMIT_ZEROCOPY, 200, 1);
	long t0 = now_ms();
	CHECK(submit(fd, &s) == 0);
	struct kdc_completion c = completion(fd);
	long dt = now_ms() - t0;
	CHECK(c.status == -ETIMEDOUT && c.hw_status == SC_ABORTED);
	CHECK((c.flags & KDC_COMPL_ABORTED) && !(c.flags & KDC_COMPL_RESET));
	/* answered by the abort, not the 2 s service; deadlines are whole jiffies */
	CHECK(dt >= 180 && dt < 1500);
	expect_seen(&e, "timed out after 200 ms");
	expect_end(&e);
	struct kdc_stats st = stats(fd);
	CHECK(st.dev_timeouts == before.dev_timeouts + 1 && st.dev_resets == before.dev_resets);
	knob(0, "service_ns", 0);
	baseline(fd);
	close(fd);
	say("  timeout_abort: answered after %ld ms", dt);
}

/* A hung engine: the abort is ignored, a controller reset ends the command. */
static void timeout_reset(void)
{
	int fd = open_dev(0);
	uint8_t *src = map_pages(16), *dst = map_pages(16);
	struct kdc_stats before = stats(fd);

	fill_random(src, 16 * PAGE, 21);
	knob(0, "fault", FAULT_STALL);
	struct expect e = expect_begin("timeout-reset", "timed out after|resetting controller");
	struct kdc_submit a = held(fd, 1, src, dst, 8 * PAGE, KDC_SUBMIT_ZEROCOPY, 200, 0);
	struct kdc_submit b = held(fd, 2, src + 8 * PAGE, dst + 8 * PAGE, 1000, 0, HOLD_MS, 1);
	long t0 = now_ms();
	CHECK(submit(fd, &a) == 0 && submit(fd, &b) == 0);
	wait_device_owned(fd, 2);
	CHECK(stats(fd).pinned_pages == 16 + 1); /* bounce pins only its dst */
	/* both must end: one by the reset, one reissued after it */
	CHECK(wait_ready(fd, 10000));
	struct kdc_completion ca = completion(fd);
	CHECK(wait_ready(fd, 10000));
	struct kdc_completion cb = completion(fd);
	long dt = now_ms() - t0;
	if (ca.id == 2) {
		struct kdc_completion t = ca;
		ca = cb;
		cb = t;
	}
	/* the timed-out command ends with the reset; abort grace is 500 ms */
	CHECK(ca.id == 1 && ca.status == -ETIMEDOUT && (ca.flags & KDC_COMPL_RESET));
	CHECK(dt >= 600 && dt < 4000);
	/* the other one was reissued after the reset and completed */
	CHECK(cb.id == 2 && cb.status == 0 && !(cb.flags & KDC_COMPL_RESET));
	CHECK(cb.crc == crc32c(src + 8 * PAGE, 1000, 0));
	CHECK(memcmp(dst + 8 * PAGE, src + 8 * PAGE, 1000) == 0);
	expect_seen(&e, "resetting controller");
	expect_end(&e);
	struct kdc_stats st = stats(fd);
	CHECK(st.dev_resets == before.dev_resets + 1 && st.dev_timeouts == before.dev_timeouts + 1);
	CHECK(st.dev_requeued >= before.dev_requeued + 1 && st.dev_state == KDC_STATE_LIVE);
	CHECK(read_knob(0, "fault") == 0); /* the reset cleared the stall */
	baseline(fd);
	close(fd);
	check_works(0);
	say("  timeout_reset: ended after %ld ms, %llu command reissued", dt,
	    (unsigned long long)(st.dev_requeued - before.dev_requeued));
}

/*
 * The controller also ignores its reset: recovery escalates to a function-level
 * reset. While the controller reset is being attempted the device can still
 * DMA, so the command's buffers must stay pinned and mapped until the FLR.
 */
static void timeout_escalation(void)
{
	int fd = open_dev(0);
	uint8_t *src = map_pages(16), *dst = map_pages(16);
	struct kdc_stats before = stats(fd);

	knob(0, "fault", FAULT_STALL | FAULT_RESET_HANG);
	struct expect e = expect_begin("escalation",
				       "timed out after|resetting controller|controller reset failed");
	struct kdc_submit s = held(fd, 1, src, dst, 16 * PAGE, KDC_SUBMIT_ZEROCOPY, 200, 2);
	CHECK(submit(fd, &s) == 0);
	struct kdc_stats st = wait_state(fd, KDC_STATE_RECOVERING, 5000);
	CHECK(st.dev_state == KDC_STATE_RECOVERING);
	/* the controller reset is in progress and has not been acknowledged */
	CHECK(st.pinned_pages == 32 && st.dev_mapped_sgts >= 2 && st.device_owned == 1);
	CHECK(!wait_ready(fd, 0));
	struct kdc_completion c = completion(fd);
	CHECK(c.status == -ETIMEDOUT && (c.flags & KDC_COMPL_RESET));
	/*
	 * reset_done() ends the command before it restarts the controller, so
	 * the completion can be read while the device is still recovering.
	 */
	st = wait_state(fd, KDC_STATE_LIVE, 5000);
	CHECK(st.dev_flrs == before.dev_flrs + 1 && st.dev_resets == before.dev_resets);
	CHECK(st.dev_state == KDC_STATE_LIVE);
	expect_seen(&e, "controller reset failed");
	expect_end(&e);
	CHECK(read_knob(0, "fault") == 0); /* the FLR cleared both faults */
	baseline(fd);
	close(fd);
	check_works(0);
}

/*
 * The device never becomes ready again: controller reset and FLR both fail,
 * bus mastering goes off and every command ends. A later reset from sysfs,
 * once the fault is gone, brings the device back through the same callbacks.
 */
static void recovery_failure(void)
{
	int dev = kdc_index(BDF1), fd = open_dev(dev);
	uint8_t *src = map_pages(8), *dst = map_pages(8);
	struct kdc_stats before = stats(fd);
	char path[96];

	knob(dev, "fault", FAULT_STALL | FAULT_STUCK);
	struct expect e = expect_begin("failure", "timed out after|resetting controller|"
					"controller reset failed|recovery failed");
	struct kdc_submit a = held(fd, 1, src, dst, 4 * PAGE, KDC_SUBMIT_ZEROCOPY, 200, 0);
	struct kdc_submit b = held(fd, 2, src, dst + 4 * PAGE, 100, 0, HOLD_MS, 1);
	CHECK(submit(fd, &a) == 0 && submit(fd, &b) == 0);
	struct kdc_completion c1 = completion(fd), c2 = completion(fd);
	if (c1.id == 2) {
		struct kdc_completion t = c1;
		c1 = c2;
		c2 = t;
	}
	CHECK(c1.id == 1 && c1.status == -ETIMEDOUT && (c1.flags & KDC_COMPL_RESET));
	CHECK(c2.id == 2 && c2.status == -EIO && (c2.flags & KDC_COMPL_RESET));
	struct kdc_stats st = stats(fd);
	CHECK(st.dev_state == KDC_STATE_FAILED && st.dev_failures == before.dev_failures + 1);
	CHECK(st.dev_pinned_pages == 0 && st.dev_mapped_sgts == 0);
	struct kdc_submit n = req(fd, 3, src, dst, 64);
	ERR(submit(fd, &n), EIO);
	expect_seen(&e, "recovery failed");
	expect_end(&e);

	knob(dev, "fault", 0);
	e = expect_begin("revive", "working again");
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/reset", BDF1);
	write_file(path, "1");
	st = stats(fd);
	CHECK(st.dev_state == KDC_STATE_LIVE && st.dev_flrs == before.dev_flrs + 1);
	expect_seen(&e, "working again");
	expect_end(&e);
	CHECK(submit(fd, &n) == 0 && completion(fd).status == 0);
	baseline(fd);
	close(fd);
	check_works(dev);
}

struct loader {
	int fd, dev, index;
	atomic_bool stop;
	unsigned done, rounds;
};

/* Keeps 4 verified copies in flight until told to stop. */
static void *load_main(void *arg)
{
	struct loader *l = arg;
	enum { DEPTH = 4, SLOT = 3 * PAGE };
	uint8_t *src = map_pages(DEPTH * 3), *dst = map_pages(DEPTH * 3);
	uint32_t len[DEPTH];
	unsigned busy = 0, next = 0;

	fill_random(src, DEPTH * SLOT, l->index + 77);
	while (!atomic_load(&l->stop) || busy) {
		while (!atomic_load(&l->stop) && __builtin_popcount(busy) < DEPTH) {
			int slot = __builtin_ctz(~busy);
			len[slot] = 1 + (next * 7919) % (2 * PAGE);
			memset(dst + slot * SLOT, 0, len[slot]);
			struct kdc_submit s = req(l->fd, (uint64_t)next << 4 | slot, src + slot * SLOT,
						  dst + slot * SLOT, len[slot]);
			s.flags = next % 3 == 0 ? KDC_SUBMIT_ZEROCOPY : 0;
			s.queue = next % 4 < 2 ? KDC_QUEUE_AUTO : next % 2;
			CHECK(submit(l->fd, &s) == 0);
			busy |= 1u << slot;
			next++;
		}
		struct kdc_completion c = completion(l->fd);
		int slot = c.id & 15;
		CHECK(slot < DEPTH && (busy & (1u << slot)));
		if (c.status)
			say("  load_main: command %llx ended with %d, flags %#x",
			    (unsigned long long)c.id, c.status, c.flags);
		CHECK(c.status == 0 && c.length == len[slot]);
		CHECK(c.crc == crc32c(src + slot * SLOT, len[slot], 0));
		CHECK(memcmp(dst + slot * SLOT, src + slot * SLOT, len[slot]) == 0);
		busy &= ~(1u << slot);
		l->done++;
	}
	munmap(src, DEPTH * SLOT);
	munmap(dst, DEPTH * SLOT);
	return NULL;
}

/* Resets started from sysfs while commands run: they are reissued and complete. */
static void sysfs_reset_under_load(void)
{
	enum { THREADS = 2, RESETS = 3 };
	struct loader l[THREADS];
	pthread_t t[THREADS];
	char path[96];
	int mon = open_dev(0);
	struct kdc_stats before = stats(mon);

	knob(0, "service_ns", 300000);
	for (int i = 0; i < THREADS; i++) {
		l[i] = (struct loader){ .fd = open_dev(0), .index = i };
		CHECK(pthread_create(&t[i], NULL, load_main, &l[i]) == 0);
	}
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/reset", BDF0);
	for (int r = 0; r < RESETS; r++) {
		while (stats(mon).dev_completed < before.dev_completed + 20 * (r + 1))
			sleep_ms(2);
		/* hold commands inside the device so the reset catches some; the
		 * function reset clears the stall */
		knob(0, "fault", FAULT_STALL);
		wait_device_fetched(0, 1);
		write_file(path, "1");
		CHECK(read_knob(0, "fault") == 0);
	}
	sleep_ms(50);
	unsigned total = 0;
	for (int i = 0; i < THREADS; i++) {
		atomic_store(&l[i].stop, true);
		CHECK(pthread_join(t[i], NULL) == 0);
		baseline(l[i].fd);
		close(l[i].fd);
		total += l[i].done;
	}
	knob(0, "service_ns", 0);
	struct kdc_stats st = stats(mon);
	CHECK(st.dev_flrs == before.dev_flrs + RESETS && st.dev_state == KDC_STATE_LIVE);
	CHECK(st.dev_requeued > before.dev_requeued); /* reissued, and still verified above */
	close(mon);
	say("  sysfs_reset_under_load: %d resets, %u verified copies, %llu reissued", RESETS, total,
	    (unsigned long long)(st.dev_requeued - before.dev_requeued));
}

/* The device enters its fatal state under load: the error interrupt starts a
 * controller reset and every command completes after it. */
static void fatal_recovery(void)
{
	int a = open_dev(0), b = open_dev(0);
	uint8_t *src = map_pages(32), *dst = map_pages(32);
	struct kdc_stats before = stats(a);

	fill_random(src, 32 * PAGE, 31);
	knob(0, "service_ns", 5000000);
	knob(0, "fault", FAULT_STALL); /* the fault must catch commands inside the device */
	struct expect e = expect_begin("fatal", "device error|resetting controller");
	for (int i = 0; i < 16; i++) {
		struct kdc_submit s = req(i & 1 ? b : a, i + 1, src + i * 2 * PAGE, dst + i * 2 * PAGE,
					  PAGE + 100 * i);
		s.flags = i % 3 == 0 ? KDC_SUBMIT_ZEROCOPY : 0;
		s.queue = i % 4;
		CHECK(submit(i & 1 ? b : a, &s) == 0);
	}
	wait_device_fetched(0, 4);
	knob(0, "fault", FAULT_STALL | FAULT_FATAL);
	for (int i = 0; i < 16; i++) {
		int fd = i & 1 ? b : a;
		struct kdc_completion c = completion(fd);
		int k = (int)c.id - 1;
		CHECK(c.status == 0 && c.length == PAGE + 100u * k);
		CHECK(memcmp(dst + k * 2 * PAGE, src + k * 2 * PAGE, c.length) == 0);
	}
	expect_seen(&e, "resetting controller");
	expect_end(&e);
	knob(0, "service_ns", 0);
	struct kdc_stats st = stats(a);
	CHECK(st.dev_resets == before.dev_resets + 1 && st.dev_errors >= before.dev_errors + 1);
	CHECK(st.dev_timeouts == before.dev_timeouts && st.dev_requeued >= before.dev_requeued + 4);
	baseline(a);
	baseline(b);
	close(a);
	close(b);
	say("  fatal_recovery: 16 commands completed, %llu reissued after the reset",
	    (unsigned long long)(st.dev_requeued - before.dev_requeued));
}

/* A dropped completion and a corrupted cid look the same to the driver: the
 * command times out, the device no longer knows it, a reset ends it. */
static void lost_completions(void)
{
	int fd = open_dev(0);
	uint8_t *src = map_pages(4), *dst = map_pages(4);
	struct kdc_stats before = stats(fd);
	static const char *const knobs[] = { "fault_drop", "fault_bad_cid" };

	struct expect e = expect_begin("lost", "timed out after|resetting controller|"
				       "completion for unknown cid");
	for (int k = 0; k < 2; k++) {
		knob(0, knobs[k], 1);
		struct kdc_submit s = held(fd, k + 1, src, dst + k * 2 * PAGE, 3000, 0, 300, 0);
		CHECK(submit(fd, &s) == 0);
		struct kdc_completion c = completion(fd);
		CHECK(c.id == (uint64_t)k + 1 && c.status == -ETIMEDOUT && (c.flags & KDC_COMPL_RESET));
		/*
		 * The reset ends the command before it re-enables the controller,
		 * and re-enabling clears the knobs: wait until it is done.
		 */
		CHECK(wait_state(fd, KDC_STATE_LIVE, 5000).dev_state == KDC_STATE_LIVE);
	}
	expect_seen(&e, "completion for unknown cid");
	expect_end(&e);
	struct kdc_stats st = stats(fd);
	CHECK(st.dev_bad_completions == before.dev_bad_completions + 1);
	CHECK(st.dev_timeouts == before.dev_timeouts + 2 && st.dev_resets == before.dev_resets + 2);
	baseline(fd);
	close(fd);
}

/* close() and KDC_IOC_RESET on a hung device return after the abort grace and
 * a reset, not never; buffers come back only after the reset. */
static void bounded_close_and_reset(void)
{
	int mon = open_dev(0), fd = open_dev(0);
	uint8_t *src = map_pages(64), *dst = map_pages(64);
	struct kdc_stats before = stats(mon);
	struct closer cl = { .fd = fd };
	pthread_t thread;

	knob(0, "fault", FAULT_STALL);
	struct expect e = expect_begin("bounded", "resetting controller");
	for (int i = 0; i < 4; i++) {
		struct kdc_submit s = held(fd, i + 1, src + i * 16 * PAGE, dst + i * 16 * PAGE,
					   16 * PAGE, KDC_SUBMIT_ZEROCOPY, HOLD_MS, i);
		CHECK(submit(fd, &s) == 0);
	}
	wait_device_owned(fd, 4);
	long t0 = now_ms();
	CHECK(pthread_create(&thread, NULL, close_thread, &cl) == 0);
	sleep_ms(200);
	CHECK(!atomic_load(&cl.done) && stats(mon).dev_pinned_pages == 128);
	for (int i = 0; i < 500 && !atomic_load(&cl.done); i++)
		sleep_ms(10);
	CHECK(atomic_load(&cl.done)); /* close returned within 5 s */
	long dt_close = now_ms() - t0;
	CHECK(pthread_join(thread, NULL) == 0);
	/* close returns once the reset has ended the commands, maybe before it restarts the device */
	struct kdc_stats st = wait_state(mon, KDC_STATE_LIVE, 5000);
	CHECK(st.dev_pinned_pages == 0 && st.dev_resets == before.dev_resets + 1);

	fd = open_dev(0);
	knob(0, "fault", FAULT_STALL);
	for (int i = 0; i < 2; i++) {
		struct kdc_submit s = held(fd, i + 1, src + i * PAGE, dst + i * PAGE, 200, 0, HOLD_MS, i);
		CHECK(submit(fd, &s) == 0);
	}
	wait_device_owned(fd, 2);
	t0 = now_ms();
	CHECK(ioctl(fd, KDC_IOC_RESET) == 0);
	long dt_reset = now_ms() - t0;
	CHECK(dt_reset < 5000);
	for (int i = 0; i < 2; i++) {
		struct kdc_completion c = completion(fd);
		CHECK(c.status == -ECONNRESET && (c.flags & KDC_COMPL_RESET));
	}
	CHECK(wait_state(mon, KDC_STATE_LIVE, 5000).dev_state == KDC_STATE_LIVE);
	expect_seen(&e, "resetting controller");
	expect_end(&e);
	baseline(fd);
	close(fd);
	close(mon);
	say("  bounded_close_and_reset: close returned after %ld ms, reset after %ld ms", dt_close,
	    dt_reset);
}

/* ---- DMA faults ---- */

/*
 * The device's write goes to an address nobody mapped. With the IOMMU that is
 * a DMAR fault the test expects; without it the write fails in the bus. Either
 * way the command ends with -EIO, nothing reaches the real destination, and
 * its buffers stay pinned until the device has completed.
 */
static void dma_fault(void)
{
	int fd = open_dev(0);
	uint8_t *src = map_pages(8), *dst = map_pages(8);
	bool iommu = iommu_active();
	static const uint32_t flags[] = { KDC_SUBMIT_ZEROCOPY, KDC_SUBMIT_BOUNCE };

	knob(0, "service_ns", 100000000); /* 100 ms in flight before the write */
	for (int i = 0; i < 2; i++) {
		struct expect e = { 0 };
		memset(dst, 0xEE, 8 * PAGE);
		if (iommu)
			e = expect_begin(i ? "dmar-bounce" : "dmar-zerocopy",
					 "DMAR: (\\[DMA Write|DRHD: handling fault)");
		knob(0, "fault_bad_dma", 1);
		uint32_t len = i ? 2000 : 4 * PAGE;
		struct kdc_submit s = req(fd, i + 1, src, dst + 100, len);
		s.flags = flags[i];
		CHECK(submit(fd, &s) == 0);
		struct kdc_stats st = stats(fd);
		CHECK(st.device_owned == 1 && st.pinned_pages >= 1);
		struct kdc_completion c = completion(fd);
		CHECK(c.status == -EIO && c.hw_status == SC_DATA_ERROR && !(c.flags & KDC_COMPL_RESET));
		for (size_t b = 0; b < 8 * PAGE; b++)
			CHECK(dst[b] == 0xEE);
		if (iommu) {
			expect_seen(&e, "DMAR: [DMA Write");
			expect_end(&e);
		}
		baseline(fd);
	}
	knob(0, "service_ns", 0);
	close(fd);
	check_works(0);
	say("  dma_fault: 2 faulting copies ended with -EIO (%s)", iommu ? "IOMMU fault reported" :
							    "no IOMMU");
}

/* ---- destination overlap rule ---- */

static int sub(int fd, uint64_t id, const void *src, void *dst, uint32_t len, uint32_t flags)
{
	struct kdc_submit s = held(fd, id, src, dst, len, flags, HOLD_MS, KDC_QUEUE_AUTO);
	return submit(fd, &s);
}

static void dst_overlap(void)
{
	int a = open_dev(0), b = open_dev(0), other = open_dev(1);
	uint8_t *src = map_pages(8), *dst = map_pages(16);
	struct kdc_stats before = stats(a);
	int refused = 0;

	fill_random(src, 8 * PAGE, 41);
	memset(dst, 0xEE, 16 * PAGE);
	knob(0, "fault", FAULT_STALL);
	knob(1, "fault", FAULT_STALL);
	/* checked before anything is mapped, bounce or zero-copy alike */
	CHECK(sub(a, 1, src, dst, 100, KDC_SUBMIT_BOUNCE) == 0);                  /* [0,100) */
	ERR(sub(a, 2, src, dst + 99, 100, KDC_SUBMIT_BOUNCE), EADDRINUSE);
	refused++;
	CHECK(sub(a, 3, src + 100, dst + 100, 100, KDC_SUBMIT_BOUNCE) == 0);      /* adjacent */
	CHECK(sub(a, 4, src + 200, dst + 200, 9000, KDC_SUBMIT_ZEROCOPY) == 0);   /* adjacent */
	ERR(sub(a, 5, src, dst + 4096, 64, KDC_SUBMIT_ZEROCOPY), EADDRINUSE);
	refused++;
	ERR(sub(b, 5, src, dst + 9199, 1, KDC_SUBMIT_BOUNCE), EADDRINUSE);
	refused++;
	ERR(sub(other, 5, src, dst + 300, 5000, KDC_SUBMIT_ZEROCOPY), EADDRINUSE);
	CHECK(sub(b, 6, src, dst + 9200, 50, KDC_SUBMIT_ZEROCOPY) == 0);          /* adjacent */
	/* the same physical page through a second mapping (mremap with old size 0) */
	uint8_t *m1 = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(m1 != MAP_FAILED);
	uint8_t *m2 = mremap(m1, 0, 2 * PAGE, MREMAP_MAYMOVE);
	CHECK(m2 != MAP_FAILED && m1 != m2);
	memset(m1, 0xEE, 2 * PAGE);
	CHECK(sub(b, 7, src, m1 + 10, 3000, KDC_SUBMIT_ZEROCOPY) == 0);
	ERR(sub(b, 8, src, m2 + 3000, 100, KDC_SUBMIT_BOUNCE), EADDRINUSE);
	refused++;
	ERR(sub(other, 8, src, m2 + 10, 3000, KDC_SUBMIT_ZEROCOPY), EADDRINUSE);
	/* one destination that maps a physical page twice */
	uint8_t *win = mmap(NULL, 2 * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(win != MAP_FAILED);
	CHECK(mremap(m1 + PAGE, 0, PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, win) == win);
	CHECK(mremap(m1 + PAGE, 0, PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, win + PAGE) == win + PAGE);
	ERR(sub(b, 9, src, win + 2000, PAGE + 100, KDC_SUBMIT_ZEROCOPY), EADDRINUSE);
	refused++;
	CHECK(stats(a).dev_overlaps == before.dev_overlaps + (unsigned)refused);
	CHECK(stats(a).device_owned == 3 && stats(b).device_owned == 2);
	knob(0, "fault", 0);
	knob(1, "fault", 0);
	for (int i = 0; i < 5; i++) {
		int fd = i < 3 ? a : b;
		struct kdc_completion c = completion(fd);
		CHECK(c.status == 0);
	}
	CHECK(memcmp(dst, src, 100) == 0 && memcmp(dst + 100, src + 100, 100) == 0);
	CHECK(memcmp(dst + 200, src + 200, 9000) == 0 && memcmp(dst + 9200, src, 50) == 0);
	for (size_t i = 9250; i < 16 * PAGE; i++)
		CHECK(dst[i] == 0xEE);
	CHECK(memcmp(m2 + 10, src, 3000) == 0 && m2[9] == 0xEE && m2[3010] == 0xEE);
	/* refused only while the other command was in flight */
	CHECK(sub(a, 2, src, dst + 99, 100, KDC_SUBMIT_BOUNCE) == 0);
	CHECK(completion(a).status == 0 && dst[99] == src[0]);
	baseline(a);
	baseline(b);
	close(a);
	close(b);
	close(other);
	globals_idle();
	say("  dst_overlap: %d overlapping submits refused, 6 adjacent ones completed", refused + 2);
}

/*
 * Destinations packed back to back at odd offsets share cache lines but no
 * bytes; they must all run at once, mixed zero-copy and bounce, without a
 * DMA-API report, and every byte must come out right.
 */
static void adjacent_destinations(void)
{
	enum { CMDS = 96, BIG = 6 };
	int f[2] = { open_dev(0), open_dev(0) };
	uint8_t *src = map_pages(64), *dst = map_pages(100);
	uint32_t off[CMDS + 1], len[CMDS];

	fill_random(src, 64 * PAGE, 51);
	memset(dst, 0xEE, 100 * PAGE);
	off[0] = 13;
	for (int i = 0; i < CMDS; i++) {
		len[i] = 1 + rnd() % 300;
		off[i + 1] = off[i] + len[i];
	}
	knob(0, "fault", FAULT_STALL);
	for (int i = 0; i < CMDS; i++) {
		struct kdc_submit s = held(f[i & 1], i + 1, src + i * 300, dst + off[i], len[i],
					   i % 3 ? KDC_SUBMIT_ZEROCOPY : 0, HOLD_MS, i % 4);
		CHECK(submit(f[i & 1], &s) == 0);
	}
	CHECK(stats(f[0]).device_owned == CMDS / 2 && stats(f[1]).device_owned == CMDS / 2);
	knob(0, "fault", 0);
	for (int i = 0; i < CMDS; i++)
		CHECK(completion(f[i & 1]).status == 0);
	for (int i = 0; i < CMDS; i++)
		CHECK(memcmp(dst + off[i], src + i * 300, len[i]) == 0);
	CHECK(dst[12] == 0xEE && dst[off[CMDS]] == 0xEE);
	/* large zero-copy destinations back to back, unaligned */
	uint32_t base = 40 * PAGE + 7, big = 30001;
	knob(0, "fault", FAULT_STALL);
	for (int i = 0; i < BIG; i++) {
		struct kdc_submit s = held(f[i & 1], 1000 + i, src + i * PAGE, dst + base + i * big,
					   big, KDC_SUBMIT_ZEROCOPY, HOLD_MS, i % 4);
		CHECK(submit(f[i & 1], &s) == 0);
	}
	knob(0, "fault", 0);
	for (int i = 0; i < BIG; i++)
		CHECK(completion(f[i & 1]).status == 0);
	for (int i = 0; i < BIG; i++)
		CHECK(memcmp(dst + base + i * big, src + i * PAGE, big) == 0);
	CHECK(dst[base - 1] == 0xEE && dst[base + BIG * big] == 0xEE);
	CHECK(read_ul("/sys/kernel/debug/dma-api/error_count") == 0);
	baseline(f[0]);
	baseline(f[1]);
	close(f[0]);
	close(f[1]);
	say("  adjacent_destinations: %d small and %d large adjacent copies in flight together", CMDS,
	    BIG);
}

/* ---- fault injection on the submit and probe error paths ---- */

static int fail_nth_open(void)
{
	int fd = open("/proc/thread-self/fail-nth", O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0);
	return fd;
}

static void fail_nth_arm(int fd, int n)
{
	char text[16];
	int len = snprintf(text, sizeof(text), "%d", n);
	CHECK(pwrite(fd, text, len, 0) == len);
}

/* true if the armed failure was injected */
static bool fail_nth_fired(int fd)
{
	char text[16] = { 0 };
	CHECK(pread(fd, text, sizeof(text) - 1, 0) > 0);
	bool fired = atoi(text) == 0;
	fail_nth_arm(fd, 0);
	return fired;
}

/* [start, end) of a kernel function, from /proc/kallsyms (the test runs as root). */
static void symbol_range(const char *name, unsigned long *start, unsigned long *end)
{
	FILE *f = fopen("/proc/kallsyms", "r");
	char line[256], sym[128], type;
	unsigned long addr;

	CHECK(f);
	*start = 0;
	*end = ~0UL;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%lx %c %127s", &addr, &type, sym) == 3 && !strcmp(sym, name))
			*start = addr;
	CHECK(*start);
	rewind(f);
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%lx %c %127s", &addr, &type, sym) == 3 && addr > *start && addr < *end)
			*end = addr;
	fclose(f);
}

/*
 * Never fail allocations under register_irq_proc(): the IRQ core tolerates a
 * missing /proc/irq/N entry at request_irq() time and then WARNs in
 * remove_proc_entry() when the IRQ is freed. That is a core-kernel limitation
 * this test found, not a driver path.
 */
static void fail_reject(bool on)
{
	static const char *const attrs[] = { "failslab", "fail_page_alloc" };
	unsigned long start = 0, end = 0;
	char path[96], text[32];

	if (on)
		symbol_range("register_irq_proc", &start, &end);
	for (int i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "/sys/kernel/debug/%s/reject-start", attrs[i]);
		snprintf(text, sizeof(text), "%#lx", start);
		write_file(path, text);
		snprintf(path, sizeof(path), "/sys/kernel/debug/%s/reject-end", attrs[i]);
		snprintf(text, sizeof(text), "%#lx", end);
		write_file(path, text);
	}
}

static void fault_injection(void)
{
	int nth = fail_nth_open(), fd = open_dev(0), injected = 0, n;
	uint8_t *src = map_pages(8), *dst = map_pages(8);
	static const uint32_t flags[] = { KDC_SUBMIT_ZEROCOPY, KDC_SUBMIT_BOUNCE };
	char unbind[] = "/sys/bus/pci/drivers/kdc/unbind", bind[] = "/sys/bus/pci/drivers/kdc/bind";

	fill_random(src, 8 * PAGE, 61);
	for (int k = 0; k < 2; k++) {
		for (n = 1; n < 1000; n++) {
			struct kdc_submit s = req(fd, n, src + 5, dst + 9, k ? 2000 : 5 * PAGE);
			s.flags = flags[k];
			fail_nth_arm(nth, n);
			int r = submit(fd, &s), e = errno;
			bool fired = fail_nth_fired(nth);
			if (r == 0) {
				struct kdc_completion c = completion(fd);
				CHECK(c.status == 0 && memcmp(dst + 9, src + 5, s.length) == 0);
			} else {
				CHECK(fired && (e == ENOMEM || e == EFAULT));
			}
			baseline(fd);
			if (!fired)
				break; /* every allocation on the path has failed once */
			injected++;
		}
		CHECK(n < 1000);
	}
	close(fd);
	globals_idle();
	int submit_points = injected;

	/* probe: every failure must unwind cleanly, then a clean bind works */
	write_file(unbind, BDF1);
	fail_reject(true);
	struct expect e = expect_begin("probe-failures", "probe with driver kdc failed|"
				       "really_probe: driver_sysfs_add failed");
	for (n = 1; n < 2000; n++) {
		int bfd = open(bind, O_WRONLY | O_CLOEXEC);
		CHECK(bfd >= 0);
		fail_nth_arm(nth, n);
		ssize_t r = write(bfd, BDF1, strlen(BDF1));
		bool fired = fail_nth_fired(nth);
		close(bfd);
		if (!fired) {
			CHECK(r == (ssize_t)strlen(BDF1));
			break;
		}
		injected++;
		if (r > 0) /* a tolerated failure (debugfs, say): unbind and go on */
			write_file(unbind, BDF1);
		CHECK(kdc_index(BDF1) < 0);
	}
	CHECK(n < 2000);
	expect_seen(&e, "probe with driver kdc failed");
	expect_end(&e);
	fail_reject(false);
	close(nth);
	int dev = wait_kdc_index(BDF1, 2000);
	CHECK(dev >= 0);
	check_works(dev);
	globals_idle();
	say("  fault_injection: %d submit and %d probe allocation failures, all unwound",
	    submit_points, injected - submit_points);
}

/* ---- removal under load ---- */

struct remover {
	int fd, index;
	uint64_t epoch;
	unsigned submitted, ok, nodev;
	uint8_t seen[4096];
	atomic_bool ready;
};

/*
 * Submits until the device is gone, then reads until every submitted command
 * has completed exactly once; afterwards the file reports ENODEV.
 */
static void *remover_main(void *arg)
{
	struct remover *r = arg;
	uint8_t *src = map_pages(32), *dst = map_pages(32);
	unsigned completed = 0;
	bool gone = false;

	while (!gone || completed < r->submitted) {
		if (!gone && r->submitted < 4000) {
			unsigned slot = r->submitted % 16;
			struct kdc_submit s = req_epoch(r->epoch, r->submitted + 1, src + slot * PAGE,
							dst + slot * 2 * PAGE, 1 + r->submitted % 5000);
			s.flags = r->submitted % 3 == 0 ? KDC_SUBMIT_ZEROCOPY : 0;
			s.timeout_ms = HOLD_MS;
			if (submit(r->fd, &s) == 0)
				r->submitted++;
			else if (errno == ENODEV)
				gone = true;
			else
				CHECK(errno == ENOSPC || errno == EADDRINUSE);
		}
		struct kdc_completion c;
		ssize_t n = read(r->fd, &c, sizeof(c));
		if (n == sizeof(c)) {
			CHECK(c.id >= 1 && c.id <= r->submitted && !r->seen[c.id]);
			r->seen[c.id] = 1;
			completed++;
			if (c.status == 0) {
				r->ok++;
			} else {
				CHECK(c.status == -ENODEV && (c.flags & KDC_COMPL_RESET));
				r->nodev++;
			}
			if (r->ok >= 8)
				atomic_store(&r->ready, true);
			continue;
		}
		CHECK(n == -1 && (errno == EAGAIN || errno == ENODEV));
		if (errno == ENODEV) {
			CHECK(gone || completed == r->submitted);
			gone = true;
		}
		sleep_ms(1);
	}
	struct kdc_completion c;
	ERR(read(r->fd, &c, sizeof(c)), ENODEV);
	struct pollfd p = { .fd = r->fd, .events = POLLIN };
	CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLHUP));
	munmap(src, 32 * PAGE);
	munmap(dst, 32 * PAGE);
	return NULL;
}

static void removal_under_load(const char *qom_id, bool surprise, unsigned *nodev_out)
{
	enum { THREADS = 3 };
	int dev = kdc_index(BDF1);
	struct remover *r = calloc(THREADS, sizeof(*r));
	pthread_t t[THREADS];
	char request[96];

	CHECK(dev >= 0 && r);
	int idle = open_dev(dev), staged = open_dev(dev);
	uint8_t *buf = map_pages(4);
	struct kdc_submit s = req(staged, 1, buf, buf + 2 * PAGE, 100);
	s.flags = KDC_SUBMIT_BATCH;
	CHECK(submit(staged, &s) == 0);
	knob(dev, "service_ns", 2000000);
	for (int i = 0; i < THREADS; i++) {
		r[i].fd = open_dev(dev);
		r[i].index = i;
		r[i].epoch = stats(r[i].fd).epoch;
		CHECK(pthread_create(&t[i], NULL, remover_main, &r[i]) == 0);
	}
	for (int i = 0; i < THREADS; i++)
		while (!atomic_load(&r[i].ready))
			sleep_ms(1);
	struct expect e = { 0 };
	if (surprise) {
		e = expect_begin("surprise", "device not responding");
		snprintf(request, sizeof(request), "qom-set %s fault-gone true", qom_id);
		host(request);
		long end = now_ms() + 10000;
		while (bar0_id(BDF1) != 0xffffffff && now_ms() < end)
			sleep_ms(5);
		CHECK(bar0_id(BDF1) == 0xffffffff); /* MMIO now reads all-ones */
		CHECK(wait_state(idle, KDC_STATE_GONE, 5000).dev_state == KDC_STATE_GONE);
		expect_seen(&e, "device not responding");
	} else {
		/* keep every command device-owned until the removal */
		knob(dev, "fault", FAULT_STALL);
		sleep_ms(20);
	}
	snprintf(request, sizeof(request), "device_del %s", qom_id);
	host(request);
	CHECK(wait_absent(BDF1, 20000));
	if (surprise)
		expect_end(&e);
	unsigned nodev = 0, ok = 0, submitted = 0;
	for (int i = 0; i < THREADS; i++) {
		CHECK(pthread_join(t[i], NULL) == 0);
		submitted += r[i].submitted;
		ok += r[i].ok;
		nodev += r[i].nodev;
		ERR(ioctl(r[i].fd, KDC_IOC_FLUSH), ENODEV);
		CHECK(close(r[i].fd) == 0);
	}
	/* the staged command was ended by the removal too */
	struct kdc_completion c;
	CHECK(read(staged, &c, sizeof(c)) == sizeof(c) && c.status == -ENODEV);
	ERR(read(staged, &c, sizeof(c)), ENODEV);
	ERR(ioctl(idle, KDC_IOC_STATS, &(struct kdc_stats){ 0 }), ENODEV);
	CHECK(close(staged) == 0 && close(idle) == 0);
	CHECK(nodev >= 1 && ok + nodev == submitted);
	globals_idle();
	free(r);
	*nodev_out = nodev;
	say("  %s: %u commands from %d threads, %u completed, %u ended with ENODEV, each once",
	    surprise ? "removal_surprise" : "removal_orderly", submitted, THREADS, ok, nodev);
}

static void removal_orderly(void)
{
	unsigned nodev;
	removal_under_load("kdc1", false, &nodev);
	/* hot-add a new function in the same slot; the driver probes it */
	host("device_add {\"driver\": \"kdc-dma\", \"id\": \"kdc1b\", \"bus\": \"rp1\", \"queues\": 2}");
	int dev = wait_kdc_index(BDF1, 20000);
	CHECK(dev >= 0);
	check_works(dev);
}

static void removal_surprise(void)
{
	unsigned nodev;
	removal_under_load("kdc1b", true, &nodev);
}

int main(void)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} cases[] = {
		{ "device_info", device_info },
		{ "abi_validation", abi_validation },
		{ "data_integrity", data_integrity },
		{ "nop_and_batch_read", nop_and_batch_read },
		{ "queue_selection", queue_selection },
		{ "batch_flush", batch_flush },
		{ "coalescing", coalescing },
		{ "pinned_page_not_recycled", pinned_page_not_recycled },
		{ "cancel_driver_owned", cancel_driver_owned },
		{ "cancel_device_owned", cancel_device_owned },
		{ "cancel_race", cancel_race },
		{ "close_with_inflight", close_with_inflight },
		{ "reset_semantics", reset_semantics },
		{ "quotas", quotas },
		{ "file_isolation", file_isolation },
		{ "concurrency", concurrency },
		{ "shared_file", shared_file },
		{ "ring_overflow_all_queues", ring_overflow_all_queues },
		{ "second_device", second_device },
		{ "unbind_rebind", unbind_rebind },
		{ "timeout_abort", timeout_abort },
		{ "timeout_reset", timeout_reset },
		{ "timeout_escalation", timeout_escalation },
		{ "recovery_failure", recovery_failure },
		{ "sysfs_reset_under_load", sysfs_reset_under_load },
		{ "fatal_recovery", fatal_recovery },
		{ "lost_completions", lost_completions },
		{ "bounded_close_and_reset", bounded_close_and_reset },
		{ "dma_fault", dma_fault },
		{ "dst_overlap", dst_overlap },
		{ "adjacent_destinations", adjacent_destinations },
		{ "fault_injection", fault_injection },
		{ "removal_orderly", removal_orderly },
		{ "removal_surprise", removal_surprise },
	};
	unsigned n = sizeof(cases) / sizeof(cases[0]);

	setvbuf(stdout, NULL, _IOLBF, 0);
	crc_init();
	ncpus = (unsigned)sysconf(_SC_NPROCESSORS_ONLN);
	alarm(3000);
	kmsg_out = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	int fd0 = open_dev(0);
	bounce_max = stats(fd0).bounce_max;
	close(fd0);
	for (unsigned i = 0; i < n; i++) {
		long t0 = now_ms();
		cases[i].fn();
		say("PASS %s (%ld ms)", cases[i].name, now_ms() - t0);
	}
	say("KDC_TEST_PASS cases=%u cpus=%u", n, ncpus);
	return 0;
}
