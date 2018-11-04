#include <stdint.h>

typedef uint8_t BYTE;

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
		size_t old_capacity = buffer->capacity;
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

void expandable_buffer_push_file_end(expandable_buffer* buffer){
	BYTE* data_start = expandable_buffer_increase_size(buffer, 1);
	*data_start = '\0';
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
	switch(expandable_buffer_read_data_in(buffer, in, HEADER_SIZE)){
	case 0:
		// The file has no data
		assert(feof(in));
		return 1;
	case HEADER_SIZE:
		// No problem
		break;
	default:
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
		const int CRC = buffer->pointer[buffer->size - 1]
			+ (buffer->pointer[buffer->size - 2] << 8)
			+ (buffer->pointer[buffer->size - 3] << 16)
			+ (buffer->pointer[buffer->size - 4] << 24);
		//fprintf(stderr, "%d ", CRC);
	} while(strncmp(chunk_name, END_BLOCK_NAME, END_BLOCK_NAME_LENGTH) != 0);
	//fprintf(stderr, "\n");

	// This breaks ffmpeg
	//expandable_buffer_push_file_end(buffer);
	
	return 0;
}

int expandable_buffer_write_to_file(expandable_buffer* buffer, FILE* out){
	fflush(out);
	if (ftruncate(fileno(out), buffer->size) != 0){
		fprintf(stderr, "Error truncating the output file\n");
		return 1;
	}

	rewind(out);
	fwrite(buffer->pointer, sizeof(buffer->pointer[0]), buffer->size, out);
	fflush(out);
	
	return 0;
}
void expandable_buffer_write_to_pipe(expandable_buffer* buffer, FILE* out){
	BYTE* pointer = buffer->pointer;
	size_t size_left = buffer->size;
	while(size_left > 0) {
		size_t total_written = fwrite(pointer, sizeof(pointer[0]), size_left, out);
		pointer += total_written;
		size_left -= total_written;

		assert(size_left == 0);
		if (ferror(out)){
			clearerr(out);

			fd_set set;
			FD_ZERO(&set);
			FD_SET(fileno(out), &set);
			if (select(FD_SETSIZE, NULL, &set, NULL, NULL) == -1){
				fprintf(stderr, "Error writing to pipe\n");
				fprintf(stderr, "%s", strerror(errno));
			} 
		}
	}
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
void expandable_buffer_print_last_n_bytes(expandable_buffer* buffer, size_t n){
	fprintf(stdout, "Buffer(s = %zu) End: ", buffer->size);
	size_t start_index = buffer->size - n;
	if (start_index > buffer->size) start_index = 0; // If it overflowed, reset it 
	char number[3] = {'\0'};
	size_t i;
	for (i = 0; i < n && (i + start_index) < buffer->size; i++){
		sprintf(number, "%02x", buffer->pointer[start_index + i]);
		fprintf(stdout, "%s ", number);
	}
	fprintf(stdout, "\n");
}
