/* SPDX-License-Identifier: MIT */
/*
 * sweetaudio_tone [SECONDS] [HZ] [BEEPS]: writes a sine (-3 dBFS PCM; the kernel gain, default -15 dB since xnu patch 0181, makes it -18 dBFS), both channels, to /dev/sweetaudio, paced by the kernel FIFO.
 * BEEPS > 0: BEEPS x (0.5 s on, 0.5 s off) instead of a steady tone (SECONDS = BEEPS). Tests the kernel path without CoreAudio. Bounded: at most 10 s of audio, at most SECONDS + 2 s wall time, 1 ms sleeps.
 * Prints one result line: "sweetaudio_tone: done: N frames written, U underruns, O overruns" or "sweetaudio_tone: FAIL ...".
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "sweetaudio_dev.h"

int
main(int argc, char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 2.0, hz = argc > 2 ? atof(argv[2]) : 440.0;
	int beeps = argc > 3 ? atoi(argv[3]) : 0;
	if (beeps > 0 && beeps <= 10)
		secs = beeps;
	if (!(secs > 0.0) || secs > 10.0 || !(hz >= 20.0) || hz > 20000.0) {
		fprintf(stderr, "usage: sweetaudio_tone [SECONDS 0-10] [HZ 20-20000]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	int fd = open(SWAU_DEV_PATH, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		printf("sweetaudio_tone: FAIL open %s errno %d\n", SWAU_DEV_PATH, errno);
		return 1;
	}
	struct swau_info in;
	memset(&in, 0, sizeof in);
	if (ioctl(fd, SWAU_IOC_INFO, &in) != 0 || in.version != SWAU_VERSION || in.rate != SWAU_RATE || in.channels != 2 || in.bits != 16) {
		printf("sweetaudio_tone: FAIL info errno %d v %u rate %u ch %u bits %u\n", errno, in.version, in.rate, in.channels, in.bits);
		close(fd);
		return 1;
	}
	printf("sweetaudio_tone: info fifo %u start %u flags 0x%x max_gain %d mdB; %.2f s of %.0f Hz (%d beeps), -3 dBFS PCM\n", in.fifo_frames,
	    in.start_frames, in.flags, in.max_gain_mdb, secs, hz, beeps);
	const uint64_t total = (uint64_t)(secs * SWAU_RATE);
	const double amp = 32768.0 * 0.708;    /* -3 dBFS PCM (kernel gain -15 dB by default, xnu patch 0181) */
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	const uint64_t deadline = mach_absolute_time() + (uint64_t)((secs + 2.0) * 1e9 * tb.denom / tb.numer);
	int16_t buf[256 * 2];
	uint64_t done = 0;
	int rc = 0;
	while (done < total) {
		if (mach_absolute_time() > deadline) {
			printf("sweetaudio_tone: FAIL timeout after %llu of %llu frames (FIFO not draining)\n", (unsigned long long)done,
			    (unsigned long long)total);
			rc = 1;
			break;
		}
		uint32_t n = total - done > 256 ? 256 : (uint32_t)(total - done);
		for (uint32_t i = 0; i < n; i++) {
			uint64_t fr = done + i;
			int on = beeps <= 0 || (fr / (SWAU_RATE / 2)) % 2 == 0;
			int16_t v = on ? (int16_t)lrint(amp * sin(2.0 * M_PI * hz * (double)fr / SWAU_RATE)) : 0;
			buf[2 * i] = buf[2 * i + 1] = v;
		}
		ssize_t w = write(fd, buf, n * SWAU_BYTES_PER_FRAME);
		if (w < 0) {
			printf("sweetaudio_tone: FAIL write errno %d after %llu frames\n", errno, (unsigned long long)done);
			rc = 1;
			break;
		}
		done += (uint64_t)w / SWAU_BYTES_PER_FRAME;
		if ((uint64_t)w < n * SWAU_BYTES_PER_FRAME)
			usleep(1000);   /* FIFO full: wait for the AOC feed */
	}
	struct swau_pos p;
	memset(&p, 0, sizeof p);
	int perr = ioctl(fd, SWAU_IOC_POS, &p) == 0 ? 0 : errno;
	close(fd);
	if (rc == 0)
		printf("sweetaudio_tone: done: %llu frames written, played %llu, fifo %u, %u underruns, %u overruns (pos errno %d)\n",
		    (unsigned long long)done, (unsigned long long)p.frames_played, p.fifo_fill, p.underruns, p.overruns, perr);
	return rc;
}
