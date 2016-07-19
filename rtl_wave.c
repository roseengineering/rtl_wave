/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include "getopt/getopt.h"
#endif

#include "rtl-sdr.h"
#include "convenience.h"

#define DEFAULT_SAMPLE_RATE		2048000
#define DEFAULT_BUF_LENGTH		(16 * 16384)
#define MINIMAL_BUF_LENGTH		512
#define MAXIMAL_BUF_LENGTH		(256 * 16384)

static int do_exit = 0;
static uint64_t bytes_to_read = 0;
static rtlsdr_dev_t *dev = NULL;

void usage(void)
{
	fprintf(stderr,
		"rtl_sdr, an I/Q recorder for RTL2832 based DVB-T receivers\n\n"
		"Usage:\t -f frequency_to_tune_to [Hz]\n"
		"\t[-s samplerate (default: 2048000 Hz)]\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-g gain (default: 0 for auto)]\n"
		"\t[-p ppm_error (default: 0)]\n"
		"\t[-b output_block_size (default: 16 * 16384)]\n"
		"\t[-n number of samples to read (default: 0, infinite)]\n"
		"\t[-S force sync output (default: async)]\n"
		"\tfilename (a '-' dumps samples to stdout)\n\n");
	exit(1);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		rtlsdr_cancel_async(dev);
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
	do_exit = 1;
	rtlsdr_cancel_async(dev);
}
#endif

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	if (ctx) {
		if (do_exit)
			return;

		if ((bytes_to_read > 0) && (bytes_to_read < len)) {
			len = bytes_to_read;
			do_exit = 1;
			rtlsdr_cancel_async(dev);
		}

		for (int n=0; n<len; n++) buf[n] = buf[n] - 128;

		if (fwrite(buf, 1, len, (FILE*)ctx) != len) {
			fprintf(stderr, "Short write, samples lost, exiting!\n");
			rtlsdr_cancel_async(dev);
		}

		if (bytes_to_read > 0)
			bytes_to_read -= len;
	}
}



///////////////////////////////////

// datetime

typedef struct {
    uint16_t year;
    uint16_t month;
    uint16_t day_of_week;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
    uint16_t milliseconds;
} __attribute__((packed)) datetime_t;

// riff

typedef struct {
    char id[4];
    uint32_t size;
    char type[4];
} __attribute__((packed)) riff_t;

// fmt

typedef struct {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t samples_per_sec;
    uint32_t data_rate;
    uint16_t block_size;
    uint16_t bits_per_sample;
} __attribute__((packed)) fmt_t;

// auxi

typedef struct {
    datetime_t start_time;
    datetime_t stop_time;
    uint32_t frequency; //receiver center frequency
    uint32_t sample_frequency; //A/D sample frequency before downsampling
    uint32_t if_frequency; //IF freq if an external down converter is used
    uint32_t bandwidth; //displayable BW
    uint32_t dc_offset; //DC offset of I/Q channels in 1/1000's of a count
} __attribute__((packed)) auxi_t;

// chunk

typedef struct {
    char id[4];
    uint32_t size;
} __attribute__((packed)) chunk_t;


void set_datetime(datetime_t* dt)
{
    time_t rawtime = time(NULL);
    struct tm *tm = gmtime(&rawtime);
    dt->year = tm->tm_year + 1900;
    dt->month = tm->tm_mon + 1;
    dt->day = tm->tm_mday;
    dt->hour = tm->tm_hour;
    dt->minute = tm->tm_min;
    dt->second = tm->tm_sec;
}

void wave_header(FILE *file, uint32_t samp_rate, uint32_t frequency, uint32_t bits_per_sample)
{
    riff_t riff;
    fmt_t fmt;
    chunk_t chunk;
    auxi_t auxi;

    // write riff header
    memset(&riff, 0, sizeof(riff_t));
    strncpy(riff.id, "RIFF", 4);
    strncpy(riff.type, "WAVE", 4);
    riff.size = -1;
    if (fwrite(&riff, 1, sizeof(riff_t), file) != sizeof(riff_t)) exit(1);

    // write fmt header
    memset(&chunk, 0, sizeof(chunk_t));
    strncpy(chunk.id, "fmt ", 4);
    chunk.size = sizeof(fmt_t);
    if (fwrite(&chunk, 1, sizeof(chunk_t), file) != sizeof(chunk_t)) exit(1);

    // write fmt data
    memset(&fmt, 0, sizeof(fmt_t));
    fmt.format_tag = 1; // PCM
    fmt.channels = 2;
    fmt.bits_per_sample = bits_per_sample;
    fmt.samples_per_sec = samp_rate;
    fmt.data_rate = fmt.channels * fmt.bits_per_sample / 8 * fmt.samples_per_sec;
    fmt.block_size = fmt.channels * fmt.bits_per_sample / 8;
    if (fwrite(&fmt, 1, sizeof(fmt_t), file) != sizeof(fmt_t)) exit(1);

    // write auxi header
    memset(&chunk, 0, sizeof(chunk_t));
    strncpy(chunk.id, "auxi", 4);
    chunk.size = sizeof(auxi_t);
    if (fwrite(&chunk, 1, sizeof(chunk_t), file) != sizeof(chunk_t)) exit(1);

    // write auxi data
    memset(&auxi, 0, sizeof(auxi_t));
    auxi.frequency = frequency;
    set_datetime(&auxi.start_time);
    if (fwrite(&auxi, 1, sizeof(auxi_t), file) != sizeof(auxi_t)) exit(1);

    // write data header
    strncpy(chunk.id, "data", 4);
    chunk.size = -1;
    if (fwrite(&chunk, 1, sizeof(chunk_t), file) != sizeof(chunk_t)) exit(1);
}

float db(float x)
{
    return 10 * logf(x);    
}

int interval_seconds = 2; 

///////////////////////////////////


int main(int argc, char **argv)
{
#ifndef _WIN32
	struct sigaction sigact;
#endif
	char *filename = NULL;
	int n_read;
	int r, opt;
	int gain = 0;
	int ppm_error = 0;
	int sync_mode = 0;
	FILE *file;
	uint8_t *buffer;
	int dev_index = 0;
	int dev_given = 0;
	uint32_t frequency = 100000000;
	uint32_t samp_rate = DEFAULT_SAMPLE_RATE;
	uint32_t out_block_size = DEFAULT_BUF_LENGTH;

	while ((opt = getopt(argc, argv, "d:f:g:s:b:n:p:S")) != -1) {
		switch (opt) {
		case 'd':
			dev_index = verbose_device_search(optarg);
			dev_given = 1;
			break;
		case 'f':
			frequency = (uint32_t)atofs(optarg);
			break;
		case 'g':
			gain = (int)(atof(optarg) * 10); /* tenths of a dB */
			break;
		case 's':
			samp_rate = (uint32_t)atofs(optarg);
			break;
		case 'p':
			ppm_error = atoi(optarg);
			break;
		case 'b':
			out_block_size = (uint32_t)atof(optarg);
			break;
		case 'n':
			bytes_to_read = (uint32_t)atof(optarg) * 2;
			break;
		case 'S':
			sync_mode = 1;
			break;
		default:
			usage();
			break;
		}
	}

	if (argc <= optind) {
		usage();
	} else {
		filename = argv[optind];
	}

	if(out_block_size < MINIMAL_BUF_LENGTH ||
	   out_block_size > MAXIMAL_BUF_LENGTH ){
		fprintf(stderr,
			"Output block size wrong value, falling back to default\n");
		fprintf(stderr,
			"Minimal length: %u\n", MINIMAL_BUF_LENGTH);
		fprintf(stderr,
			"Maximal length: %u\n", MAXIMAL_BUF_LENGTH);
		out_block_size = DEFAULT_BUF_LENGTH;
	}

	buffer = malloc(out_block_size * sizeof(uint8_t));

	if (!dev_given) {
		dev_index = verbose_device_search("0");
	}

	if (dev_index < 0) {
		exit(1);
	}

	r = rtlsdr_open(&dev, (uint32_t)dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
		exit(1);
	}
#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif
	/* Set the sample rate */
	verbose_set_sample_rate(dev, samp_rate);

	/* Set the frequency */
	verbose_set_frequency(dev, frequency);

	if (0 == gain) {
		 /* Enable automatic gain */
		verbose_auto_gain(dev);
	} else {
		/* Enable manual gain */
		gain = nearest_gain(dev, gain);
		verbose_gain_set(dev, gain);
	}

	verbose_ppm_set(dev, ppm_error);

	if(strcmp(filename, "-") == 0) { /* Write samples to stdout */
		file = stdout;
#ifdef _WIN32
		_setmode(_fileno(stdin), _O_BINARY);
#endif
	} else {
		file = fopen(filename, "wb");
		if (!file) {
			fprintf(stderr, "Failed to open %s\n", filename);
			goto out;
		}
	}


        //////////////////////////////////////////

        int interval = 0;
        float ipeak = 0, qpeak = 0;
        float iavg = 0, qavg = 0;

	wave_header(file, samp_rate, frequency, 8);

        //////////////////////////////////////////


	/* Reset endpoint before we start reading from it (mandatory) */
	verbose_reset_buffer(dev);

	if (sync_mode) {
		fprintf(stderr, "Reading samples in sync mode...\n");
		while (!do_exit) {
			r = rtlsdr_read_sync(dev, buffer, out_block_size, &n_read);
			if (r < 0) {
				fprintf(stderr, "WARNING: sync read failed.\n");
				break;
			}

			if ((bytes_to_read > 0) && (bytes_to_read < (uint32_t)n_read)) {
				n_read = bytes_to_read;
				do_exit = 1;
			}


        //////////////////////////////////////////

        for (int i=0; i < n_read; i += 2)
        {
            float ival = (float) (buffer[i] - 128) / 128;
            float qval = (float) (buffer[i+1] - 128) / 128;
            ival = ival * ival;
            qval = qval * qval;
            iavg += ival;
            qavg += qval;
            if (ival > ipeak) ipeak = ival;
            if (qval > qpeak) qpeak = qval;
        }

        interval += n_read / 2;
        if (interval > samp_rate * interval_seconds){
            iavg /= interval;
            qavg /= interval;
            fprintf(stderr, "PEAK %5.1f | %5.1f dBFS   PAR %4.1f | %4.1f dB\n",
                    db(ipeak), db(qpeak), db(ipeak / iavg), db(qpeak / qavg));
            interval = 0;
            ipeak = qpeak = iavg = qavg = 0; 
        };

        //////////////////////////////////////////


			for (int n=0; n<n_read; n++) buffer[n] = buffer[n] - 128;

			if (fwrite(buffer, 1, n_read, file) != (size_t)n_read) {
				fprintf(stderr, "Short write, samples lost, exiting!\n");
				break;
			}

			if ((uint32_t)n_read < out_block_size) {
				fprintf(stderr, "Short read, samples lost, exiting!\n");
				break;
			}

			if (bytes_to_read > 0)
				bytes_to_read -= n_read;
		}
	} else {
		fprintf(stderr, "Reading samples in async mode...\n");
		r = rtlsdr_read_async(dev, rtlsdr_callback, (void *)file,
				      0, out_block_size);
	}

	if (do_exit)
		fprintf(stderr, "\nUser cancel, exiting...\n");
	else
		fprintf(stderr, "\nLibrary error %d, exiting...\n", r);

	if (file != stdout)
		fclose(file);

	rtlsdr_close(dev);
	free (buffer);
out:
	return r >= 0 ? r : -r;
}
