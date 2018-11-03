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

#include "expandable_buffer.h"

typedef union {
	int array[2];
	struct {
		int read_from; // read from
		int write_to; // write to
	} files;
} pipe_data;

pipe_data create_pipe_data(){
	pipe_data data;
	pipe(data.array);
	return data;
}
void pipe_data_close(pipe_data* pipe){
	if (pipe->array[0] != -1) close(pipe->array[0]);
	if (pipe->array[1] != -1) close(pipe->array[1]);

	pipe->array[0] = -1;
	pipe->array[1] = -1;
}
void pipe_data_close_read_from(pipe_data* pipe){
	close(pipe->files.read_from);
	pipe->files.read_from = -1;
}
void pipe_data_close_write_to(pipe_data* pipe){
	close(pipe->files.write_to);
	pipe->files.write_to = -1;
}

int run_command(char* const command[], char* working_directory, pipe_data* input_pipe, pipe_data* output_pipe, int mute_stderr){
	pid_t process_id = fork();
	switch(process_id){
	case -1:
		fprintf(stderr, "fork() failed\n");
		exit(1);
	case 0:
		// We are the new process, start the command
		if (input_pipe != NULL){
			// Redirect stdin
			if (dup2(input_pipe->files.read_from, 0) < 0){
				pipe_data_close_read_from(input_pipe);
				fprintf(stderr, "Failed to redirect input for command %s\n", command[0]);
				exit(1);
			}
			pipe_data_close_write_to(input_pipe); // We don't need this anymore
		}
		if (output_pipe != NULL){
			// Redirect stdout
			if (dup2(output_pipe->files.write_to, 1) < 0){
				pipe_data_close_write_to(output_pipe);
				fprintf(stderr, "Failed to redirect output for command %s\n", command[0]);
				exit(1);
			}
			pipe_data_close_read_from(output_pipe); // We don't need this anymore
		}
		// Redirect stderr to /dev/null
		if (mute_stderr != 0) freopen("/dev/null", "w", stderr);
		if (working_directory != NULL){
			chdir(working_directory);
		}
		// Run ffmpeg
		execvp(command[0], command);
		// Handle ffmpeg failing
		// TODO: This won't display (i think)
		fprintf(stderr, "Failed to exec() command %s with err %s\n", command[0], strerror(errno));
		exit(1);
	}
	return process_id;
}

typedef struct {
	char absolute_filename[PATH_MAX];
	FILE* file;
} temp_file;
temp_file* create_temp_file(char* access_type){
	char filename[] = "./tmp/anime-upscaler-tempfile-XXXXXX";
	int file_descriptor = mkstemp(filename);
	if (file_descriptor == -1){
		fprintf(stderr, "Failed to create temp file\n");
		exit(1);
	}

	temp_file* tfile = malloc(sizeof(temp_file));
	
	realpath(filename, tfile->absolute_filename);
	tfile->file = fdopen(file_descriptor, access_type);
	return tfile;
}
void free_temp_file(temp_file** tfile_pointer){
	// The temp file will automatically be destroyed once unlinked
	unlink((*tfile_pointer)->absolute_filename);
	fclose((*tfile_pointer)->file);
	free(*tfile_pointer);
	*tfile_pointer = NULL;
}
void reopen_temp_file(temp_file* tfile, char* access_type){
	tfile->file = freopen(tfile->absolute_filename, access_type, tfile->file);
}

// Use waifu2x to upscale 16 images per round
#define FRAMES_PER_UPSCALE_ROUND 128
#define INITIAL_FRAME_BUFFER_SIZE 64

typedef struct {
	expandable_buffer buffer;
	temp_file* file;
	char* generic_output_filename; // The absolute path with the basename replaced with %s
	char* output_filename;
} temp_frame;
temp_frame create_temp_frame(){
	temp_frame frame;
	frame.buffer = create_expandable_buffer(INITIAL_FRAME_BUFFER_SIZE);
	frame.file = create_temp_file("wb+");

	const char* new_basename = "/%s_output.png";

	char* absolute_filename_dup = strdup(frame.file->absolute_filename);
	const char* absolute_filename_dirname = dirname(absolute_filename_dup);
	const size_t dirname_length = strlen(absolute_filename_dirname);
	// TODO: Is it an issue if total_length > PATH_MAX?
	const size_t total_length = dirname_length + strlen(new_basename) + 1;
	frame.generic_output_filename = calloc(total_length, sizeof(char));
	strcpy(frame.generic_output_filename, absolute_filename_dirname);
	strcpy(frame.generic_output_filename + dirname_length, new_basename);
	free(absolute_filename_dup);

	absolute_filename_dup = strdup(frame.file->absolute_filename);
	const char* absolute_filename_basename = basename(absolute_filename_dup);
	frame.output_filename = calloc(PATH_MAX, sizeof(char));
	snprintf(frame.output_filename, PATH_MAX, frame.generic_output_filename, absolute_filename_basename);
	free(absolute_filename_dup);

	// Open and close the file to make sure it exists
	fclose(fopen(frame.output_filename, "wb"));
	
	fprintf(stdout, "temp file output name is: %s\n", frame.output_filename);
	return frame;
}
void free_temp_frame(temp_frame* frame){
	free_temp_file(&frame->file);
	free(frame->generic_output_filename);
	unlink(frame->output_filename);
	free(frame->output_filename);
}

typedef struct {
	int pid;
	pipe_data input_pipe;
} waifu2x_process_data;

static volatile sig_atomic_t should_stop = 0;

void SIGINT_handler(int sig){
	signal(sig, SIG_IGN);
	should_stop = 1;
}

int main(int argc, char* argv[]){
	if (argc < 3){
		fprintf(stderr, "Usage: %s [input_file] [output_file]\n", argv[0]);
		return 1;
	}

	char* input_filepath = argv[1];
	char* output_filepath = argv[2];

	int dev_null_read = open("/dev/null", O_RDONLY);
	int dev_null_write = open("/dev/null", O_WRONLY);

	/*
	  Set up the ffmpeg source daemon
	*/
	pipe_data ffmpeg_source_input_pipe = create_pipe_data();
	pipe_data ffmpeg_source_output_pipe = create_pipe_data();
	char* ffmpeg_source_command[] = { "ffmpeg", "-y",
									  "-i", input_filepath,
									  //"-ss", "00:00:01",
									  "-vcodec", "png", "-f", "image2pipe", "-",
									  NULL };
	pid_t ffmpeg_source_pid = run_command(ffmpeg_source_command, NULL, &ffmpeg_source_input_pipe, &ffmpeg_source_output_pipe, 1);

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
									  //"-vsync", "drop",
									  //"-r", "25",
									  "-vcodec", "png", "-f", "image2pipe", "-i", "-",
									  "-c:a", "copy", "-map", "1:v:0", "-map", "0:a:0", 
									  output_filepath,
									  NULL };
	char* echo_stdin_command[] = { "cat" };
	pid_t ffmpeg_result_pid = run_command(ffmpeg_result_command, NULL, &ffmpeg_result_input_pipe, NULL, 1);//&ffmpeg_result_output_pipe);

	FILE *ffmpeg_result_input = fdopen(ffmpeg_result_input_pipe.files.write_to, "w"); // Open the input as a pipe so we can put images in it
	pipe_data_close_read_from(&ffmpeg_result_input_pipe); // We shouldn't be able to read from the input
	//pipe_data_close_write_to(&ffmpeg_result_output_pipe); // We shouldn't be able to write to the output
	//dup2(dev_null_write, ffmpeg_result_output_pipe.files.read_from); // Send the output to /dev/null because we don't care about it

	// Set upu the interrupt handles
	struct sigaction sigint_action;
	sigint_action.sa_handler = SIGINT_handler;
	sigaction(SIGINT, &sigint_action, NULL);
	sigaction(SIGQUIT, &sigint_action, NULL);
	
	// Set up the temp files AFTER the interrupt handler is set
	// If someone Ctrl-C's before this, we'll die right away
	// If someone Ctrl-C's after this, we'll get rid of the tempfiles properly
	temp_frame temp_frames[FRAMES_PER_UPSCALE_ROUND];
	{
		int i;
		for (i = 0; i < FRAMES_PER_UPSCALE_ROUND; i++) temp_frames[i] = create_temp_frame();
	}
	
    // waifu2x doesn't like using stdout
	// TODO: Forcing cudnn makes it fail
	char* waifu2x_command[] = { "th", "./waifu2x.lua",
								"-m", "scale",
								"-l", "/dev/stdin",
								"-o", temp_frames[0].generic_output_filename, // TODO: Only do one calculation for generic_output_filename
								NULL };
	const size_t waifu2x_command_output_file_index = 7; 
	char* waifu2x_location = "./waifu2x/";
	waifu2x_process_data waifu2x_process;
	
	int current_frame = 0;
	size_t last_output_frame_size = 0;
	while(should_stop == 0){
		int frameInputIndex;
		for (frameInputIndex = 0; frameInputIndex < FRAMES_PER_UPSCALE_ROUND; frameInputIndex++){
			temp_frame* frame = &temp_frames[frameInputIndex];
			// Read the PNG from ffmpeg
			// TODO: Suppress error when reading from an empty pipe
			if (expandable_buffer_read_png_in(&frame->buffer, ffmpeg_source_output) != 0)
				break;
			// Write it out 
			expandable_buffer_write_to_file(&frame->buffer, frame->file->file);
			fflush(frame->file->file);
		}
		if (frameInputIndex == 0) break; // If no PNG files were read, then stop

		int total_frames_this_round = frameInputIndex;

		// Wait for waifu2x
		waifu2x_process.input_pipe = create_pipe_data();
		FILE* waifu2x_input_file = fdopen(waifu2x_process.input_pipe.files.write_to, "wb");
			
		waifu2x_process.pid = run_command(waifu2x_command, waifu2x_location, &waifu2x_process.input_pipe, NULL, 0);
		pipe_data_close_read_from(&waifu2x_process.input_pipe);
		
		int frame_output_index;
		for (frame_output_index = 0; frame_output_index < total_frames_this_round; frame_output_index++){
			temp_frame* frame = &temp_frames[frame_output_index];
			
			char* str = frame->file->absolute_filename;
			fwrite(str, sizeof(*str), strlen(str), waifu2x_input_file);
			fputc('\n', waifu2x_input_file);	
		}
		fflush(waifu2x_input_file);
		fclose(waifu2x_input_file);	
		
		waitid(P_PID, waifu2x_process.pid, NULL, WSTOPPED|WEXITED);
		pipe_data_close(&waifu2x_process.input_pipe);
		
		for (frame_output_index = 0; frame_output_index < total_frames_this_round; frame_output_index++){
			temp_frame* frame = &temp_frames[frame_output_index];
			FILE* output_file = fopen(frame->output_filename, "rb");
			if (expandable_buffer_read_png_in(&frame->buffer, output_file) == 0){
				expandable_buffer_write_to_pipe(&frame->buffer, ffmpeg_result_input);
			}else{
				fprintf(stderr, "Error reading PNG from %s...\n", frame->output_filename);
			}
			fclose(output_file);
		}

		if (should_stop != 0) fprintf(stderr, "got sigint\n");
	}

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

	{
		int i;
		for (i = 0; i < FRAMES_PER_UPSCALE_ROUND; i++) free_temp_frame(&temp_frames[i]);
	}
}
