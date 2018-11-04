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
