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

#include "expandable_buffer.h"
#include "process_utils.h"
#include "temp_files.h"

typedef struct {
	int pid;
	pipe_data input_pipe;
} waifu2x_process_data;

static volatile sig_atomic_t stop_signalled = 0;
static volatile sig_atomic_t ffmpeg_src_stopped = 0;

static struct {
	union {
		volatile _Atomic pid_t processes[3];
		struct {
			volatile _Atomic pid_t ffmpeg_src_process;
		   	volatile _Atomic pid_t ffmpeg_rst_process;
			volatile _Atomic pid_t waifu2x_process;
		};
	};
	temp_frame* temp_frames;
	size_t temp_frame_count;
} session_data = { .processes={0}, .temp_frames=NULL, .temp_frame_count=0};
void stop_program_from_signal(int sig){
	if (sig == SIGCHLD){
		int exit_status = 0;
		pid_t exited_pid = wait(&exit_status);
		if (atomic_load(&session_data.ffmpeg_src_process) == exited_pid)
			ffmpeg_src_stopped = 1;
		// Ignore SIGCHLD if it's a clean exit
		if (WIFEXITED(exit_status)) return;
	}
	if (stop_signalled) return;
	
	if (sig == SIGINT) signal(SIGINT, SIG_IGN);
	
	int i;
	for (i = 0; i < 3; i++){
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
#define FRAMES_PER_UPSCALE_ROUND 256

void get_framerate(char* filepath, char* output, size_t output_length){
	pipe_data ffmpeg_framerate_output_pipe = create_pipe_data();
	const char* ffmpeg_framerate_command_format = "ffmpeg -i %s 2>&1 | sed -n \"s/.*, \\(.*\\) fps.*/\\1/p\"";
	const size_t ffmpeg_framerate_command_length = strlen(ffmpeg_framerate_command_format) + strlen(filepath) + 1;
	char* ffmpeg_framerate_command = calloc(ffmpeg_framerate_command_length, sizeof(char));
	snprintf(ffmpeg_framerate_command, ffmpeg_framerate_command_length, ffmpeg_framerate_command_format, filepath);
	char* ffmpeg_framerate_invocation_command[] = { "bash", "-c", ffmpeg_framerate_command, NULL };
	pid_t ffmpeg_framerate_pid = run_command(ffmpeg_framerate_invocation_command, NULL, NULL, &ffmpeg_framerate_output_pipe, 0);
	pipe_data_close_write_to(&ffmpeg_framerate_output_pipe);
	FILE* ffmpeg_framerate_output = fdopen(ffmpeg_framerate_output_pipe.files.read_from, "r");

	fread(output, sizeof(char), output_length, ffmpeg_framerate_output);
	
	fclose(ffmpeg_framerate_output);
	pipe_data_close(&ffmpeg_framerate_output_pipe);
	free(ffmpeg_framerate_command);
}

int main(int argc, char* argv[]){
	if (argc < 3){
		fprintf(stderr, "Usage: %s [input_file] [output_file]\n", argv[0]);
		return 1;
	}

	char* input_filepath = argv[1];
	char* output_filepath = argv[2];

	/* 
	   Get the framerate of the video
	*/
	char framerate_str[10] = { '\0' };
	get_framerate(input_filepath, framerate_str, 10);
	fprintf(stdout, "Framerate: %s\n", framerate_str);
	if (strlen(framerate_str) == 0) exit(1);

	int dev_null_read = open("/dev/null", O_RDONLY);
	int dev_null_write = open("/dev/null", O_WRONLY);

	/*
	  Set up the ffmpeg source daemon
	*/
	pipe_data ffmpeg_source_input_pipe = create_pipe_data();
	pipe_data ffmpeg_source_output_pipe = create_pipe_data();
	char* ffmpeg_source_command[] = { "ffmpeg", "-y",
									  "-i", input_filepath,
									  "-vf", "fps=30",
									  //"-ss", "00:00:01",
									  "-vcodec", "png", "-f", "image2pipe", "-",
									  NULL };
	pid_t ffmpeg_source_pid = run_command(ffmpeg_source_command, NULL, &ffmpeg_source_input_pipe, &ffmpeg_source_output_pipe, 0);
	atomic_store(&session_data.ffmpeg_src_process, ffmpeg_source_pid);
	
	dup2(dev_null_read, ffmpeg_source_input_pipe.files.write_to); // Send /dev/null to the input so that it doesn't use the terminal
	pipe_data_close_read_from(&ffmpeg_source_input_pipe); // We shouldn't be able to read from the input
	pipe_data_close_write_to(&ffmpeg_source_output_pipe); // We shouldn't be able to write to the output
    FILE *ffmpeg_source_output = fdopen(ffmpeg_source_output_pipe.files.read_from, "r"); // Open the output as a pipe so we can read it

	/*
	  Set up the ffmpeg result daemon
	*/
	pipe_data ffmpeg_result_input_pipe = create_pipe_data();
	//pipe_data ffmpeg_result_output_pipe = create_pipe_data();
	char* ffmpeg_result_command[] = { "ffmpeg", "-y",
									  "-i", input_filepath,
									  //"-vcodec", "copy",
									  //"-vsync", "drop",
									  "-r", "30",
									  "-vcodec", "png", "-f", "image2pipe", "-i", "-",
									  /*"-c:a", "copy",*/ "-map", "1:v:0", "-map", "0:a:0", 
									  output_filepath,
									  NULL };
	pid_t ffmpeg_result_pid = run_command(ffmpeg_result_command, NULL, &ffmpeg_result_input_pipe, NULL, 1);//&ffmpeg_result_output_pipe);
	atomic_store(&session_data.ffmpeg_rst_process, ffmpeg_result_pid);
	
	FILE *ffmpeg_result_input = fdopen(ffmpeg_result_input_pipe.files.write_to, "w"); // Open the input as a pipe so we can put images in it
	pipe_data_close_read_from(&ffmpeg_result_input_pipe); // We shouldn't be able to read from the input
	//pipe_data_close_write_to(&ffmpeg_result_output_pipe); // We shouldn't be able to write to the output
	//dup2(dev_null_write, ffmpeg_result_output_pipe.files.read_from); // Send the output to /dev/null because we don't care about it

	// Set upu the interrupt handles
	
	struct sigaction sigint_action;
	sigint_action.sa_handler = stop_program_from_signal;
	sigaction(SIGINT, &sigint_action, NULL);
	sigaction(SIGPIPE, &sigint_action, NULL);
	sigaction(SIGCHLD, &sigint_action, NULL);
	//sigaction(SIGQUIT, &sigint_action, NULL);
	
	// Set up the temp files AFTER the interrupt handler is set
	// If someone Ctrl-C's before this, we'll die right away
	// If someone Ctrl-C's after this, we'll get rid of the tempfiles properly
	session_data.temp_frame_count = FRAMES_PER_UPSCALE_ROUND;
	session_data.temp_frames = calloc(session_data.temp_frame_count, sizeof(temp_frame));
	{
		int i;
		for (i = 0; i < session_data.temp_frame_count; i++) session_data.temp_frames[i] = create_temp_frame();
	}
	atexit(cleanup);
	
    // waifu2x doesn't like using stdout
	char* waifu2x_command[] = { "th", "./waifu2x.lua",
								"-force_cudnn", "1",
								"-m", "noise_scale", "-noise_level", "1",
								"-l", "/dev/stdin",
								"-o", session_data.temp_frames[0].generic_output_filename, // TODO: Only do one calculation for generic_output_filename
								NULL };
	const size_t waifu2x_command_output_file_index = 7; 
	char* waifu2x_location = "./waifu2x/";
	waifu2x_process_data waifu2x_process;
	
	int current_frame = 0;
	size_t last_output_frame_size = 0;
	while(stop_signalled == 0){
		int frame_input_index = 0;
		for (frame_input_index = 0;
			 frame_input_index < session_data.temp_frame_count;
			 frame_input_index++){
			temp_frame* frame = &session_data.temp_frames[frame_input_index];
			// Read the PNG from ffmpeg
			if (expandable_buffer_read_png_in(&frame->buffer, ffmpeg_source_output) != 0){
				break;
			}
			// Write it out 
			expandable_buffer_write_to_file(&frame->buffer, frame->file->file);
			fflush(frame->file->file);
		}
		// This will only trigger if the ffmpeg input connection has been closed and all files have been read
		if (frame_input_index == 0){
			fprintf(stderr, "No more data from ffmpeg, stopping...\n");
			break;
		}
		
		int total_frames_this_round = frame_input_index;

		// Wait for waifu2x
		waifu2x_process.input_pipe = create_pipe_data();
		FILE* waifu2x_input_file = fdopen(waifu2x_process.input_pipe.files.write_to, "wb");
		
		waifu2x_process.pid = run_command(waifu2x_command, waifu2x_location, &waifu2x_process.input_pipe, NULL, 0);
		atomic_store(&session_data.waifu2x_process, waifu2x_process.pid);
		pipe_data_close_read_from(&waifu2x_process.input_pipe);
		
		int frame_output_index;
		for (frame_output_index = 0; frame_output_index < total_frames_this_round; frame_output_index++){
			temp_frame* frame = &session_data.temp_frames[frame_output_index];
			
			char* str = frame->file->absolute_filename;
			fwrite(str, sizeof(*str), strlen(str), waifu2x_input_file);
			fputc('\n', waifu2x_input_file);	
		}
		fflush(waifu2x_input_file);
		fclose(waifu2x_input_file);	

		// If this is Ctrl-C'd, this program closes because of a bad pipe
		// Soln. handle SIGPIPE like SIGINT etc.
		waitid(P_PID, waifu2x_process.pid, NULL, WSTOPPED|WEXITED);
		pipe_data_close(&waifu2x_process.input_pipe);
		//fprintf(stderr, "Done waiting\n");
		
		for (frame_output_index = 0; frame_output_index < total_frames_this_round; frame_output_index++){
			if (stop_signalled != 0) break;
			
			temp_frame* frame = &session_data.temp_frames[frame_output_index];
			FILE* output_file = fopen(frame->output_filename, "rb");
			if (expandable_buffer_read_png_in(&frame->buffer, output_file) == 0){
				expandable_buffer_write_to_pipe(&frame->buffer, ffmpeg_result_input);
			}else{
				fprintf(stderr, "Error reading PNG from %s...\n", frame->output_filename);
			}
			fclose(output_file);
		}
		//fprintf(stderr, "looping bvack\n");

		if (stop_signalled != 0) fprintf(stderr, "got sigint\n");
	}

	if (stop_signalled) exit(0);
	
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
