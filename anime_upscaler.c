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

typedef unsigned char BYTE;


const size_t initial_buffer_size = 64;

typedef struct {
	size_t capacity;
	size_t size;
	BYTE* pointer;
} expandable_buffer;

expandable_buffer create_expandable_buffer(size_t starting_capacity){
	expandable_buffer buffer = {
		.capacity = starting_capacity,
		.size = 0,
		.pointer = (BYTE*)malloc(sizeof(BYTE) * starting_capacity)
	};
	return buffer;
}
void free_expandable_buffer(expandable_buffer* buffer){
	free(buffer->pointer);
	buffer->size = 0;
	buffer->capacity = 0;
	buffer->pointer = NULL;
}

void expandable_buffer_clear(expandable_buffer* buffer){
	buffer->size = 0; // No need to empty
}

// Increases the capacity and size of the buffer,
// returns the old end of the buffer
BYTE* expandable_buffer_increase_size(expandable_buffer* buffer, size_t size_delta){
	assert(buffer);

	size_t old_size = buffer->size;
	
	buffer->size += size_delta;
	if (buffer->size > buffer->capacity){
		buffer->capacity = buffer->size - (buffer->size % 8) + 8;
		assert(buffer->capacity >= buffer->size);
		buffer->pointer = (BYTE*)realloc(buffer->pointer, buffer->capacity);
	}

	return buffer->pointer + old_size;
}

// Reads data from a FILE* and appends to buffer
int expandable_buffer_read_data_in(expandable_buffer* buffer, FILE* in, size_t size){	
	BYTE* data_start = expandable_buffer_increase_size(buffer, size);
	return fread(data_start, 1, size, in);
}

// Reads one PNG data from the file to the buffer
// returns 1 if unsuccessful
int expandable_buffer_read_png_in(expandable_buffer* buffer, FILE* in){
	const size_t HEADER_SIZE = 8;
	const char* HEADER = "\211PNG\r\n\032\n";

	const size_t CHUNK_LENGTH_SIZE = 4;
	const size_t CHUNK_NAME_SIZE = 4;
	const size_t CHUNK_CRC_SIZE = 4;
	const char* END_BLOCK_NAME = "IEND";
	const size_t END_BLOCK_NAME_LENGTH = 4;

	expandable_buffer_clear(buffer);
	
	// Read header
	if (expandable_buffer_read_data_in(buffer, in, HEADER_SIZE) != HEADER_SIZE){
		fprintf(stderr, "Problem reading header\n");
		return 1;
	}
	//expandable_buffer_print(buffer);
	if (memcmp(HEADER, buffer->pointer, HEADER_SIZE) != 0){
		buffer->pointer[buffer->size - 1] = '\0';
		fprintf(stderr, "Header was different, was %s\n", buffer->pointer);
		return 1;
	}
	//expandable_buffer_print(buffer);

	char chunk_name[CHUNK_NAME_SIZE + 1] = {'\0'};
	do {
		if (expandable_buffer_read_data_in(buffer, in, CHUNK_LENGTH_SIZE) != CHUNK_LENGTH_SIZE){
			fprintf(stderr, "problem reading chunk length\n");
			return 1;
		}
		//expandable_buffer_print(&buffer);
		
		// Read the size of the chunk data in (Big Endian)
		const int CHUNK_DATA_SIZE = buffer->pointer[buffer->size - 1]
			+ (buffer->pointer[buffer->size - 2] << 8)
			+ (buffer->pointer[buffer->size - 3] << 16)
			+ (buffer->pointer[buffer->size - 4] << 24);
		//fprintf(stdout, "Chonk Data Size: %d\n", CHUNK_DATA_SIZE);

		if (expandable_buffer_read_data_in(buffer, in, CHUNK_NAME_SIZE) != CHUNK_NAME_SIZE){
			fprintf(stderr, "problem reading chunk name\n");
			return 1;
		}
		strncpy(chunk_name, (char*)buffer->pointer + (buffer->size - CHUNK_NAME_SIZE), CHUNK_NAME_SIZE);

		if (CHUNK_DATA_SIZE != 0 && expandable_buffer_read_data_in(buffer, in, CHUNK_DATA_SIZE) != CHUNK_DATA_SIZE){
			fprintf(stderr, "problem reading chunk data\n");
			return 1;
		}

		if (expandable_buffer_read_data_in(buffer, in, CHUNK_CRC_SIZE) != CHUNK_CRC_SIZE){
			fprintf(stderr, "problem reading chunk crc\n");
			return 1;
		}
	} while(strncmp(chunk_name, END_BLOCK_NAME, END_BLOCK_NAME_LENGTH) != 0);

	return 0;
}

int expandable_buffer_write_to_file(expandable_buffer* buffer, FILE* out){
	if (ftruncate(fileno(out), buffer->size) != 0){
		fprintf(stderr, "Error truncation the output file\n");
		return 1;
	}

	fwrite(buffer->pointer, sizeof(buffer->pointer[0]), buffer->size, out);
	
	return 0;
}

void expandable_buffer_print(expandable_buffer* buffer){
	fprintf(stdout, "Buffer Pointer: %p\nBuffer Size: %zu\nBuffer Capacity: %zu\n", buffer->pointer, buffer->size, buffer->capacity);

	fprintf(stdout, "Buffer Data: ");
	char number[3] = {'\0'};
	int i = 0;
	for (i = 0; i < buffer->size && i < 12; i++){
		sprintf(number, "%02x", buffer->pointer[i]);
		fprintf(stdout, "%s ", number);
	}
	fprintf(stdout, "\n");
}

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

int run_command(pipe_data* pipe, char* const command[], char* working_directory){
	int process_id = fork();
	switch(process_id){
	case -1:
		fprintf(stderr, "fork() failed\n");
		exit(1);
	case 0:
		
		// We are the new process, start the command
		if (pipe != NULL){
			// Redirect stdout
			if (dup2(pipe->files.write_to, 1) < 0){
				pipe_data_close_write_to(pipe);
				fprintf(stderr, "Failed to redirect output for command\n");
				exit(1);
			}
			pipe_data_close_read_from(pipe); // We don't need this anymore
		}
		// Redirect stderr to /dev/null
		//freopen("/dev/null", "w", stderr);
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

int main(int argc, char* argv[]){
	/*if (argc == 1){
	  fprintf(stderr, "Not enough arguments!\n");
	  return 1;
	  }*/
	
	expandable_buffer buffer = create_expandable_buffer(initial_buffer_size);
	//expandable_buffer_print(&buffer);


	char frame_filename[] = "anime-upscaler-tempfile-XXXXXX.png";
	/*int frame_file_descriptor = mkstemp(frame_filename);
	if (frame_file_descriptor == -1){
		fprintf(stderr, "Failed to create temp file\n");
		return 1;
		}*/
	FILE* frame_file = fopen(frame_filename, "wb");//fdopen(frame_file_descriptor, "wb");

	FILE* output_file = fopen("./test.png", "wb");

	// Create a pipe to use with the ffmpeg input process
	pipe_data pipe_from_source = create_pipe_data();

    // Create a new process for the ffmpeg image splitting
	char* ffmpeg_command[] = { "ffmpeg", "-y", "-i", "small.mp4", "-vcodec", "png", "-f", "image2pipe", "-", NULL };

	int ffmpeg_input_process_pid = run_command(&pipe_from_source, ffmpeg_command, NULL);
	pipe_data_close_write_to(&pipe_from_source);
	
	// Open the ffmpeg pipe output as a file
    FILE *ffmpeg_pipein = fdopen(pipe_from_source.files.read_from, "r");

	// NOTE: For waifu2x file output, use -o /dev/stdout
	char actual_frame_path[PATH_MAX + 1];
	realpath(frame_filename, actual_frame_path);
	char* waifu2x_command[] = { "th", "./waifu2x.lua", "-m", "scale",  "-i", actual_frame_path, "-o", "/dev/stdout", NULL };
	char* waifu2x_location = "./waifu2x/";
	
	while(1){
		// Read PNG
		if (expandable_buffer_read_png_in(&buffer, ffmpeg_pipein) != 0) break;
			
		fprintf(stdout, "Read a PNG file of size %zu, buffer capacity is %zu\n", buffer.size, buffer.capacity);

		expandable_buffer_write_to_file(&buffer, frame_file);
		fflush(frame_file);

		{
			// Start waifu2x
			pipe_data pipe_from_waifu2x = create_pipe_data();
			int waifu2x_process_pid = run_command(&pipe_from_waifu2x, waifu2x_command, waifu2x_location);
			pipe_data_close_write_to(&pipe_from_waifu2x);

			fprintf(stdout, "WAIFU PID %d\n", waifu2x_process_pid);
			waitpid(waifu2x_process_pid, NULL, 0);

			FILE *waifu2x_pipein = fdopen(pipe_from_source.files.read_from, "r");

			// Read the png in
			expandable_buffer_clear(&buffer);
			if (expandable_buffer_read_png_in(&buffer, waifu2x_pipein) != 0) break;
			fprintf(stdout, "Read a PNG file from waifu2x of size %zu, buffer capacity is %zu\n", buffer.size, buffer.capacity);
			expandable_buffer_write_to_file(&buffer, output_file);
			if (expandable_buffer_read_png_in(&buffer, waifu2x_pipein) != 0){
				fprintf(stderr, "Failed to read another png in from waifu2x\n");
			}
						fprintf(stdout, "Read a PNG file from waifu2x of size %zu, buffer capacity is %zu\n", buffer.size, buffer.capacity);

			if (expandable_buffer_read_png_in(&buffer, waifu2x_pipein) != 0){
				fprintf(stderr, "Failed to read another png in from waifu2x\n");
			}
						fprintf(stdout, "Read a PNG file from waifu2x of size %zu, buffer capacity is %zu\n", buffer.size, buffer.capacity);

			// Stop waifu2x once done
			kill(waifu2x_process_pid, SIGINT);
			pipe_data_close(&pipe_from_waifu2x);
		}

			
		break;
	}

	fclose(output_file);

	free_expandable_buffer(&buffer);
	// Flush and close input and output pipes
    fflush(ffmpeg_pipein);
    fclose(ffmpeg_pipein);
	// Send SIGINT to the process so ffmpeg can clean up its control characters
	kill(ffmpeg_input_process_pid, SIGINT);
	pipe_data_close(&pipe_from_source);
    //fflush(pipeout);
    //pclose(pipeout);
	// The temp file will automatically be destroyed once unlinked
	//unlink(frame_filename);
	fclose(frame_file);
}
