#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <libgen.h>
#include <stdatomic.h>
#include <poll.h>
#include <getopt.h>
#include <math.h>

#include "expandable_buffer.h"
#include "process_utils.h"
#include "temp_files.h"

static volatile sig_atomic_t stop_signalled = 0;
static volatile sig_atomic_t ffmpeg_src_stopped = 0;

static struct {
	union {
		volatile _Atomic pid_t processes[6];
		struct {
			volatile _Atomic pid_t ffmpeg_src_process;
		   	volatile _Atomic pid_t ffmpeg_rst_process;
			volatile _Atomic pid_t waifu2x_process;

			// TODO: Use these
			volatile _Atomic pid_t ffmpeg_src_monitor_process;
			volatile _Atomic pid_t ffmpeg_rst_monitor_process;
			volatile _Atomic pid_t waifu2x_monitor_process;
		};
	};
	temp_frame* temp_frames;
	size_t temp_frame_count;
} session_data = { .processes={0}, .temp_frames=NULL, .temp_frame_count=0};
void stop_program_from_signal(int sig){
	/*if (sig == SIGCHLD){
		int exit_status = 0;
		pid_t exited_pid = wait(&exit_status);
		if (atomic_load(&session_data.ffmpeg_src_process) == exited_pid)
			ffmpeg_src_stopped = 1;
		// Ignore SIGCHLD if it's a clean exit
		// TODO: Unclean exits should be captured also
		if (WIFEXITED(exit_status)) return;
		}*/
	if (stop_signalled) return;
	
	if (sig == SIGINT) signal(SIGINT, SIG_IGN);
	
	int i;
	for (i = 0; i < 6; i++){
		size_t process_to_kill = atomic_load(&session_data.processes[i]);
		if (process_to_kill != 0)
			kill(process_to_kill, SIGINT);
	}
	
	stop_signalled = 1;
}

void cleanup(){
	if (session_data.temp_frames == NULL) return;
	int i;
	for (i = 0; i < session_data.temp_frame_count; i++)
		free_temp_frame(&session_data.temp_frames[i]);
	free(session_data.temp_frames);
	session_data.temp_frames = NULL;
}

// Use waifu2x to upscale 16 images per round
#define MAX_FRAMERATE_CHARACTERS 10
typedef struct {
	float framerate;
	char framerate_str[MAX_FRAMERATE_CHARACTERS];
	unsigned int width;
	unsigned int height;
} source_file_data;
void fill_source_file_data(char* filepath, source_file_data* output, size_t output_length){
	char* ffprobe_command[] = { "ffprobe",
								"-v", "error", // Only log extra messages on error
								"-select_streams", "v:0", // Print data on first video stream
								"-show_entries", "stream=width,height,avg_frame_rate", // Print width, then height, then fps
								"-of", "csv=p=0", // Format as CSV
								filepath,
								NULL };
	pipe_data ffprobe_output_pipe = create_pipe_data();
	pid_t ffprobe_output_pid = run_command(ffprobe_command, NULL, NULL, &ffprobe_output_pipe, 0);
	//pipe_data_close_write_to(&ffprobe_output_pipe);

	float fps_nom, fps_denom = 1.0f;
	FILE* ffprobe_output = fdopen(ffprobe_output_pipe.files.read_from, "r");
	if (fscanf(ffprobe_output, "%u,%u,%f/%f", &output->width, &output->height, &fps_nom, &fps_denom) < 3){ // Allow 3 or 4
		fprintf(stderr, "Error parsing ffprobe output\n");
		exit(1);
	}
	output->framerate = fps_nom / fps_denom;
	snprintf(output->framerate_str, MAX_FRAMERATE_CHARACTERS, "%f", output->framerate);
	
	fclose(ffprobe_output);
	// This ends up closing the pipe?
}

#define DEFAULT_FRAMES_PER_UPSCALE_ROUND 256

static struct {
	char* input_filepath;
	char* output_filepath;
	
	size_t frames_per_upscale_round;

	int should_mute_ffmpeg_source;
	int should_mute_waifu2x;
	int should_mute_ffmpeg_result;

	char* waifu2x_folder;
	char* waifu2x_file;
	char* waifu2x_model;

	unsigned int target_width;
	unsigned int target_height;
	
	int dry_run;
} options = {
	.input_filepath = NULL,
	.output_filepath = NULL,
	.frames_per_upscale_round = DEFAULT_FRAMES_PER_UPSCALE_ROUND,
	.should_mute_ffmpeg_source = 1,
	.should_mute_waifu2x = 0,
	.should_mute_ffmpeg_result = 1,
	.waifu2x_folder = "./waifu2x/",
	.waifu2x_file = "waifu2x.lua",
	.waifu2x_model = "models/photo",
	.dry_run = 0,

	.target_width = 0,
	.target_height = 0
};

void print_help(FILE* file, int argc, char* argv[]){
	fprintf(file, "Usage: %s [--frame-count=<FC>] [--waifu2x=<PATH>] [--waifu2x-model=<NAME>] [--target-size=<SIZE>,<SIZE>] [--] input-file output-file\n", argv[0]);
	fprintf(file, "Options:\n\
 -h --help        Show this screen\n\
 --frame-count    Set the amount of frames that are concurrently processed. Default: %d\n\
 --waifu2x        Set the folder containing waifu2x.lua. waifu2x will be executed from here. Default: ./waifu2x/\n\
 --waifu2x-model  Set the model used by waifu2x to upscale images. Default: models/photo\n\
 --target-size    Set the resultant size of the video. By default the program upscales the video by 2x\n\
 -d --dry-run     Do a dry run without running anything\n",
			DEFAULT_FRAMES_PER_UPSCALE_ROUND);
}
void get_options(int argc, char* argv[]){
	static char* short_options = "hd";
	static struct option long_options[] = {
		{ "frame-count", required_argument, NULL, 'f' },
		{ "waifu2x", required_argument, NULL, 'w' },
		{ "waifu2x-model", required_argument, NULL, 'm' },
		{ "help", no_argument, NULL, 'h' },
		{ "dry-run", no_argument, &options.dry_run, 1 },
		{ "target-size", required_argument, NULL, 's' },
	};
	int option_index = 0;

	while(1){
		char option_code = getopt_long(argc, argv, short_options, long_options, &option_index);
		if (option_code == -1) break;
		
		switch(option_code){
		case 'h':
			print_help(stdout, argc, argv);
			exit(0);
		case 'f':
			if (sscanf(optarg, "%zu", &options.frames_per_upscale_round) != 1){
				fprintf(stderr, "Invalid value for --frame-count: '%s'\n", optarg);
				print_help(stderr, argc, argv);
				exit(1);
			}	
			break;
		case 'w':
			options.waifu2x_folder = optarg;
			break;
		case 'm':
			options.waifu2x_model = optarg;
			break;
		case 'd':
			options.dry_run = 1;
			break;
		case 's':
		{
			char sep;
			if (sscanf(optarg, "%u%c%u", &options.target_width, &sep, &options.target_height) != 3){
				fprintf(stderr, "Invalid value for --target-size: '%s'\n", optarg);
				fprintf(stderr, "Must be <NUMBER>(non-whitespace separator)<NUMBER>\n");
				print_help(stderr, argc, argv);
				exit(1);
			}
			break;
		}
		default:
			fprintf(stderr, "Unhandled argument code '%c'\n", option_code);
		case '?':
			print_help(stderr, argc, argv);
			exit(1);
			break;
		}
	}
	if (argc - optind < 2){
		fprintf(stderr, "Not enough arguments for input/output!\n");
		print_help(stderr, argc, argv);
		exit(1);
	}else if (argc - optind > 2){
		fprintf(stderr, "Too many arguments for input/output!\n");
		print_help(stderr, argc, argv);
		exit(1);
	}else{
		options.input_filepath = argv[optind++];
		options.output_filepath = argv[optind++];
	}
}

int main(int argc, char* argv[]){
	get_options(argc, argv);
	
	/* 
	   Get the framerate of the video
	*/
	if (errno){
		fprintf(stderr, "preframerate err: %s\n", strerror(errno));
		errno = 0;
	}
	source_file_data source_data;
	fill_source_file_data(options.input_filepath, &source_data, 10);
	fprintf(stdout, "Framerate: %f\n", source_data.framerate);
	if (strlen(source_data.framerate_str) == 0) exit(1);
	if (errno){
		fprintf(stderr, "predevnull err: %s\n", strerror(errno));
		errno = 0;
		exit(errno);
	}

	if (options.target_width == 0){
		options.target_width = source_data.width * 2;
	}
	if (options.target_height == 0){
		options.target_height = source_data.height * 2;
	}

	size_t upscale_rounds_from_width = (size_t)ceil(options.target_width * 0.5f / source_data.width);
	size_t upscale_rounds_from_height = (size_t)ceil(options.target_height * 0.5f / source_data.height);
	size_t upscale_rounds = (upscale_rounds_from_width > upscale_rounds_from_height) ? upscale_rounds_from_width : upscale_rounds_from_height;  

	if (options.dry_run){
		fprintf(stdout, "Dry Run:\n\
Input: %s\n\
Output: %s\n\n\
Frames per upscale batch: %zu\n\
Waifu2x folder: %s\n\
Waifu2x file: %s\n\
Waifu2x model: %s\n\
Source Framerate: %f\n\
Source Size: %ux%u\n\
Target Size: %ux%u\n\
Total Upscale Rounds: %zu\n",
				options.input_filepath,
				options.output_filepath,
				options.frames_per_upscale_round,
				options.waifu2x_folder,
				options.waifu2x_file,
				options.waifu2x_model,
				source_data.framerate,
				source_data.width, source_data.height,
				options.target_width, options.target_height,
				upscale_rounds
			);
		exit(0);
	}

	int dev_null_read = open("/dev/null", O_RDONLY);
	int dev_null_write = open("/dev/null", O_WRONLY);
	if (errno){
		fprintf(stderr, "presetuperr: %s\n", strerror(errno));
		errno = 0;
		exit(errno);
	}

	/*
	  Set up the ffmpeg source daemon
	*/
	pipe_data ffmpeg_source_input_pipe = create_pipe_data();
	pipe_data ffmpeg_source_output_pipe = create_pipe_data();

	// Use a filter to make sure ffmpeg outputs a frame for every (1/fps) second, uniformly
	// Use the framerate of the original video
	const char* ffmpeg_filter_command_format = "fps=%s";
	const size_t ffmpeg_filter_command_length =
		strlen(ffmpeg_filter_command_format) +
		strlen(source_data.framerate_str) + 1;
	char* ffmpeg_filter_command = calloc(ffmpeg_filter_command_length, sizeof(char));
	snprintf(ffmpeg_filter_command, ffmpeg_filter_command_length, ffmpeg_filter_command_format, source_data.framerate_str);
	char* ffmpeg_source_command[] = { "ffmpeg", "-y",
									  "-i", options.input_filepath,
									  "-vf", ffmpeg_filter_command,
									  "-hide_banner", "-loglevel", "panic", "-nostats", // Logging bits
									  "-vcodec", "png", "-f", "image2pipe", "-",
									  NULL };
	pid_t ffmpeg_source_pid = run_command(ffmpeg_source_command, NULL, &ffmpeg_source_input_pipe, &ffmpeg_source_output_pipe, NULL);
	atomic_store(&session_data.ffmpeg_src_process, ffmpeg_source_pid);
	free(ffmpeg_filter_command);
	
	dup2(dev_null_read, ffmpeg_source_input_pipe.files.write_to); // Send /dev/null to the input so that it doesn't use the terminal
	//pipe_data_close_read_from(&ffmpeg_source_input_pipe); // We shouldn't be able to read from the input
	//pipe_data_close_write_to(&ffmpeg_source_output_pipe); // We shouldn't be able to write to the output
    FILE *ffmpeg_source_output = fdopen(ffmpeg_source_output_pipe.files.read_from, "r"); // Open the output as a pipe so we can read it
	//pipe_data_close_write_to(&ffmpeg_source_progress_pipe); // We shouldn't be able to write to the progress
	
	if (errno){
		fprintf(stderr, "post ffmpeg src err: %s\n", strerror(errno));
		errno = 0;
	}
	
	/*
	  Set up the ffmpeg result daemon
	*/
	pipe_data ffmpeg_result_input_pipe = create_pipe_data();
	//pipe_data ffmpeg_result_output_pipe = create_pipe_data();
	pipe_data ffmpeg_result_progress_pipe = create_pipe_data();

	char* ffmpeg_scale_filter_format = "scale=%u:%u";
	const size_t ffmpeg_scale_filter_length = strlen(ffmpeg_scale_filter_format)
		+ ((options.target_width / 10) + 1) // Worst case estimate for width digits
		+ ((options.target_height / 10) + 1) // Worst case estimate for height digits
		+ 1; // Null terminator
	char* ffmpeg_scale_filter = calloc(ffmpeg_scale_filter_length, sizeof(char));
	snprintf(ffmpeg_scale_filter, ffmpeg_scale_filter_length, ffmpeg_scale_filter_format,
			 options.target_width, options.target_height);
	char* ffmpeg_result_command[] = { "ffmpeg", "-y",
									  "-hide_banner", "-loglevel", "panic", "-progress", "/dev/stderr", "-nostats", // Logging bits
									  "-i", options.input_filepath,
									  "-r", source_data.framerate_str,
									  "-vcodec", "png", "-f", "image2pipe", "-i", "-",
									  "-max_muxing_queue_size", "9999", // Fixes a bug with ffmpeg
									  "-map", "1:v:0", "-map", "0:a:0",
									  "-vf", ffmpeg_scale_filter,
									  options.output_filepath,
									  NULL };
	pid_t ffmpeg_result_pid = run_command(ffmpeg_result_command, NULL, &ffmpeg_result_input_pipe, NULL, &ffmpeg_result_progress_pipe);
	atomic_store(&session_data.ffmpeg_rst_process, ffmpeg_result_pid);
	free(ffmpeg_scale_filter);
	
	FILE *ffmpeg_result_input = fdopen(ffmpeg_result_input_pipe.files.write_to, "w"); // Open the input as a pipe so we can put images in it
	//pipe_data_close_read_from(&ffmpeg_result_input_pipe); // We shouldn't be able to read from the input
	//pipe_data_close_write_to(&ffmpeg_result_output_pipe); // We shouldn't be able to write to the output
	//dup2(dev_null_write, ffmpeg_result_output_pipe.files.read_from); // Send the output to /dev/null because we don't care about it

	char* ffmpeg_grep_filter_command[] = { "sed", "-n", "-r", "s/frame=([0-9]+)/\\1/p", "-", NULL};
	pipe_data ffmpeg_result_framecount_pipe = create_pipe_data();
	pid_t ffmpeg_result_progress_pid = run_command(ffmpeg_grep_filter_command, NULL, &ffmpeg_result_progress_pipe, &ffmpeg_result_framecount_pipe, NULL);
	FILE* ffmpeg_result_framecount_file = fdopen(ffmpeg_result_framecount_pipe.files.read_from, "r");
	
	if (errno){
		fprintf(stderr, "post ffmpeg res err: %s\n", strerror(errno));
		errno = 0;
	}
	
	// Set upu the interrupt handles
	
	struct sigaction sigint_action;
	sigint_action.sa_handler = stop_program_from_signal;
	sigaction(SIGINT, &sigint_action, NULL);
	sigaction(SIGPIPE, &sigint_action, NULL);
	//sigaction(SIGCHLD, &sigint_action, NULL);
	//sigaction(SIGQUIT, &sigint_action, NULL);
	
	// Set up the temp files AFTER the interrupt handler is set
	// If someone Ctrl-C's before this, we'll die right away
	// If someone Ctrl-C's after this, we'll get rid of the tempfiles properly
	session_data.temp_frame_count = options.frames_per_upscale_round;
	session_data.temp_frames = calloc(session_data.temp_frame_count, sizeof(temp_frame));
	{
		int i;
		for (i = 0; i < session_data.temp_frame_count; i++) session_data.temp_frames[i] = create_temp_frame();
	}
	atexit(cleanup);
	
    // waifu2x doesn't like using stdout
	char* waifu2x_noise_command[] = { "th", options.waifu2x_file,
									  "-force_cudnn", "1",
									  "-model_dir", options.waifu2x_model,
									  "-m", "noise_scale", "-noise_level", "1",
									  "-l", "/dev/stdin",
									  "-o", session_data.temp_frames[0].generic_output_filename, // TODO: Only do one calculation for generic_output_filename
									  NULL };
	char* waifu2x_scale_only_command[] = { "th", options.waifu2x_file,
										   "-force_cudnn", "1",
										   "-model_dir", options.waifu2x_model,
										   "-m", "scale",
										   "-l", "/dev/stdin",
										   "-o", session_data.temp_frames[0].generic_output_filename, // TODO: Only do one calculation for generic_output_filename
										   NULL };
	char* waifu2x_result_process_command[] = { "stdbuf", "-oL", "bash", "-c",
											   // These don't have commas, they will be concatanated
											   "sed -u \""
											   "s/[^[:print:]\\n]//g;"
											   "s/.*Step: //g"
											   "\"",
											   NULL	};
	if (errno){
		fprintf(stderr, "prelooperr: %s\n", strerror(errno));
		errno = 0;
	}
	int current_frame = 0;
	size_t last_output_frame_size = 0;
	while(stop_signalled == 0){
		int frame_input_index;
		int frame_output_index;

		for (frame_input_index = 0;
			 frame_input_index < session_data.temp_frame_count;
			 frame_input_index++){
			temp_frame* frame = &session_data.temp_frames[frame_input_index];
			// Read the PNG from ffmpeg
			if (expandable_buffer_read_png_in(&frame->buffer, ffmpeg_source_output) != 0){
				break;
			}
		}
		// This will only trigger if the ffmpeg input connection has been closed and all files have been read
		if (frame_input_index == 0){
			fprintf(stderr, "No more data from ffmpeg, stopping...\n");
			break;
		}
		if (errno){
			fprintf(stderr, "err: %s\n", strerror(errno));
			errno = 0;
		}
		int total_frames_this_round = frame_input_index;
		
		size_t upscale_round;
		for (upscale_round = 0; upscale_round < upscale_rounds; upscale_round++){
			for (frame_input_index = 0; frame_input_index < total_frames_this_round; frame_input_index++){
				temp_frame* frame = &session_data.temp_frames[frame_input_index];
                // Write the buffers' data out
				FILE* output_file = fopen(frame->file->absolute_filename, "wb");
				expandable_buffer_write_to_file(&frame->buffer, output_file);
				fflush(output_file);
				fclose(output_file);
			}
			
			// Wait for waifu2x
			pipe_data waifu2x_input_pipe = create_pipe_data();
			FILE* waifu2x_input_file = fdopen(waifu2x_input_pipe.files.write_to, "wb");
			for (frame_output_index = 0; frame_output_index < total_frames_this_round; frame_output_index++){
				temp_frame* frame = &session_data.temp_frames[frame_output_index];
				
				char* str = frame->file->absolute_filename;
				fwrite(str, sizeof(*str), strlen(str), waifu2x_input_file);
				fputc('\n', waifu2x_input_file);	
			}
			fflush(waifu2x_input_file);
			fclose(waifu2x_input_file);	
			if (errno){
				fprintf(stderr, "prewaif err: %s\n", strerror(errno));
				exit(errno);
				errno = 0;
			}

			pipe_data waifu2x_progress_pipe = create_pipe_data();
			pipe_data waifu2x_crbuffered_progress_pipe = create_pipe_data();
			pipe_data waifu2x_formatted_progress_pipe = create_pipe_data();

			char** waifu2x_command = (upscale_round == 0) ? waifu2x_noise_command : waifu2x_scale_only_command;
			pid_t waifu2x_pid = run_command(waifu2x_command, options.waifu2x_folder, &waifu2x_input_pipe, &waifu2x_progress_pipe, NULL);
			atomic_store(&session_data.waifu2x_process, waifu2x_pid);
			//pipe_data_close_read_from(&waifu2x_input_pipe);

			pid_t waifu2x_carriage_return_pid = fork_to_function(&fix_carriage_return_passthrough, NULL, NULL, &waifu2x_progress_pipe, &waifu2x_crbuffered_progress_pipe, NULL);
			pid_t waifu2x_process_result_pid = run_command(waifu2x_result_process_command, NULL, &waifu2x_crbuffered_progress_pipe, &waifu2x_formatted_progress_pipe, NULL);
			
			// If this is Ctrl-C'd, this program closes because of a bad pipe
			// Soln. handle SIGPIPE like SIGINT etc.
			//waitid(P_PID, waifu2x_pid, NULL, WSTOPPED|WEXITED);
			char* output_line = NULL;
			size_t line_length = 100;
			FILE* waifu2x_formatted_progress_file = fdopen(waifu2x_formatted_progress_pipe.files.read_from, "r");

			char w2_step_string[100] = { '\0' };
			unsigned int w2_upscaled_file_count = 0;
			unsigned int w2_total_file_count = 0;
			static unsigned int encoded_frame_count = 0;
			unsigned int w2_ended = 0;
			while(1){
				int poll_positive = 1;
				while (poll_positive && !w2_ended){
					int poll_result = poll(&(struct pollfd){ .fd = waifu2x_formatted_progress_pipe.files.read_from, .events = POLLIN }, 1, 0);
					switch(poll_result){
					case 1:
						if (getline(&output_line, &line_length, waifu2x_formatted_progress_file) != -1){
							sscanf(output_line, "%s %u/%u", w2_step_string, &w2_upscaled_file_count, &w2_total_file_count);
						}else{
							w2_ended = 1;
						}
						break;
					case -1:
						w2_ended = 1;
						break;
					default:
						poll_positive = 0;
					}
				}
				while (poll(&(struct pollfd){ .fd = ffmpeg_result_framecount_pipe.files.read_from, .events = POLLIN }, 1, 0)==1){
					if (getline(&output_line, &line_length, ffmpeg_result_framecount_file) != -1){
						sscanf(output_line, "%u", &encoded_frame_count);
					}
				}

				fprintf(stderr, "\rCurrent batch has converted %u/%u frames at %s/frame, currently encoded: %u", w2_upscaled_file_count, w2_total_file_count, w2_step_string, encoded_frame_count);
				if (w2_upscaled_file_count == w2_total_file_count && w2_total_file_count != 0) break;
				if (stop_signalled != 0) break;
				if (w2_ended != 0) break;
			}
			fprintf(stderr, "\n");
			
			kill(waifu2x_carriage_return_pid, SIGKILL);
			kill(waifu2x_process_result_pid, SIGKILL);
			//pipe_data_close(&waifu2x_process.input_pipe);
			//fprintf(stderr, "Done waiting\n");
			if (errno){
				fprintf(stderr, "postwaif err: %s\n", strerror(errno));
				exit(errno);
			}
		
			for (frame_output_index = 0; frame_output_index < total_frames_this_round; frame_output_index++){
				if (stop_signalled != 0) break;
				
				temp_frame* frame = &session_data.temp_frames[frame_output_index];
				FILE* output_file = fopen(frame->output_filename, "rb");
				if (output_file == NULL || expandable_buffer_read_png_in(&frame->buffer, output_file) != 0){
					fprintf(stderr, "err: %s\n", strerror(errno));
					fprintf(stderr, "Error reading PNG from %s...\n", frame->output_filename);
					exit(errno || 1);
				}
				fclose(output_file);
			}
			if (errno){
				fprintf(stderr, "postout err: %s\n", strerror(errno));
				exit(errno);
			}
		}

		for (frame_output_index = 0; frame_output_index < total_frames_this_round; frame_output_index++){
			if (stop_signalled != 0) break;
				
			temp_frame* frame = &session_data.temp_frames[frame_output_index];
			expandable_buffer_write_to_pipe(&frame->buffer, ffmpeg_result_input);
		}
		fflush(ffmpeg_result_input);
		
		if (stop_signalled != 0) fprintf(stderr, "got sigint\n");
	}

	if (stop_signalled) exit(0);

	
	kill(ffmpeg_result_progress_pid, SIGKILL);
	
	// Wait for the FFMpeg result process to finish
	fflush(ffmpeg_result_input);
    fclose(ffmpeg_result_input);
	if (waitid(P_PID, ffmpeg_result_pid, NULL, WSTOPPED|WEXITED) != 0){
		fprintf(stderr, "Error waiting for PID %d %s\n", ffmpeg_result_pid, strerror(errno));
	}
	pipe_data_close(&ffmpeg_result_input_pipe);
	//pipe_data_close(&ffmpeg_result_output_pipe);

	/*
	  Close FFMpeg source process
	*/
	// Flush and close input and output pipes
    fflush(ffmpeg_source_output);
    fclose(ffmpeg_source_output);
	// Send SIGINT to the process so ffmpeg can clean up its control characters
	kill(ffmpeg_source_pid, SIGINT);
	waitpid(ffmpeg_source_pid, NULL, 0);
	pipe_data_close(&ffmpeg_source_input_pipe);
	pipe_data_close(&ffmpeg_source_output_pipe);

	cleanup();
}
