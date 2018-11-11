typedef union {
	int array[2];
	struct {
		int read_from; // read from
		int write_to; // write to
	} files;
} pipe_data;

pipe_data create_pipe_data(){
	pipe_data data;
	if (pipe(data.array)){
		fprintf(stderr, "Failed to create pipe\n");
		exit(1);
	}
	return data;
}
void pipe_data_close(pipe_data* pipe){
	if (pipe->array[0] != -1) close(pipe->array[0]);
	if (pipe->array[1] != -1) close(pipe->array[1]);

	pipe->array[0] = -1;
	pipe->array[1] = -1;
}
void pipe_data_close_read_from(pipe_data* pipe){
	if(pipe->files.read_from == -1) return;
	close(pipe->files.read_from);
	pipe->files.read_from = -1;
}
void pipe_data_close_write_to(pipe_data* pipe){
	if(pipe->files.write_to == -1) return;
	close(pipe->files.write_to);
	pipe->files.write_to = -1;
}

pid_t fork_to_function(int (*func)(void*), void* data, char* working_directory, pipe_data* input_pipe, pipe_data* output_pipe, pipe_data* err_pipe){
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
				fprintf(stderr, "Failed to redirect input for fork\n");
				exit(1);
			}
			pipe_data_close_write_to(input_pipe); // We don't need this anymore
		}
		if (output_pipe != NULL){
			// Redirect stdout
			if (dup2(output_pipe->files.write_to, 1) < 0){
				pipe_data_close_write_to(output_pipe);
				fprintf(stderr, "Failed to redirect output for fork\n");
				exit(1);
			}
			pipe_data_close_read_from(output_pipe); // We don't need this anymore
		}
		// Redirect stderr to /dev/null
		if (err_pipe == NULL){
			//freopen("/dev/null", "w", stderr);
		}else{
			// Redirect stderr
			if (dup2(err_pipe->files.write_to, 2) < 0){
				pipe_data_close_write_to(err_pipe);
				fprintf(stderr, "Failed to redirect stderr for fork\n");
				exit(1);
			}
			pipe_data_close_read_from(err_pipe); // We don't need this anymore
		}
		if (working_directory != NULL){
			chdir(working_directory);
		}
		// Run the command, exit with returned status
		// Use _exit so we don't call exit handlers
		_exit((*func)(data));
		break;
	default:
		// We are the parent
		if (err_pipe != NULL) pipe_data_close_write_to(err_pipe);
		if (output_pipe != NULL) pipe_data_close_write_to(output_pipe);
		if (input_pipe != NULL) pipe_data_close_read_from(input_pipe);
	}
	return process_id;
}

int exec_from_void(void* command){
	char** actual_command = (char**)command;
	execvp(actual_command[0], actual_command);
	fprintf(stderr, "Failed to exec() command %s with err %s\n", actual_command[0], strerror(errno));
	return 1;
}

pid_t run_command(char* const command[], char* working_directory, pipe_data* input_pipe, pipe_data* output_pipe, pipe_data* err_pipe){
	return fork_to_function(&exec_from_void, (void*)command, working_directory, input_pipe, output_pipe, err_pipe);
}

int fix_carriage_return_passthrough(void* arg){
	int c;
	while((c = getc(stdin)) != EOF){
		fputc(c == '\r' ? '\n' : c, stdout);
	}
	fflush(stdout);
	fclose(stdout);
	return 0;
}
