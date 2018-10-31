#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

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

int main(int argc, char* argv[]){
	/*if (argc == 1){
		fprintf(stderr, "Not enough arguments!\n");
		return 1;
		}*/
	
	expandable_buffer buffer = create_expandable_buffer(initial_buffer_size);
	//expandable_buffer_print(&buffer);


	FILE* frameFile = tmpfile();
	
	// Open an input pipe from ffmpeg and an output pipe to a second instance of ffmpeg
    FILE *pipein = popen("ffmpeg -i small.mp4 -vcodec png -f image2pipe - 2> /dev/null", "r");

	// NOTE: For waifu2x file output, use -o /dev/stdout
	
    //FILE *pipeout = popen("ffmpeg -y -f image2pipe -vcodec png -i - -f mp4 -q:v 5 -an -vcodec mpeg4 output.mp4", "w");
     
	while(1){
		// Read PNG
		{
			expandable_buffer_clear(&buffer);
			
			// Read header
			const size_t headerSize = 8;
			const char* header = "\211PNG\r\n\032\n";
			if (expandable_buffer_read_data_in(&buffer, pipein, headerSize) != headerSize){
				fprintf(stderr, "Problem reading header\n");
				break;
			}
			//expandable_buffer_print(&buffer);
			
			if (memcmp(header, buffer.pointer, headerSize) != 0){
				buffer.pointer[8] = '\0';
				fprintf(stderr, "Header was different, was %s\n", buffer.pointer);
				break;
			}
			//expandable_buffer_print(&buffer);

			char chunkName[5] = {'\0'};
			do {
				const size_t lengthSize = 4;
				if (expandable_buffer_read_data_in(&buffer, pipein, 4) != lengthSize){
					fprintf(stderr, "problem reading chunk length\n");
					break;
				}
				//expandable_buffer_print(&buffer);
				int dataSize = buffer.pointer[buffer.size - 1]
					+ (buffer.pointer[buffer.size - 2] << 8)
					+ (buffer.pointer[buffer.size - 3] << 16)
					+ (buffer.pointer[buffer.size - 4] << 24);
				//fprintf(stdout, "Chonk Data Size: %d\n", dataSize);

				const size_t nameSize = 4;
				if (expandable_buffer_read_data_in(&buffer, pipein, 4) != nameSize){
					fprintf(stderr, "problem reading chunk name\n");
					break;
				}
				strncpy(chunkName, (char*)buffer.pointer + (buffer.size - nameSize), nameSize);

				if (dataSize != 0 && expandable_buffer_read_data_in(&buffer, pipein, dataSize) != dataSize){
					fprintf(stderr, "problem reading chunk data\n");
					break;
				}

				const size_t crcSize = 4;
				if (expandable_buffer_read_data_in(&buffer, pipein, crcSize) != crcSize){
					fprintf(stderr, "problem reading chunk crc\n");
					break;
				}
			} while(strncmp(chunkName, "IEND", 4) != 0);
		}

		fprintf(stdout, "Read a PNG file of size %zu, buffer capacity is %zu\n", buffer.size, buffer.capacity);
		break;
	}

	free_expandable_buffer(&buffer);
	// Flush and close input and output pipes
    fflush(pipein);
    pclose(pipein);
    //fflush(pipeout);
    //pclose(pipeout);
}
