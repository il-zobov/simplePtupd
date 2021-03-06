#include <stdbool.h>
/* Dump simple-pt buffers to files (ptout.cpu) */
/* sptdump [filenameprefix] */
/* Always adds .N for the different cpus */

/*
 * Copyright (c) 2015, Intel Corporation
 * Author: Andi Kleen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define _GNU_SOURCE 1
#include "simple-pt.h"
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <sys/wait.h>

#define err(x) perror(x), exit(1)
const char *filename =  "testTrace";
const char *filename2 =  "./testTrace.0";

#include "map.h"

#define BIT(x) (1U << (x))

typedef unsigned long long u64;


static char psb[16] = {
	0x02, 0x82, 0x02, 0x82, 0x02, 0x82, 0x02, 0x82,
	0x02, 0x82, 0x02, 0x82, 0x02, 0x82, 0x02, 0x82
};

#define LEFT(x) ((end - p) >= (x))

/* Caller must have checked length */
static u64 get_ip_val(unsigned char **pp, unsigned char *end, int len, uint64_t *last_ip)
{
	unsigned char *p = *pp;
	u64 v = *last_ip;
	int i;
	unsigned shift = 0;

	if (len == 0) {
		*last_ip = 0;
		return 0; /* out of context */
	}
	if (len < 4) {
		if (!LEFT(len)) {
			*last_ip = 0;
			return 0; /* XXX error */
		}
		for (i = 0; i < len; i++, shift += 16, p += 2) {
			uint64_t b = *(uint16_t *)p;
			v = (v & ~(0xffffULL << shift)) | (b << shift);
		}
		v = ((int64_t)(v << (64 - 48))) >> (64 - 48); /* sign extension */
	} else {
		return 0; /* XXX error */
	}
	*pp = p;
	*last_ip = v;
	return v;
}

/* Caller must have checked length */
static u64 get_val(unsigned char **pp, int len)
{
	unsigned char *p = *pp;
	u64 v = 0;
	int i;
	unsigned shift = 0;

	for (i = 0; i < len; i++, shift += 8)
		v |= ((uint64_t)(*p++)) << shift;
	*pp = p;
	return v;
}

static void print_unknown(unsigned char *p, unsigned char *end, unsigned char *map)
{
	printf("unknown packet: ");
	unsigned len = end - p;
	int i;
	if (len > 16)
		len = 16;
	for (i = 0; i < len; i++)
		printf("%02x ", p[i]);
	printf("\n");
}

static void print_tnt_byte(unsigned char v, int max)
{
	int i;
	for (i = max - 1; i >= 0; i--)
		if (v & BIT(i))
			putchar('T');
		else
			putchar('N');
}

static void print_tnt_stop(unsigned char v)
{
	int j;
	for (j = 7; j >= 0; j--) {
		if (v & BIT(j))
			break;
	}
	print_tnt_byte(v, j);
}

static void print_multi_tnt(unsigned char *p, int len)
{
	int i;

	for (i = len - 1; i >= 0 && p[i] == 0; i--)
		;
	if (i >= 0) {
		print_tnt_stop(p[i]);
		i--;
	} else {
		printf("??? no stop bit");
		return;
	}
	for (; i >= 0; i--)
		print_tnt_byte(p[i], 8);
}

void  decode_buffer(unsigned char *map, size_t len)
{
    printf("Start decode ");
	unsigned char *end = map + len;
	unsigned char *p;
	size_t skipped = 0;
	size_t overflow = 0;
	uint64_t last_ip = 0;

	for (p = map; p < end; ) {
		unsigned char *prev = p;
		/* look for PSB */
		p = memmem(p, end - p, psb, 16);
		if (!p) {
			p = end;
			break;
		}
		skipped += p - prev;
		while (p < end) {
			printf("%lx\t", p - map);

			if (*p == 2 && LEFT(2)) {
				if (p[1] == 0xa3 && LEFT(8)) { /* long TNT */
					printf("tnt64 ");
					print_multi_tnt(p + 2, 6);
					printf("\n");
					p += 8;
					continue;
				}
				if (p[1] == 0x43 && LEFT(8)) { /* PIP */
					p += 2;
					printf("pip\t%llx\n", (get_val(&p, 6) >> 1) << 5);
					continue;
				}
				if (p[1] == 3 && LEFT(4) && p[3] == 0) { /* CBR */
					printf("cbr\t%u\n", p[2]);
					p += 4;
					continue;
				}
				if (p[1] == 0b10000011) {
					printf("tracestop\n");
					p += 2;
					continue;
				}
				if (p[1] == 0b11110011 && LEFT(8)) { /* OVF */
					printf("ovf\n");
					p += 8;
					overflow++;
					continue;
				}
				if (p[1] == 0x82 && LEFT(16) && !memcmp(p, psb, 16)) { /* PSB */
					printf("psb\n");
					p += 16;
					continue;
				}
				if (p[1] == 0b100011) { /* PSBEND */
					printf("psbend\n");
					p += 2;
					continue;
				}
				/* MNT */
				if (p[1] == 0b11000011 && LEFT(11) && p[2] == 0b10001000) {
					printf("mnt\t%llx\n",
						p[3] |
						((u64)p[4] << 8) |
						((u64)p[5] << 16) |
						((u64)p[6] << 24) |
						((u64)p[7] << 32) |
						((u64)p[8] << 40) |
						((u64)p[9] << 48) |
						((u64)p[10] << 56));
					p += 11;
					continue;
				}
				/* TMA */
				if (p[1] == 0b01110011 && LEFT(7)) {
					printf("tma\tctc=%u fc=%u\n",
							p[2] | (p[3] << 8),
							p[5] | ((p[6] & 1) << 8));
					p += 7;
					continue;
				}
				/* VMCS */
				if (p[1] == 0b11001000 && LEFT(7)) {
					printf("vmcs\t%llx\n",
						((u64)p[2] << 12) |
						((u64)p[3] << 20) |
						((u64)p[4] << 28) |
						((u64)p[5] << 36) |
						((u64)p[6] << 44));
					p += 7;
					continue;
				}
			}

			if ((*p & BIT(0)) == 0) {
				if (*p == 0) { /* PAD */
					printf("pad\n");
					p++;
					continue;
				}
				printf("tnt8 ");
				print_tnt_stop(*p >> 1);
				printf("\n");
				p++;
				continue;
			}

			char *name = NULL;
			switch (*p & 0x1f) {
			case 0xd:
				name = "tip";
				break;
			case 0x11:
				name = "tip.pge";
				break;
			case 0x1:
				name = "tip.pgd";
				break;
			case 0x1d:
				name = "fup";
				break;
			}
			if (name) {
				int ipl = *p >> 5;
				p++;
				printf("%s\t%d: %llx\n", name, ipl, get_ip_val(&p, end, ipl, &last_ip));
				continue;
			}
			if (*p == 0x99 && LEFT(2)) { /* MODE */
				if ((p[1] >> 5) == 1) {
					printf("mode.tsx");
					if (p[1] & BIT(0))
						printf(" intx");
					if (p[1] & BIT(1))
						printf(" txabort");
					printf("\n");
					p += 2;
					continue;
				} else if ((p[1] >> 5) == 0) {
					printf("mode.exec");
					printf(" lma=%d", (p[1] & BIT(0)));
					printf(" cs.d=%d", !!(p[1] & BIT(1)));
					printf("\n");
					p += 2;
					continue;
				}
			}

			if (*p == 0x19 && LEFT(8)) {  /* TSC */
				p++;
				printf("tsc\t%llu\n", get_val(&p, 7));
				continue;
			}
			if (*p == 0b01011001 && LEFT(2)) { /* MTC */
				printf("mtc\t%u\n", p[1]);
				p += 2;
				continue;
			}
			if ((*p & 3) == 3) { /* CYC */
				u64 cyc = *p >> 2;
				unsigned shift = 4;
				if ((*p & 4) && LEFT(1)) {
					do {
						p++;
						cyc |= (*p >> 1) << shift;
						shift += 7;
					} while ((*p & 1) && LEFT(1));
				}
				printf("cyc\t%llu\n", cyc);
				p++;
				continue;
			}

			print_unknown(p, end, map);
			break;
		}
	}
	if (p < end || skipped)
		printf("%lu bytes undecoded\n", (end - p) + skipped);
	if (overflow)
		printf("%lu overflows\n", overflow);
}

void  saveDump()
{
    int ncpus = sysconf(_SC_NPROCESSORS_CONF);
	int pfds[ncpus];
	int bufsize;
	char *pbuf[ncpus];

	int i;
	for (i = 0; i < ncpus; i++) {
		pfds[i] = open("/dev/simple-pt", O_RDONLY | O_CLOEXEC);
		if (pfds[i] < 0)
			err("open /dev/simple-pt");

		if (ioctl(pfds[i], SIMPLE_PT_SET_CPU, i) < 0) {
			close(pfds[i]);
			pfds[i] = -1;
			perror("SIMPLE_PT_SET_CPU");
			/* CPU likely off line */
			continue;
		}

		if (ioctl(pfds[i], SIMPLE_PT_GET_SIZE, &bufsize) < 0)
			err("SIMPLE_PT_GET_SIZE");

		pbuf[i] = mmap(NULL, bufsize, PROT_READ, MAP_PRIVATE, pfds[i], 0);
		if (pbuf[i] == (void*)-1)
			err("mmap on simplept");

		char fn[1024];
		snprintf(fn, sizeof fn, "%s.%d",filename, i);
		int fd = open(fn, O_WRONLY|O_CREAT|O_TRUNC, 0644);
		if (fd < 0)
			err("Opening output file");

		unsigned offset;
		if (ioctl(pfds[i], SIMPLE_PT_GET_OFFSET, &offset) < 0) {
			perror("SIMPLE_PT_GET_OFFSET");
			continue;
		}

		unsigned len = 0;
		if (*(uint64_t *)(pbuf[i] + offset))
			len += write(fd, pbuf[i] + offset, bufsize - offset);
		len += write(fd, pbuf[i], offset);
		printf("cpu %3d offset %6u, %5u KB, writing to %s\n", i, offset, len >> 10, fn);
		close(fd);
		if (len == 0)
			unlink(fn);
	}
}

void do_file(char *fn)
{
    printf(" Do file: ");
    printf(fn);
	size_t len;
	unsigned char *map = mapfile(fn, &len);
	if (!map) {
		perror(fn);
		return;
	}
	decode_buffer(map, len);
	unmapfile(map, len);
}

int main(int ac, char **av)
{
while(true)
{
  //  system("./sptcmd --enable");
   // system("./a.out");
   // sleep(3);
    do_file("./testTrace.0");
    
   //system("./fastdecode ./testTrace.0 | grep pip " );
  //  system("./sptcmd --disable");

	//saveDump();
}
	return 0;
}
