/* SPDX-License-Identifier: MIT */
/*
 * PixOS /dev/sweetaudio: the kernel <-> userland PCM contract (docs/research/54-ios-audio-path.md).
 * Shared by the kernel driver (copy this header next to the cdev), the SweetAudio.driver HAL plug-in and sweetaudio_tone.
 *
 * Format is fixed: 48000 Hz, 2 channels interleaved (L, R), signed 16-bit little endian. 1 frame = 4 bytes.
 * open(O_WRONLY): attaches the one writer (second opener: EBUSY). ENXIO when the output path (AOC ring + amps) is not up.
 * write(): whole frames only (len % 4 != 0: EINVAL). Never blocks: copies min(len, free) bytes into the kernel FIFO and
 *          returns that count (0 = FIFO full, the caller drops). EIO if the path went down after open.
 * close(): the FIFO plays out (bounded by its size), then the kernel feeds silence again.
 */
#ifndef SWEETAUDIO_DEV_H
#define SWEETAUDIO_DEV_H

#include <stdint.h>
#include <sys/ioccom.h>

#define SWAU_DEV_PATH        "/dev/sweetaudio"
#define SWAU_VERSION         1u
#define SWAU_RATE            48000u
#define SWAU_CHANNELS        2u
#define SWAU_BYTES_PER_FRAME 4u
#define SWAU_FIFO_FRAMES     8192u   /* kernel FIFO capacity (170.7 ms) */
#define SWAU_START_FRAMES    960u    /* the kernel starts draining once this much is queued (20 ms); refilled after an underrun */

struct swau_info {                   /* SWAU_IOC_INFO */
	uint32_t version;                /* SWAU_VERSION */
	uint32_t rate;                   /* SWAU_RATE */
	uint16_t channels;               /* SWAU_CHANNELS */
	uint16_t bits;                   /* 16 */
	uint32_t fifo_frames;            /* SWAU_FIFO_FRAMES */
	uint32_t start_frames;           /* SWAU_START_FRAMES */
	uint32_t flags;                  /* SWAU_F_* */
	int32_t  max_gain_mdb;           /* kernel gain clamp in milli-dB (<= 0), plan rule A-3 */
	uint32_t reserved[3];
};
#define SWAU_F_PATH_UP   0x1u        /* AOC playback ring + amps running */
#define SWAU_F_SILENCED  0x2u        /* the kernel discards PCM (safety stop); writes still succeed */

struct swau_pos {                    /* SWAU_IOC_POS */
	uint64_t frames_played;          /* frames taken from the FIFO by the AOC feed since open (silence not counted) */
	uint64_t host_time;              /* mach_absolute_time() when frames_played was sampled */
	uint32_t fifo_fill;              /* frames queued now */
	uint32_t underruns;              /* FIFO-empty events since open */
	uint32_t overruns;               /* bytes refused (FIFO full) since open / 4 */
	uint32_t reserved;
};

#define SWAU_IOC_INFO     _IOR('a', 1, struct swau_info)
#define SWAU_IOC_POS      _IOR('a', 2, struct swau_pos)
#define SWAU_IOC_GAIN     _IOW('a', 3, int32_t)      /* requested gain in milli-dB; clamped to [-96000, max_gain_mdb] */
#define SWAU_IOC_FLUSH    _IO('a', 4)                /* drop what is queued (HAL StopIO) */

#endif
