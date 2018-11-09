#include <sys/stat.h>
#include <sys/types.h>

#define TEMP_DIR "/dev/shm/anime-upscaler/"
#define TEMP_FILE_PATTERN "anime-upscaler-tempfile-XXXXXX"
#define TEMP_FILE_PATH_PATTERN TEMP_DIR TEMP_FILE_PATTERN

typedef struct {
	char absolute_filename[PATH_MAX];
	FILE* file;
} temp_file;
temp_file* create_temp_file(char* access_type){
	struct stat st = {0};
	if (stat(TEMP_DIR, &st) == -1){
		mkdir(TEMP_DIR, 0700);
	}
	
	char filename[] = TEMP_FILE_PATH_PATTERN;
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
	
	//fprintf(stdout, "temp file output name is: %s\n", frame.output_filename);
	return frame;
}
void free_temp_frame(temp_frame* frame){
	free_expandable_buffer(&frame->buffer);
	free_temp_file(&frame->file);
	free(frame->generic_output_filename);
	unlink(frame->output_filename);
	free(frame->output_filename);
}
