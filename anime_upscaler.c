#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

typedef unsigned char BYTE;

const size_t initialBufferSize = 64;
size_t currentBufferSize = initialBufferSize;
size_t currentBufferUsage = 0;
BYTE* buffer = NULL;

// Reads data from a FILE* and appends to buffer
// returns 0 if the size read in was different to the target size, 1 otherwise
int readDataIntoBuffer(FILE* in, size_t size){
	assert(buffer);

	if (currentBufferUsage + size > currentBufferSize){
		int target = currentBufferUsage + size;
		currentBufferSize = target - (target % 8) + 8;
		assert(currentBufferSize >= target);
		buffer = (BYTE*)realloc(buffer, currentBufferSize);
	}

	BYTE* startFrom = buffer + currentBufferUsage;
	currentBufferUsage += size;
	return fread(startFrom, 1, size, in);
}

int main(int argc, char* argv[]){
	/*if (argc == 1){
		fprintf(stderr, "Not enough arguments!\n");
		return 1;
		}*/
	
	buffer = (BYTE*)malloc(sizeof(BYTE) * currentBufferSize);
	
	// Open an input pipe from ffmpeg and an output pipe to a second instance of ffmpeg
    FILE *pipein = popen("ffmpeg -i small.mp4 -vcodec png -f image2pipe - 2> /dev/null", "r");

	// NOTE: For waifu2x file output, use -o /dev/stdout
	
    //FILE *pipeout = popen("ffmpeg -y -f image2pipe -vcodec png -i - -f mp4 -q:v 5 -an -vcodec mpeg4 output.mp4", "w");
     
	while(1){
		// Read PNG
		{
			currentBufferUsage = 0;
			
			// Read header
			const size_t headerSize = 8;
			const char* header = "\211PNG\r\n\032\n";
			if (readDataIntoBuffer(pipein, headerSize) != headerSize){
				fprintf(stderr, "Problem reading header\n");
				break;
			}
			
			if (memcmp(header, buffer, headerSize) != 0){
				buffer[8] = '\0';
				fprintf(stderr, "Header was different, was %s\n", buffer);
				break;
			}

			char chunkName[5] = {'\0'};
			do {
				const size_t lengthSize = 4;
				if (readDataIntoBuffer(pipein, 4) != lengthSize){
					fprintf(stderr, "problem reading chunk length\n");
					break;
				}
				int dataSize = buffer[currentBufferUsage - 4]
					+ (buffer[currentBufferUsage - 3] << 8)
					+ (buffer[currentBufferUsage - 2] << 16)
					+ (buffer[currentBufferUsage - 1] << 24);
				fprintf(stdout, "Chonk Data Size: %d\n", dataSize);

				const size_t nameSize = 4;
				if (readDataIntoBuffer(pipein, 4) != nameSize){
					fprintf(stderr, "problem reading chunk name\n");
					break;
				}
				strncpy(chunkName, buffer + (currentBufferUsage - nameSize), nameSize);

				if (dataSize != 0 && readDataIntoBuffer(pipein, dataSize) != dataSize){
					fprintf(stderr, "problem reading chunk data\n");
					break;
				}

				const size_t crcSize = 4;
				if (readDataIntoBuffer(pipein, crcSize) != crcSize){
					fprintf(stderr, "problem reading chunk crc\n");
					break;
				}
			} while(strncmp(chunkName, "IEND", 4) != 0);
		}

		fprintf(stdout, "Read a PNG file of size %zu, buffer is %zu\n", currentBufferUsage, currentBufferSize);
		break;
	}

	// Flush and close input and output pipes
    fflush(pipein);
    pclose(pipein);
    //fflush(pipeout);
    //pclose(pipeout);
}
