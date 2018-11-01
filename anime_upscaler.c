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

#include "expandable_buffer.h"

const size_t initial_buffer_size = 64;

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
	char filename[] = "anime-upscaler-tempfile-XXXXXX";
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

int main(int argc, char* argv[]){
	if (argc < 3){
		fprintf(stderr, "Usage: %s [input_file] [output_file]\n", argv[0]);
		return 1;
	}

	char* input_filepath = argv[1];
	char* output_filepath = argv[2];
	
	expandable_buffer buffer = create_expandable_buffer(initial_buffer_size);
	//expandable_buffer_print(&buffer);

	temp_file* input_frame = create_temp_file("wb");
	temp_file* output_frame = create_temp_file("rb");

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
    //FILE *ffmpeg_source_output = fdopen(ffmpeg_source_output_pipe.files.read_from, "r"); // Open the output as a pipe so we can read it

	/*
	  Set up the ffmpeg result daemon
	*/
	//pipe_data ffmpeg_result_input_pipe = create_pipe_data();
	//pipe_data ffmpeg_result_output_pipe = create_pipe_data();
	char* ffmpeg_result_command[] = { "ffmpeg", "-y",
									  //"-i", input_filepath,
									  "-vcodec", "png", "-f", "image2pipe", "-i", "-",
									  //"-r", "1/1",//"-f", "matroska", //"-c:v", "libx264", //"-c:a", "copy", "-map", "1:v:0", "-map", "0:a:0", 
									  output_filepath,
									  NULL };
	char* echo_stdin_command[] = { "cat" };
	pid_t ffmpeg_result_pid = run_command(ffmpeg_result_command, NULL, &ffmpeg_source_output_pipe, NULL, 0);//&ffmpeg_result_output_pipe);

	//FILE *ffmpeg_result_input = fdopen(ffmpeg_result_input_pipe.files.write_to, "w"); // Open the input as a pipe so we can put images in it
	//pipe_data_close_read_from(&ffmpeg_result_input_pipe); // We shouldn't be able to read from the input
	//pipe_data_close_write_to(&ffmpeg_result_output_pipe); // We shouldn't be able to write to the output
	//dup2(dev_null_write, ffmpeg_result_output_pipe.files.read_from); // Send the output to /dev/null because we don't care about it


    // waifu2x doesn't like using stdout
	char* waifu2x_command[] = { "th", "./waifu2x.lua", "-m", "scale",  "-i", input_frame->absolute_filename, "-o", output_frame->absolute_filename, NULL };
	char* waifu2x_location = "./waifu2x/";

	/*int current_frame = 0;
	size_t last_output_frame_size = 0;
	while(1){
		// Read PNG
		//fprintf(stderr, "Reading frame %d from input \n", current_frame);
		if (expandable_buffer_read_png_in(&buffer, ffmpeg_source_output) != 0) break;

		expandable_buffer_write_to_file(&buffer, input_frame->file);
		// Push the changes to the file
		fflush(input_frame->file);

		// TODO: If this is enabled the same frame is sent to ffmpeg each time
		if (0){
			// Wait for waifu2x
			//pipe_data waifu2x_output_pipe = create_pipe_data();
			
			pid_t waifu2x_process_pid = run_command(waifu2x_command, waifu2x_location, NULL, NULL, 1);
			//pipe_data_close_write_to(&waifu2x_output_pipe); // We shouldn't be able to write to the output
			//dup2(dev_null_write, waifu2x_output_pipe.files.read_from); // Send the output to /dev/null because we don't care about it
			waitpid(waifu2x_process_pid, NULL, 0);

			//pipe_data_close(&waifu2x_output_pipe);

			//fprintf(stderr, "Reading frame %d from output\n", current_frame++);
			//clearerr(output_frame->file);
			//sleep(1);
			reopen_temp_file(output_frame, "rb");
			expandable_buffer_read_png_in(&buffer, output_frame->file);
		}

		
		if (buffer.pointer[buffer.size-1])fprintf(stderr, "End of output file: %d\n", buffer.pointer[buffer.size-1]);
		expandable_buffer_write_to_pipe(&buffer, ffmpeg_result_input);
		last_output_frame_size = buffer.size;
		fflush(stderr);
		//break;
	}
	fprintf(stderr, "\n");
	fflush(stderr);*/


	// Send SIGINT to the process so ffmpeg can clean up its control characters
	//kill(ffmpeg_result_pid, sadsdwdas);
	//fflush(ffmpeg_result_input);
    //fclose(ffmpeg_result_input);
	//pipe_data_close(&ffmpeg_result_input_pipe);
	if (waitid(P_PID, ffmpeg_result_pid, NULL, WSTOPPED|WEXITED) != 0){
		fprintf(stderr, "Error waiting for PID %d %s\n", ffmpeg_result_pid, strerror(errno));
	}
	
	free_expandable_buffer(&buffer);
	// Flush and close input and output pipes
    //fflush(ffmpeg_source_output);
    //fclose(ffmpeg_source_output);
	
	// Send SIGINT to the process so ffmpeg can clean up its control characters
	kill(ffmpeg_source_pid, SIGINT);
	waitpid(ffmpeg_source_pid, NULL, 0);
	pipe_data_close(&ffmpeg_source_input_pipe);
	pipe_data_close(&ffmpeg_source_output_pipe);


	//pipe_data_close(&ffmpeg_result_output_pipe);

	free_temp_file(&input_frame);
	free_temp_file(&output_frame);
}
