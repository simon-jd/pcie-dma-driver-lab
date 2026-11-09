// SPDX-License-Identifier: GPL-2.0
/*
 * In-guest benchmark client for /dev/kdc0 (static binary; scripts/run_bench.py
 * puts it into the initramfs together with a plan file).
 *
 * The plan has one "reps N" line, then one line per arm:
 *   <name> threads=T qd=Q size=S mode=nop|zc|bounce|fixed batch=0|1 coalesce=C
 *          service_ns=N queues=own|one files=own|shared ms=M
 * qd is per thread; qd=0 is an idle window (no commands). coalesce is the
 * driver's debugfs value, usecs << 16 | count.
 * mode=fixed registers each thread's buffers once (KDC_IOC_REGISTER) and submits
 * KDC_SUBMIT_FIXED commands into them. Every arm runs N times; repetition r starts at arm r mod (number of arms),
 * so the arms are interleaved in time rather than run back to back.
 *
 * One window: open the files, map and fill the buffers, set the device knobs,
 * run a short warm-up and drain it, then ask the host for a counter snapshot
 * ("@M <token>" on the console), wait until the host has written the token into
 * the device's scratch register, run for M ms with Q commands in flight per
 * thread, drain, and ask for the closing snapshot. The host answers that one
 * with the token plus REJECT_BIT when the window had more foreign load on the
 * VM's host CPUs than it accepts; the window is then run again (at most
 * MAX_TRIES times). Nothing is printed inside a window. Results go to the
 * kernel log as "kdc_bench: RESULT ..." lines.
 *
 * Every completion is checked: status 0 and the CRC32C of its source, which
 * is computed once per arm before the window.
 */
#define _GNU_SOURCE
#include "kdc_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096UL
#define MAX_ARMS 128
#define MAX_THREADS 8
#define MAX_TRIES 4
#define WARMUP_MS 150
#define REJECT_BIT 0x80000000u
#define SLOT_BITS 16

static int kmsg = -1;
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	char buf[1000];
	va_list ap;
	int n = snprintf(buf, sizeof(buf), "kdc_bench: ");

	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	if (kmsg < 0 || write(kmsg, buf, n) < 0)
		fwrite(buf, 1, n, stdout);
}

static void die(const char *what) __attribute__((noreturn));
static void die(const char *what)
{
	say("FAIL %s (errno=%d %s)", what, errno, strerror(errno));
	exit(1);
}

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}

static void sleep_us(long us)
{
	struct timespec t = { .tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000 };
	while (nanosleep(&t, &t) && errno == EINTR)
		;
}

static uint32_t crc_table[256];
static uint32_t crc32c(const uint8_t *p, size_t len)
{
	uint32_t crc = ~0u;
	while (len--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

/* ---- device knobs (debugfs) ---- */

static void knob_path(char *path, size_t n, const char *name)
{
	snprintf(path, n, "/sys/kernel/debug/kdc/kdc0/%s", name);
}

static void set_knob(const char *name, unsigned long value)
{
	char path[96], text[32];
	knob_path(path, sizeof(path), name);
	int fd = open(path, O_WRONLY);
	int n = snprintf(text, sizeof(text), "%lu\n", value);
	if (fd < 0 || write(fd, text, n) != n)
		die(path);
	close(fd);
}

static unsigned long get_knob(const char *name)
{
	char path[96], text[32] = { 0 };
	knob_path(path, sizeof(path), name);
	int fd = open(path, O_RDONLY); /* debugfs attributes cannot seek: reopen */
	if (fd < 0 || read(fd, text, sizeof(text) - 1) <= 0)
		die(path);
	close(fd);
	return strtoul(text, NULL, 10);
}

/*
 * Ask the host for a snapshot and wait for its answer in the scratch register.
 * The request is a short console line, because every character the guest
 * writes to the serial port is a VM exit inside the counters being taken.
 */
static unsigned token_seq;
static bool host_mark(void)
{
	unsigned token = ++token_seq;

	say("@M %u", token);
	for (uint64_t end = now_ns() + 30000000000ull;;) {
		unsigned long v = get_knob("scratch");
		if ((v & ~REJECT_BIT) == token)
			return !(v & REJECT_BIT);
		if (now_ns() > end) {
			errno = ETIMEDOUT;
			die("host never answered the mark");
		}
		sleep_us(200);
	}
}

/* ---- arms ---- */

enum mode { MODE_NOP, MODE_ZC, MODE_BOUNCE, MODE_FIXED };

struct arm {
	char name[48];
	unsigned threads, qd, size, ms, coalesce, service_ns;
	enum mode mode;
	bool batch, one_queue, shared_file;
};

static int parse_arm(char *line, struct arm *a)
{
	char *tok = strtok(line, " \t\n");
	if (!tok || tok[0] == '#')
		return 0;
	memset(a, 0, sizeof(*a));
	snprintf(a->name, sizeof(a->name), "%s", tok);
	a->threads = 1;
	a->qd = 1;
	a->ms = 1000;
	a->mode = MODE_ZC;
	while ((tok = strtok(NULL, " \t\n"))) {
		char *eq = strchr(tok, '=');
		if (!eq)
			return -1;
		*eq++ = 0;
		if (!strcmp(tok, "threads"))
			a->threads = strtoul(eq, NULL, 0);
		else if (!strcmp(tok, "qd"))
			a->qd = strtoul(eq, NULL, 0);
		else if (!strcmp(tok, "size"))
			a->size = strtoul(eq, NULL, 0);
		else if (!strcmp(tok, "ms"))
			a->ms = strtoul(eq, NULL, 0);
		else if (!strcmp(tok, "coalesce"))
			a->coalesce = strtoul(eq, NULL, 0);
		else if (!strcmp(tok, "service_ns"))
			a->service_ns = strtoul(eq, NULL, 0);
		else if (!strcmp(tok, "batch"))
			a->batch = atoi(eq);
		else if (!strcmp(tok, "mode"))
			a->mode = !strcmp(eq, "nop") ? MODE_NOP : !strcmp(eq, "bounce") ? MODE_BOUNCE :
				  !strcmp(eq, "fixed") ? MODE_FIXED : MODE_ZC;
		else if (!strcmp(tok, "queues"))
			a->one_queue = !strcmp(eq, "one");
		else if (!strcmp(tok, "files"))
			a->shared_file = !strcmp(eq, "shared");
		else
			return -1;
	}
	if (a->mode == MODE_NOP)
		a->size = 0;
	if (!a->threads || a->threads > MAX_THREADS || a->qd > KDC_MAX_CMDS_PER_FILE ||
	    (a->mode != MODE_NOP && (!a->size || a->size > KDC_MAX_LENGTH)) ||
	    (a->shared_file && a->batch) ||
	    2ull * a->size * a->qd * (a->shared_file ? a->threads : 1) > KDC_FILE_BYTE_QUOTA ||
	    a->qd * (a->shared_file ? a->threads : 1) > KDC_MAX_CMDS_PER_FILE)
		return -1;
	return 1;
}

/* ---- one window ---- */

struct slot {
	uint8_t *src, *dst;
	uint32_t crc;
	_Atomic uint64_t t_submit; /* 0 while free */
};

struct worker {
	unsigned idx;
	const struct arm *arm;
	int fd;
	struct slot *slots;
	uint8_t *region; /* mode=fixed: every slot's src and dst, registered */
	unsigned buf_index;
	pthread_t thread;
	uint64_t deadline, t_end;
	uint64_t done, bad;
	unsigned queues_seen;
	uint32_t *lat;
	size_t nlat, cap;
	bool record;
	atomic_bool arrived; /* left run_loop() of the current phase */
};

static struct worker workers[MAX_THREADS];
static unsigned nworkers;
static pthread_barrier_t start_barrier;
static uint64_t warmup_end, window_start;
static _Atomic long outstanding; /* shared-file mode: commands of all threads in flight */
static unsigned ncpus;

static void submit_slot(struct worker *w, unsigned i, bool stage)
{
	const struct arm *a = w->arm;
	struct slot *s = &w->slots[i];
	struct kdc_submit req = {
		.version = KDC_ABI_VERSION, .size = sizeof(req), .epoch = 1,
		.id = (uint64_t)w->idx << SLOT_BITS | i,
		.queue = a->one_queue ? 0 : KDC_QUEUE_AUTO,
	};

	if (a->mode == MODE_NOP) {
		req.flags = KDC_SUBMIT_NOP;
	} else {
		req.src = (uintptr_t)s->src;
		req.dst = (uintptr_t)s->dst;
		req.length = a->size;
		req.flags = a->mode == MODE_BOUNCE ? KDC_SUBMIT_BOUNCE :
			    a->mode == MODE_FIXED ? KDC_SUBMIT_FIXED : KDC_SUBMIT_ZEROCOPY;
	}
	if (stage)
		req.flags |= KDC_SUBMIT_BATCH;
	atomic_store_explicit(&s->t_submit, now_ns(), memory_order_relaxed);
	if (a->shared_file)
		atomic_fetch_add(&outstanding, 1);
	if (ioctl(w->fd, KDC_IOC_SUBMIT, &req))
		die("submit");
}

/* Account one completion read by @w (in shared-file mode it may be another thread's). */
static unsigned complete_one(struct worker *w, const struct kdc_completion *c, uint64_t now)
{
	unsigned owner = c->id >> SLOT_BITS, i = c->id & ((1u << SLOT_BITS) - 1);

	if (owner >= nworkers || i >= w->arm->qd) {
		errno = EPROTO;
		die("completion with an unknown id");
	}
	struct slot *s = &workers[owner].slots[i];
	uint64_t t = atomic_exchange_explicit(&s->t_submit, 0, memory_order_relaxed);
	if (c->status || c->crc != s->crc || c->length != w->arm->size)
		w->bad++;
	w->queues_seen |= 1u << (c->queue & 31);
	w->done++;
	if (w->record && w->nlat < w->cap) {
		uint64_t d = now - t;
		w->lat[w->nlat++] = d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
	}
	if (w->arm->shared_file)
		atomic_fetch_sub(&outstanding, 1);
	return i;
}

static void run_loop(struct worker *w)
{
	const struct arm *a = w->arm;
	struct kdc_completion comp[KDC_MAX_CMDS_PER_FILE];
	unsigned freed[KDC_MAX_CMDS_PER_FILE];
	long inflight = 0;

	if (!a->qd) { /* idle arm: what the window costs with no commands */
		uint64_t now = now_ns();
		if (w->deadline > now)
			sleep_us((w->deadline - now) / 1000);
		w->t_end = now_ns();
		return;
	}
	for (unsigned i = 0; i < a->qd; i++)
		submit_slot(w, i, a->batch);
	if (a->batch && ioctl(w->fd, KDC_IOC_FLUSH))
		die("flush");
	inflight = a->qd;
	for (;;) {
		ssize_t n = read(w->fd, comp, sizeof(comp));
		if (n < 0) {
			/* shared file: the main thread interrupts readers after the deadline */
			if (errno == EINTR && now_ns() >= w->deadline)
				break;
			if (errno == EINTR)
				continue;
			die("read");
		}
		uint64_t now = now_ns();
		unsigned nfreed = 0;
		for (ssize_t k = 0; k < n / (ssize_t)sizeof(comp[0]); k++) {
			unsigned owner = comp[k].id >> SLOT_BITS;
			unsigned i = complete_one(w, &comp[k], now);
			if (owner == w->idx)
				freed[nfreed++] = i;
		}
		if (!a->shared_file)
			inflight -= nfreed;
		if (now >= w->deadline)
			break;
		if (a->shared_file) {
			/* our own slots freed by any thread */
			for (unsigned i = 0; i < a->qd; i++)
				if (!atomic_load_explicit(&w->slots[i].t_submit, memory_order_relaxed))
					submit_slot(w, i, false);
			continue;
		}
		for (unsigned k = 0; k < nfreed; k++)
			submit_slot(w, freed[k], a->batch);
		if (a->batch && nfreed && ioctl(w->fd, KDC_IOC_FLUSH))
			die("flush");
		inflight += nfreed;
	}
	/* drain: own file only; the main thread drains a shared one */
	while (!a->shared_file && inflight > 0) {
		ssize_t n = read(w->fd, comp, sizeof(comp));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("drain");
		}
		uint64_t now = now_ns();
		for (ssize_t k = 0; k < n / (ssize_t)sizeof(comp[0]); k++, inflight--)
			complete_one(w, &comp[k], now);
	}
	w->t_end = now_ns();
}

static void *worker_main(void *arg)
{
	struct worker *w = arg;
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(w->idx % ncpus, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		die("affinity");
	w->deadline = warmup_end; /* warm-up, not recorded */
	run_loop(w);
	atomic_store(&w->arrived, true);
	pthread_barrier_wait(&start_barrier); /* the opening snapshot has been taken */
	w->done = w->bad = 0;
	w->queues_seen = 0;
	w->record = true;
	w->deadline = window_start + w->arm->ms * 1000000ull;
	run_loop(w);
	atomic_store(&w->arrived, true);
	return NULL;
}

static void on_signal(int sig)
{
	(void)sig;
}

/*
 * Wait until every worker has left run_loop(). A reader blocked on a shared
 * file may wait for completions that other threads took, so after @deadline
 * the stragglers get a signal that makes read() return EINTR.
 */
static void wait_arrival(bool shared, uint64_t deadline)
{
	for (uint64_t end = deadline + 30000000000ull;;) {
		unsigned left = 0;
		uint64_t now = now_ns();
		for (unsigned t = 0; t < nworkers; t++) {
			if (atomic_load(&workers[t].arrived))
				continue;
			left++;
			if (shared && now >= deadline)
				pthread_kill(workers[t].thread, SIGUSR1);
		}
		if (!left)
			return;
		if (now > end) {
			errno = ETIMEDOUT;
			die("workers did not finish");
		}
		sleep_us(200);
	}
}

/* Main thread: read a shared file empty after the workers have stopped. */
static void drain_shared(int fd, struct worker *w)
{
	struct kdc_completion comp[KDC_MAX_CMDS_PER_FILE];

	while (atomic_load(&outstanding) > 0) {
		ssize_t n = read(fd, comp, sizeof(comp));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("drain shared");
		}
		uint64_t now = now_ns();
		for (ssize_t k = 0; k < n / (ssize_t)sizeof(comp[0]); k++)
			complete_one(w, &comp[k], now);
	}
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static uint8_t *map_buf(size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
		       -1, 0);
	if (p == MAP_FAILED)
		die("mmap");
	return p;
}

static void run_arm(const struct arm *a, unsigned rep)
{
	size_t span = a->size ? (a->size + PAGE - 1) / PAGE * PAGE : 0;
	struct worker drainer = { .arm = a };
	int shared_fd = -1;

	set_knob("service_ns", a->service_ns);
	set_knob("coalesce", a->coalesce);
	nworkers = a->threads;
	for (unsigned t = 0; t < nworkers; t++) {
		struct worker *w = &workers[t];
		memset(w, 0, sizeof(*w));
		w->idx = t;
		w->arm = a;
		if (!a->shared_file || shared_fd < 0) {
			w->fd = open("/dev/kdc0", O_RDWR | O_CLOEXEC);
			if (w->fd < 0)
				die("open");
			if (a->shared_file)
				shared_fd = w->fd;
		} else {
			w->fd = shared_fd;
		}
		w->slots = calloc(a->qd, sizeof(*w->slots));
		w->cap = (size_t)a->ms * 2000 + 1000; /* up to 2 M completions per second per thread */
		w->lat = malloc(w->cap * sizeof(*w->lat));
		if (!w->slots || !w->lat)
			die("malloc");
		if (a->mode == MODE_FIXED && a->qd) {
			struct kdc_buffer b = { .version = KDC_ABI_VERSION, .size = sizeof(b),
						.length = 2 * span * a->qd };
			w->region = map_buf(b.length);
			b.addr = (uintptr_t)w->region;
			if (ioctl(w->fd, KDC_IOC_REGISTER, &b))
				die("register");
			w->buf_index = b.index;
		}
		for (unsigned i = 0; i < a->qd && span; i++) {
			struct slot *s = &w->slots[i];
			s->src = w->region ? w->region + 2 * i * span : map_buf(span);
			s->dst = w->region ? w->region + (2 * i + 1) * span : map_buf(span);
			for (size_t b = 0; b < span; b++)
				s->src[b] = (uint8_t)(b * 31 + i * 7 + t * 13 + rep);
			s->crc = crc32c(s->src, a->size);
		}
	}
	drainer.fd = shared_fd;

	for (unsigned attempt = 1;; attempt++) {
		atomic_store(&outstanding, 0);
		pthread_barrier_init(&start_barrier, NULL, nworkers + 1);
		warmup_end = now_ns() + WARMUP_MS * 1000000ull;
		for (unsigned t = 0; t < nworkers; t++) {
			workers[t].done = workers[t].bad = workers[t].nlat = 0;
			workers[t].record = false;
			atomic_store(&workers[t].arrived, false);
			if (pthread_create(&workers[t].thread, NULL, worker_main, &workers[t]))
				die("pthread_create");
		}
		wait_arrival(a->shared_file, warmup_end);
		if (a->shared_file)
			drain_shared(shared_fd, &drainer);
		drainer.done = drainer.bad = 0;
		for (unsigned t = 0; t < nworkers; t++)
			atomic_store(&workers[t].arrived, false);
		host_mark();
		window_start = now_ns();
		uint64_t t0 = window_start;
		pthread_barrier_wait(&start_barrier);
		wait_arrival(a->shared_file, t0 + a->ms * 1000000ull);
		if (a->shared_file)
			drain_shared(shared_fd, &drainer);
		uint64_t t1 = now_ns();
		for (unsigned t = 0; t < nworkers; t++)
			pthread_join(workers[t].thread, NULL);
		if (!a->shared_file) {
			t1 = 0;
			for (unsigned t = 0; t < nworkers; t++)
				if (workers[t].t_end > t1)
					t1 = workers[t].t_end;
		}
		bool accepted = host_mark();
		pthread_barrier_destroy(&start_barrier);

		uint64_t done = drainer.done, bad = drainer.bad;
		unsigned queues = 0;
		size_t nlat = 0;
		for (unsigned t = 0; t < nworkers; t++) {
			done += workers[t].done;
			bad += workers[t].bad;
			queues |= workers[t].queues_seen;
			nlat += workers[t].nlat;
		}
		uint32_t *all = malloc((nlat + 1) * sizeof(*all));
		if (!all)
			die("malloc");
		size_t k = 0;
		for (unsigned t = 0; t < nworkers; t++) {
			memcpy(all + k, workers[t].lat, workers[t].nlat * sizeof(*all));
			k += workers[t].nlat;
		}
		qsort(all, nlat, sizeof(*all), cmp_u32);
		uint32_t p50 = nlat ? all[nlat / 2] : 0, p99 = nlat ? all[nlat * 99 / 100] : 0;
		uint32_t p10 = nlat ? all[nlat / 10] : 0, p90 = nlat ? all[nlat * 9 / 10] : 0;
		free(all);
		say("RESULT arm=%s rep=%u try=%u token=%u accepted=%d cmds=%llu bad=%llu ns=%llu "
		    "lat_p10=%u lat_p50=%u lat_p90=%u lat_p99=%u queues=0x%x",
		    a->name, rep, attempt, token_seq, accepted, (unsigned long long)done,
		    (unsigned long long)bad, (unsigned long long)(t1 - t0), p10, p50, p90, p99, queues);
		if (bad) {
			errno = EIO;
			die("completions with a wrong status or CRC");
		}
		if (accepted || attempt == MAX_TRIES)
			break;
	}
	for (unsigned t = 0; t < nworkers; t++) { /* before any close: a shared file is closed once */
		struct worker *w = &workers[t];
		struct kdc_buffer b = { .version = KDC_ABI_VERSION, .size = sizeof(b),
					.index = w->buf_index };
		if (w->region && ioctl(w->fd, KDC_IOC_UNREGISTER, &b))
			die("unregister");
	}
	for (unsigned t = 0; t < nworkers; t++) {
		struct worker *w = &workers[t];
		if (w->region)
			munmap(w->region, 2 * span * a->qd);
		for (unsigned i = 0; i < a->qd && span && !w->region; i++) {
			munmap(w->slots[i].src, span);
			munmap(w->slots[i].dst, span);
		}
		free(w->slots);
		free(w->lat);
		if (!a->shared_file || t == 0)
			close(w->fd);
	}
}

int main(int argc, char **argv)
{
	static struct arm arms[MAX_ARMS];
	unsigned narms = 0, reps = 1;
	char line[512];
	struct sigaction sa = { .sa_handler = on_signal }; /* no SA_RESTART: read() returns EINTR */

	setvbuf(stdout, NULL, _IOLBF, 0);
	kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	sigaction(SIGUSR1, &sa, NULL);
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (c & 1 ? 0x82F63B78u : 0);
		crc_table[i] = c;
	}
	ncpus = (unsigned)sysconf(_SC_NPROCESSORS_ONLN);
	FILE *plan = fopen(argc > 1 ? argv[1] : "/plan.txt", "r");
	if (!plan)
		die("plan");
	while (fgets(line, sizeof(line), plan)) {
		if (!strncmp(line, "reps ", 5)) {
			reps = strtoul(line + 5, NULL, 0);
			continue;
		}
		if (narms == MAX_ARMS) {
			errno = E2BIG;
			die("too many arms");
		}
		int r = parse_arm(line, &arms[narms]);
		if (r < 0) {
			errno = EINVAL;
			die("bad plan line");
		}
		narms += r;
	}
	fclose(plan);
	say("START arms=%u reps=%u cpus=%u", narms, reps, ncpus);
	for (unsigned r = 0; r < reps; r++)
		for (unsigned k = 0; k < narms; k++)
			run_arm(&arms[(r + k) % narms], r);
	set_knob("coalesce", 0);
	set_knob("service_ns", 0);
	say("DONE");
	return 0;
}
