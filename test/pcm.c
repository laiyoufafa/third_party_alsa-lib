/*
 *  This small demo sends a simple sinusoidal wave to your speakers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include "../include/asoundlib.h"
#include <sys/time.h>
#include <math.h>

char *device = "plughw:0,0";			 /* playback device */
snd_pcm_format_t format = SND_PCM_FORMAT_S16;	 /* sample format */
int rate = 44100;				 /* stream rate */
int channels = 1;				 /* count of channels */
int buffer_time = 500000;			 /* ring buffer length in us */
int period_time = 100000;			 /* period time in us */
double freq = 440;				 /* sinusoidal wave frequency in Hz */

snd_pcm_sframes_t buffer_size;
snd_pcm_sframes_t period_size;
snd_output_t *output = NULL;

static void generate_sine(const snd_pcm_channel_area_t *areas, 
			  snd_pcm_uframes_t offset,
			  int count, double *_phase)
{
	double phase = *_phase;
	double max_phase = 1.0 / freq;
	double step = 1.0 / (double)rate;
	double res;
	signed short *samples[channels];
	int steps[channels];
	int chn, ires;
	
	/* verify and prepare the contents of areas */
	for (chn = 0; chn < channels; chn++) {
		if ((areas[chn].first % 8) != 0) {
			printf("areas[%i].first == %i, aborting...\n", chn, areas[chn].first);
			exit(EXIT_FAILURE);
		}
		samples[chn] = (signed short *)(((unsigned char *)areas[chn].addr) + (areas[chn].first / 8));
		if ((areas[chn].step % 16) != 0) {
			printf("areas[%i].step == %i, aborting...\n", chn, areas[chn].step);
			exit(EXIT_FAILURE);
		}
		steps[chn] = areas[chn].step / 16;
		samples[chn] += offset * steps[chn];
	}
	/* fill the channel areas */
	while (count-- > 0) {
		res = sin((phase * 2 * M_PI) / max_phase - M_PI) * 32767;
		ires = res;
		for (chn = 0; chn < channels; chn++) {
			*samples[chn] = ires;
			samples[chn] += steps[chn];
		}
		phase += step;
		if (phase >= max_phase)
			phase -= max_phase;
	}
	*_phase = phase;
}

static int set_hwparams(snd_pcm_t *handle,
			snd_pcm_hw_params_t *params,
			snd_pcm_access_t access)
{
	int err, dir;

	/* choose all parameters */
	err = snd_pcm_hw_params_any(handle, params);
	if (err < 0) {
		printf("Broken configuration for playback: no configurations available: %s\n", snd_strerror(err));
		return err;
	}
	/* set the interleaved read/write format */
	err = snd_pcm_hw_params_set_access(handle, params, access);
	if (err < 0) {
		printf("Access type not available for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* set the sample format */
	err = snd_pcm_hw_params_set_format(handle, params, format);
	if (err < 0) {
		printf("Sample format not available for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* set the count of channels */
	err = snd_pcm_hw_params_set_channels(handle, params, channels);
	if (err < 0) {
		printf("Channels count (%i) not available for playbacks: %s\n", channels, snd_strerror(err));
		return err;
	}
	/* set the stream rate */
	err = snd_pcm_hw_params_set_rate_near(handle, params, rate, 0);
	if (err < 0) {
		printf("Rate %iHz not available for playback: %s\n", rate, snd_strerror(err));
		return err;
	}
	if (err != rate) {
		printf("Rate doesn't match (requested %iHz, get %iHz)\n", rate, err);
		return -EINVAL;
	}
	/* set the buffer time */
	err = snd_pcm_hw_params_set_buffer_time_near(handle, params, buffer_time, &dir);
	if (err < 0) {
		printf("Unable to set buffer time %i for playback: %s\n", buffer_time, snd_strerror(err));
		return err;
	}
	buffer_size = snd_pcm_hw_params_get_buffer_size(params);
	/* set the period time */
	err = snd_pcm_hw_params_set_period_time_near(handle, params, period_time, &dir);
	if (err < 0) {
		printf("Unable to set period time %i for playback: %s\n", period_time, snd_strerror(err));
		return err;
	}
	period_size = snd_pcm_hw_params_get_period_size(params, &dir);
	/* write the parameters to device */
	err = snd_pcm_hw_params(handle, params);
	if (err < 0) {
		printf("Unable to set hw params for playback: %s\n", snd_strerror(err));
		return err;
	}
	return 0;
}

static int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams)
{
	int err;

	/* get the current swparams */
	err = snd_pcm_sw_params_current(handle, swparams);
	if (err < 0) {
		printf("Unable to determine current swparams for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* start the transfer when the buffer is full */
	err = snd_pcm_sw_params_set_start_threshold(handle, swparams, buffer_size);
	if (err < 0) {
		printf("Unable to set start threshold mode for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* allow the transfer when at least period_size samples can be processed */
	err = snd_pcm_sw_params_set_avail_min(handle, swparams, period_size);
	if (err < 0) {
		printf("Unable to set avail min for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* align all transfers to 1 sample */
	err = snd_pcm_sw_params_set_xfer_align(handle, swparams, 1);
	if (err < 0) {
		printf("Unable to set transfer align for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* write the parameters to the playback device */
	err = snd_pcm_sw_params(handle, swparams);
	if (err < 0) {
		printf("Unable to set sw params for playback: %s\n", snd_strerror(err));
		return err;
	}
	return 0;
}

/*
 *   Underrun and suspend recovery
 */
 
static int xrun_recovery(snd_pcm_t *handle, int err)
{
	if (err == -EPIPE) {	/* under-run */
		err = snd_pcm_prepare(handle);
		if (err < 0)
			printf("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
		return 0;
	} else if (err == -ESTRPIPE) {
		while ((err = snd_pcm_resume(handle)) == -EAGAIN)
			sleep(1);	/* wait until the suspend flag is released */
		if (err < 0) {
			err = snd_pcm_prepare(handle);
			if (err < 0)
				printf("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
		}
		return 0;
	}
	return err;
}

/*
 *   Transfer method - write only
 */

static int write_loop(snd_pcm_t *handle,
		      signed short *samples,
		      snd_pcm_channel_area_t *areas)
{
	double phase = 0;
	signed short *ptr;
	int err, cptr;

	while (1) {
		generate_sine(areas, 0, period_size, &phase);
		ptr = samples;
		cptr = period_size;
		while (cptr > 0) {
			err = snd_pcm_writei(handle, ptr, cptr);
			if (err == -EAGAIN)
				continue;
			if (err < 0) {
				if (xrun_recovery(handle, err) < 0) {
					printf("Write error: %s\n", snd_strerror(err));
					exit(EXIT_FAILURE);
				}
				break;	/* skip one period */
			}
			ptr += err * channels;
			cptr -= err;
		}
	}
}
 
/*
 *   Transfer method - write and wait for room in buffer using poll
 */

static int wait_for_poll(snd_pcm_t *handle, struct pollfd *ufds, unsigned int count)
{
	unsigned short revents;

	while (1) {
		poll(ufds, count, -1);
		snd_pcm_poll_descriptors_revents(handle, ufds, count, &revents);
		if (revents & POLLERR)
			return -EIO;
		if (revents & POLLOUT)
			return 0;
	}
}

static int write_and_poll_loop(snd_pcm_t *handle,
			       signed short *samples,
			       snd_pcm_channel_area_t *areas)
{
	struct pollfd *ufds;
	double phase = 0;
	signed short *ptr;
	int err, count, cptr, init;

	count = snd_pcm_poll_descriptors_count (handle);
	if (count <= 0) {
		printf("Invalid poll descriptors count\n");
		return count;
	}

	ufds = malloc(sizeof(struct pollfd) * count);
	if (ufds == NULL) {
		printf("No enough memory\n");
		return err;;
	}
	if ((err = snd_pcm_poll_descriptors(handle, ufds, count)) < 0) {
		printf("Unable to obtain poll descriptors for playback: %s\n", snd_strerror(err));
		return err;
	}

	init = 1;
	while (1) {
		if (!init) {
			err = wait_for_poll(handle, ufds, count);
			if (err < 0) {
				if (snd_pcm_state(handle) == SND_PCM_STATE_XRUN ||
				    snd_pcm_state(handle) == SND_PCM_STATE_SUSPENDED) {
					err = snd_pcm_state(handle) == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE;
					if (xrun_recovery(handle, err) < 0) {
						printf("Write error: %s\n", snd_strerror(err));
						exit(EXIT_FAILURE);
					}
					init = 1;
				} else {
					printf("Wait for poll failed\n");
					return err;
				}
			}
		}

		generate_sine(areas, 0, period_size, &phase);
		ptr = samples;
		cptr = period_size;
		while (cptr > 0) {
			err = snd_pcm_writei(handle, ptr, cptr);
			if (err < 0) {
				if (xrun_recovery(handle, err) < 0) {
					printf("Write error: %s\n", snd_strerror(err));
					exit(EXIT_FAILURE);
				}
				init = 1;
				break;	/* skip one period */
			}
			if (snd_pcm_state(handle) == SND_PCM_STATE_RUNNING)
				init = 0;
			ptr += err * channels;
			cptr -= err;
			if (cptr == 0)
				break;
			/* it is possible, that the initial buffer cannot store */
			/* all data from the last period, so wait awhile */
			err = wait_for_poll(handle, ufds, count);
			if (err < 0) {
				if (snd_pcm_state(handle) == SND_PCM_STATE_XRUN ||
				    snd_pcm_state(handle) == SND_PCM_STATE_SUSPENDED) {
					err = snd_pcm_state(handle) == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE;
					if (xrun_recovery(handle, err) < 0) {
						printf("Write error: %s\n", snd_strerror(err));
						exit(EXIT_FAILURE);
					}
					init = 1;
				} else {
					printf("Wait for poll failed\n");
					return err;
				}
			}
		}
	}
}

/*
 *   Transfer method - asynchronous notification
 */

struct async_private_data {
	signed short *samples;
	snd_pcm_channel_area_t *areas;
	double phase;
};

static void async_callback(snd_async_handler_t *ahandler)
{
	snd_pcm_t *handle = snd_async_handler_get_pcm(ahandler);
	struct async_private_data *data = snd_async_handler_get_callback_private(ahandler);
	signed short *samples = data->samples;
	snd_pcm_channel_area_t *areas = data->areas;
	snd_pcm_sframes_t avail;
	int err;
	
	avail = snd_pcm_avail_update(handle);
	while (avail >= period_size) {
		generate_sine(areas, 0, period_size, &data->phase);
		err = snd_pcm_writei(handle, samples, period_size);
		if (err < 0) {
			printf("Initial write error: %s\n", snd_strerror(err));
			exit(EXIT_FAILURE);
		}
		if (err != period_size) {
			printf("Initial write error: written %i expected %li\n", err, period_size);
			exit(EXIT_FAILURE);
		}
		avail = snd_pcm_avail_update(handle);
	}
}

static int async_loop(snd_pcm_t *handle,
		      signed short *samples,
		      snd_pcm_channel_area_t *areas)
{
	struct async_private_data data;
	snd_async_handler_t *ahandler;
	int err, count;

	data.samples = samples;
	data.areas = areas;
	data.phase = 0;
	err = snd_async_add_pcm_handler(&ahandler, handle, async_callback, &data);
	if (err < 0) {
		printf("Unable to register async handler\n");
		exit(EXIT_FAILURE);
	}
	for (count = 0; count < 2; count++) {
		generate_sine(areas, 0, period_size, &data.phase);
		err = snd_pcm_writei(handle, samples, period_size);
		if (err < 0) {
			printf("Initial write error: %s\n", snd_strerror(err));
			exit(EXIT_FAILURE);
		}
		if (err != period_size) {
			printf("Initial write error: written %i expected %li\n", err, period_size);
			exit(EXIT_FAILURE);
		}
	}
	err = snd_pcm_start(handle);
	if (err < 0) {
		printf("Start error: %s\n", snd_strerror(err));
		exit(EXIT_FAILURE);
	}

	/* because all other work is done in the signal handler,
	   suspend the process */
	while (1) {
		sleep(1);
	}
}

/*
 *   Transfer method - direct write only
 */

static int direct_loop(snd_pcm_t *handle,
		       signed short *samples,
		       snd_pcm_channel_area_t *areas)
{
	double phase = 0;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t offset, frames, size;
	snd_pcm_sframes_t avail, commitres;
	snd_pcm_state_t state;
	int err, first = 1;

	while (1) {
		state = snd_pcm_state(handle);
		if (state == SND_PCM_STATE_XRUN) {
			err = xrun_recovery(handle, -EPIPE);
			if (err < 0) {
				printf("XRUN recovery failed: %s\n", snd_strerror(err));
				return err;
			}
			first = 1;
		} else if (state == SND_PCM_STATE_SUSPENDED) {
			err = xrun_recovery(handle, -ESTRPIPE);
			if (err < 0) {
				printf("SUSPEND recovery failed: %s\n", snd_strerror(err));
				return err;
			}
		}
		avail = snd_pcm_avail_update(handle);
		if (avail < 0) {
			err = xrun_recovery(handle, avail);
			if (err < 0) {
				printf("avail update failed: %s\n", snd_strerror(err));
				return err;
			}
			first = 1;
			continue;
		}
		if (avail < period_size) {
			if (first) {
				first = 0;
				err = snd_pcm_start(handle);
				if (err < 0) {
					printf("Start error: %s\n", snd_strerror(err));
					exit(EXIT_FAILURE);
				}
			} else {
				err = snd_pcm_wait(handle, -1);
				if (err < 0) {
					if ((err = xrun_recovery(handle, err)) < 0) {
						printf("snd_pcm_wait error: %s\n", snd_strerror(err));
						exit(EXIT_FAILURE);
					}
					first = 1;
				}
			}
			continue;
		}
		size = period_size;
		while (size > 0) {
			frames = size;
			err = snd_pcm_mmap_begin(handle, &my_areas, &offset, &frames);
			if (err < 0) {
				if ((err = xrun_recovery(handle, err)) < 0) {
					printf("MMAP begin avail error: %s\n", snd_strerror(err));
					exit(EXIT_FAILURE);
				}
				first = 1;
			}
			generate_sine(my_areas, offset, frames, &phase);
			commitres = snd_pcm_mmap_commit(handle, offset, frames);
			if (commitres < 0 || commitres != frames) {
				if ((err = xrun_recovery(handle, commitres >= 0 ? -EPIPE : commitres)) < 0) {
					printf("MMAP commit error: %s\n", snd_strerror(err));
					exit(EXIT_FAILURE);
				}
				first = 1;
			}
			size -= frames;
		}
	}
}
 
/*
 *   Transfer method - direct write only using mmap_write functions
 */

static int direct_write_loop(snd_pcm_t *handle,
			     signed short *samples,
			     snd_pcm_channel_area_t *areas)
{
	double phase = 0;
	signed short *ptr;
	int err, cptr;

	while (1) {
		generate_sine(areas, 0, period_size, &phase);
		ptr = samples;
		cptr = period_size;
		while (cptr > 0) {
			err = snd_pcm_mmap_writei(handle, ptr, cptr);
			if (err == -EAGAIN)
				continue;
			if (err < 0) {
				if (xrun_recovery(handle, err) < 0) {
					printf("Write error: %s\n", snd_strerror(err));
					exit(EXIT_FAILURE);
				}
				break;	/* skip one period */
			}
			ptr += err * channels;
			cptr -= err;
		}
	}
}
 
/*
 *
 */

struct transfer_method {
	const char *name;
	snd_pcm_access_t access;
	int (*transfer_loop)(snd_pcm_t *handle,
			     signed short *samples,
			     snd_pcm_channel_area_t *areas);
};

static struct transfer_method transfer_methods[] = {
	{ "write", SND_PCM_ACCESS_RW_INTERLEAVED, write_loop },
	{ "write_and_poll", SND_PCM_ACCESS_RW_INTERLEAVED, write_and_poll_loop },
	{ "async", SND_PCM_ACCESS_RW_INTERLEAVED, async_loop },
	{ "direct_interleaved", SND_PCM_ACCESS_MMAP_INTERLEAVED, direct_loop },
	{ "direct_noninterleaved", SND_PCM_ACCESS_MMAP_NONINTERLEAVED, direct_loop },
	{ "direct_write", SND_PCM_ACCESS_MMAP_INTERLEAVED, direct_write_loop },
	{ NULL, SND_PCM_ACCESS_RW_INTERLEAVED, NULL }
};

static void help(void)
{
	int k;
	printf("\
Usage: latency [OPTION]... [FILE]...
-h,--help       help
-D,--device     playback device
-r,--rate	stream rate in Hz
-c,--channels	count of channels in stream
-f,--frequency  sine wave frequency in Hz
-b,--buffer     ring buffer size in samples
-p,--period     period size in us
-m,--method     transfer method

");
        printf("Recognized sample formats are:");
        for (k = 0; k < SND_PCM_FORMAT_LAST; ++(unsigned long) k) {
                const char *s = snd_pcm_format_name(k);
                if (s)
                        printf(" %s", s);
        }
        printf("\n");
        printf("Recognized transfer methods are:");
        for (k = 0; transfer_methods[k].name; k++)
        	printf(" %s", transfer_methods[k].name);
	printf("\n");
}

int main(int argc, char *argv[])
{
	struct option long_option[] =
	{
		{"help", 0, NULL, 'h'},
		{"device", 1, NULL, 'D'},
		{"rate", 1, NULL, 'r'},
		{"channels", 1, NULL, 'c'},
		{"frequency", 1, NULL, 'f'},
		{"buffer", 1, NULL, 'b'},
		{"period", 1, NULL, 'p'},
		{"method", 1, NULL, 'm'},
		{NULL, 0, NULL, 0},
	};
	snd_pcm_t *handle;
	int err, morehelp;
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_sw_params_t *swparams;
	int method = 0;
	signed short *samples;
	int chn;
	snd_pcm_channel_area_t *areas;

	snd_pcm_hw_params_alloca(&hwparams);
	snd_pcm_sw_params_alloca(&swparams);

	morehelp = 0;
	while (1) {
		int c;
		if ((c = getopt_long(argc, argv, "hD:r:c:f:b:p:m:", long_option, NULL)) < 0)
			break;
		switch (c) {
		case 'h':
			morehelp++;
			break;
		case 'D':
			device = strdup(optarg);
			break;
		case 'r':
			rate = atoi(optarg);
			rate = rate < 4000 ? 4000 : rate;
			rate = rate > 196000 ? 196000 : rate;
			break;
		case 'c':
			channels = atoi(optarg);
			channels = channels < 1 ? 1 : channels;
			channels = channels > 1024 ? 1024 : channels;
			break;
		case 'f':
			freq = atoi(optarg);
			freq = freq < 50 ? 50 : freq;
			freq = freq > 5000 ? 5000 : freq;
			break;
		case 'b':
			buffer_size = atoi(optarg);
			buffer_size = buffer_size < 64 ? 64 : buffer_size;
			buffer_size = buffer_size > 64*1024 ? 64*1024 : buffer_size;
			break;
		case 'p':
			period_time = atoi(optarg);
			period_time = period_time < 1000 ? 1000 : period_time;
			period_time = period_time > 1000000 ? 1000000 : period_time;
			break;
		case 'm':
			for (method = 0; transfer_methods[method].name; method++)
				if (!strcasecmp(transfer_methods[method].name, optarg))
					break;
			if (transfer_methods[method].name == NULL)
				method = 0;
			break;
		}
	}

	if (morehelp) {
		help();
		return 0;
	}

	err = snd_output_stdio_attach(&output, stdout, 0);
	if (err < 0) {
		printf("Output failed: %s\n", snd_strerror(err));
		return 0;
	}

	printf("Playback device is %s\n", device);
	printf("Stream parameters are %iHz, %s, %i channels\n", rate, snd_pcm_format_name(format), channels);
	printf("Sine wave rate is %.4fHz\n", freq);
	printf("Using transfer method: %s\n", transfer_methods[method].name);

	if ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		printf("Playback open error: %s\n", snd_strerror(err));
		return 0;
	}
	
	if ((err = set_hwparams(handle, hwparams, transfer_methods[method].access)) < 0) {
		printf("Setting of hwparams failed: %s\n", snd_strerror(err));
		exit(EXIT_FAILURE);
	}
	if ((err = set_swparams(handle, swparams)) < 0) {
		printf("Setting of swparams failed: %s\n", snd_strerror(err));
		exit(EXIT_FAILURE);
	}

	samples = malloc((period_size * channels * snd_pcm_format_width(format)) / 8);
	if (samples == NULL) {
		printf("No enough memory\n");
		exit(EXIT_FAILURE);
	}
	
	areas = calloc(channels, sizeof(snd_pcm_channel_area_t));
	if (areas == NULL) {
		printf("No enough memory\n");
		exit(EXIT_FAILURE);
	}
	for (chn = 0; chn < channels; chn++) {
		areas[chn].addr = samples;
		areas[chn].first = chn * 16;
		areas[chn].step = channels * 16;
	}

	err = transfer_methods[method].transfer_loop(handle, samples, areas);
	if (err < 0)
		printf("Transfer failed: %s\n", snd_strerror(err));

	free(areas);
	free(samples);
	snd_pcm_close(handle);
	return 0;
}

