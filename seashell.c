#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>            //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <fnmatch.h>
const char * sysname = "seashell";

int shortdir_del(char *short_name, char *file_name, int MAX_LINE_LENGTH);
int kdiff(int mod, char *file1_name, char *file2_name);
char *toLower(char *tok);
int isDuplicate(char *word);

enum return_codes {
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};
struct command_t {
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3]; // in/out redirection
	struct command_t *next; // for piping
};
/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t * command)
{
	int i=0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background?"yes":"no");
	printf("\tNeeds Auto-complete: %s\n", command->auto_complete?"yes":"no");
	printf("\tRedirects:\n");
	for (i=0;i<3;i++)
		printf("\t\t%d: %s\n", i, command->redirects[i]?command->redirects[i]:"N/A");
	printf("\tArguments (%d):\n", command->arg_count);
	for (i=0;i<command->arg_count;++i)
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}


}
/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
	if (command->arg_count)
	{
		for (int i=0; i<command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i=0;i<3;++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
		free_command(command->next);
		command->next=NULL;
	}
	free(command->name);
	free(command);
	return 0;
}
/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
	char cwd[1024], hostname[1024];
    gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}
/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
	const char *splitters=" \t"; // split at whitespace
	int index, len;
	len=strlen(buf);
	while (len>0 && strchr(splitters, buf[0])!=NULL) // trim left whitespace
	{
		buf++;
		len--;
	}
	while (len>0 && strchr(splitters, buf[len-1])!=NULL)
		buf[--len]=0; // trim right whitespace

	if (len>0 && buf[len-1]=='?') // auto-complete
		command->auto_complete=true;
	if (len>0 && buf[len-1]=='&') // background
		command->background=true;

	char *pch = strtok(buf, splitters);
	command->name=(char *)malloc(strlen(pch)+1);
	if (pch==NULL)
		command->name[0]=0;
	else
		strcpy(command->name, pch);

	command->args=(char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index=0;
	char temp_buf[1024], *arg;
	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch) break;
		arg=temp_buf;
		strcpy(arg, pch);
		len=strlen(arg);

		if (len==0) continue; // empty arg, go for next
		while (len>0 && strchr(splitters, arg[0])!=NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len>0 && strchr(splitters, arg[len-1])!=NULL) arg[--len]=0; // trim right whitespace
		if (len==0) continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|")==0)
		{
			struct command_t *c=malloc(sizeof(struct command_t));
			int l=strlen(pch);
			pch[l]=splitters[0]; // restore strtok termination
			index=1;
			while (pch[index]==' ' || pch[index]=='\t') index++; // skip whitespaces

			parse_command(pch+index, c);
			pch[l]=0; // put back strtok termination
			command->next=c;
			continue;
		}

		// background process
		if (strcmp(arg, "&")==0)
			continue; // handled before

		// handle input redirection
		redirect_index=-1;
		if (arg[0]=='<')
			redirect_index=0;
		if (arg[0]=='>')
		{
			if (len>1 && arg[1]=='>')
			{
				redirect_index=2;
				arg++;
				len--;
			}
			else redirect_index=1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index]=malloc(len);
			strcpy(command->redirects[redirect_index], arg+1);
			continue;
		}

		// normal arguments
		if (len>2 && ((arg[0]=='"' && arg[len-1]=='"')
			|| (arg[0]=='\'' && arg[len-1]=='\''))) // quote wrapped arg
		{
			arg[--len]=0;
			arg++;
		}
		command->args=(char **)realloc(command->args, sizeof(char *)*(arg_index+1));
		command->args[arg_index]=(char *)malloc(len+1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count=arg_index;
	return 0;
}
void prompt_backspace()
{
	putchar(8); // go back 1
	putchar(' '); // write empty over
	putchar(8); // go back 1 again
}
/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
	int index=0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

    // tcgetattr gets the parameters of the current terminal
    // STDIN_FILENO will tell tcgetattr that it should write the settings
    // of stdin to oldt
    static struct termios backup_termios, new_termios;
    tcgetattr(STDIN_FILENO, &backup_termios);
    new_termios = backup_termios;
    // ICANON normally takes care that one line at a time will be processed
    // that means it will return if it sees a "\n" or an EOF or an EOL
    new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
    // Those new settings will be set to STDIN
    // TCSANOW tells tcsetattr to change attributes immediately.
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);


    //FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state=0;
	buf[0]=0;
  	while (1)
  	{
		c=getchar();
		//printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c==9) // handle tab
		{
			buf[index++]='?'; // autocomplete
			break;
		}

		if (c==127) // handle backspace
		{
			if (index>0)
			{
				prompt_backspace();
				index--;
			}
			continue;
		}
		if (c==27 && multicode_state==0) // handle multi-code keys
		{
			multicode_state=1;
			continue;
		}
		if (c==91 && multicode_state==1)
		{
			multicode_state=2;
			continue;
		}
		if (c==65 && multicode_state==2) // up arrow
		{
			int i;
			while (index>0)
			{
				prompt_backspace();
				index--;
			}
			for (i=0;oldbuf[i];++i)
			{
				putchar(oldbuf[i]);
				buf[i]=oldbuf[i];
			}
			index=i;
			continue;
		}
		else
			multicode_state=0;

		putchar(c); // echo the character
		buf[index++]=c;
		if (index>=sizeof(buf)-1) break;
		if (c=='\n') // enter key
			break;
		if (c==4) // Ctrl+D
			return EXIT;
  	}
  	if (index>0 && buf[index-1]=='\n') // trim newline from the end
  		index--;
  	buf[index++]=0; // null terminate string

  	strcpy(oldbuf, buf);

  	parse_command(buf, command);

  	//print_command(command); // DEBUG: uncomment for debugging

    // restore the old settings
    tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
  	return SUCCESS;
}
int process_command(struct command_t *command);
int main()
{
	while (1)
	{
		struct command_t *command=malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

		int code;
		code = prompt(command);
		if (code==EXIT) break;

		code = process_command(command);
		if (code==EXIT) break;

		free_command(command);
	}

	printf("\n");
	return 0;
}

int process_command(struct command_t *command)
{
	int r;
	if (strcmp(command->name, "")==0) return SUCCESS;

	if (strcmp(command->name, "exit")==0)
		return EXIT;

	if (strcmp(command->name, "cd")==0)
	{
		if (command->arg_count > 0)
		{
			r=chdir(command->args[0]);
			if (r==-1)
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			return SUCCESS;
		}
	}

	int pipe_buff[2];
	if(pipe(pipe_buff) == -1){
		printf("Pipe failed.");
		exit(0);
	}

	pid_t pid=fork();
	if (pid==0) // child
	{
		/// This shows how to do exec with environ (but is not available on MacOs)
	    // extern char** environ; // environment variables
		// execvpe(command->name, command->args, environ); // exec+args+path+environ

		/// This shows how to do exec with auto-path resolve
		// add a NULL argument to the end of args, and the name to the beginning
		// as required by exec

		// increase args size by 2
		command->args=(char **)realloc(
			command->args, sizeof(char *)*(command->arg_count+=2));

		// shift everything forward by 1
		for (int i=command->arg_count-2;i>0;--i)
			command->args[i]=command->args[i-1];

		// set args[0] as a copy of name
		command->args[0]=strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count-1]=NULL;

		/** PART 3 **/
		if (strcmp(command->name, "highlight") == 0){ //TODO NOKTALAMADAN SONRA \n GELİNCE NEW LINE YAPMIYO

        	FILE *file;
        	char *token;
        	char *copy;
			char *strippedline;
        	char line[500];
        	file = fopen(command->args[3], "r");
        	
			if(command->arg_count != 5){
            	printf("Missing arguments. Try again.\n");
            	exit(0);
        	}

			if(file == NULL){
				printf("Cannot open file: %s\n", command->args[3]);
				exit(0);
			}

			while(fgets(line, 500, file) != NULL){
            	strippedline = strtok(line,"\n\r");
            	copy=strdup(strippedline);
            	token = strtok(strippedline, " :;.,\n\r\t");
				while(token != NULL){

					if(strcmp(toLower(token), toLower(command->args[1])) == 0){
						if(strcmp("g", command->args[2])==0){
							printf("\033[0;32m");
						}else if(strcmp("b", command->args[2])==0){
							printf("\033[0;34m");
						}else{        // red if not specified
							printf("\033[0;31m");
						}
						printf("%s\033[0m%c",token,copy[token-strippedline+strlen(token)]);

					}else{
						printf("%s%c",token,copy[token-strippedline+strlen(token)]);
					}
					token = strtok(NULL, " :;.,\n\r\t");
				}
				printf("\n");
			}
			fclose(file);
    		exit(0);
    	}

		/** PART 4 **/
		if(strcmp(command->name, "goodMorning") == 0){
			if(command->arg_count == 4){
				char *time_pattern = "[0-2][0-9].[0-5][0-9]";
				if(fnmatch(time_pattern, command->args[1], 0) != 0){
					printf("Invalid time\n");
					exit(0);
				}

				char *home_path = getenv("HOME");
				char file_name[150];
				strcpy(file_name, home_path);
				strcat(file_name, "/playmusic.txt");
		
				char *hour = strtok(command->args[1], ".");
				char *minute = strtok(NULL, " ");
				FILE *file = fopen(file_name, "w");
		
				fprintf(file, "%s %s", minute, hour);
				fprintf(file, " * * * XDG_RUNTIME_DIR=/run/user/$(id -u) DISPLAY=:0.0 /usr/bin/rhythmbox-client --play ");
				fprintf(file, "%s\n", command->args[2]);

				fclose(file);

				char cmd[200];
				strcpy(cmd, "crontab ");
				strcat(cmd, file_name);
				system(cmd);
				exit(0);
			} else {
				printf("Invalid arguments\n");
				exit(0);
			}
		}

		/** PART 5 **/
		if(strcmp(command->name, "kdiff") == 0){
			if(command->arg_count == 5 && strcmp(command->args[1], "-a") == 0){
				if(kdiff(0, command->args[2], command->args[3]) == SUCCESS)
					exit(0);
			} else if (command->arg_count == 4){
				if(kdiff(0, command->args[1], command->args[2]) == SUCCESS)
					exit(0);
			} else if(command->arg_count == 5 && strcmp(command->args[1], "-b") == 0){
				if(kdiff(1, command->args[2], command->args[3]) == SUCCESS)
					exit(0);
			} else {
				printf("Invalid arguments\n");
				exit(0);
			}
		}

		/** PART 2 **/
		if(strcmp(command->name, "shortdir") == 0){
			if(command->arg_count == 1 || command->arg_count > 4){
				printf("Invalid arguments\n");
				exit(0);
			}

			char *func_name = command->args[1];
			int MAX_LINE_LENGTH = 500;
			int MAX_DIR_LENGTH = 300;

			char *home_path = getenv("HOME");
			char file_name[MAX_DIR_LENGTH];
			strcpy(file_name, home_path);
			strcat(file_name, "/shortdir.txt");

			if(command->arg_count == 3){

				if(strcmp(func_name, "clear") == 0){
					FILE *shdir_file = fopen(file_name, "w");
					fclose(shdir_file);

					exit(0);

				} else if(strcmp(func_name, "list") == 0){
					FILE *shdir_file = fopen(file_name, "r");

					if(shdir_file != NULL){
						char line[MAX_LINE_LENGTH];
						fgets(line, MAX_LINE_LENGTH, shdir_file);
						while(!feof(shdir_file)){
							printf("%s", line);
							fgets(line, MAX_LINE_LENGTH, shdir_file);
						}

						fclose(shdir_file);
					}

					exit(0);
				}
			} else if(command->arg_count == 4){

				char *short_name = command->args[2];

				if(strcmp(func_name, "set") == 0){
					FILE *shdir_file_r = fopen(file_name, "r");

					char curr_dir[MAX_DIR_LENGTH];

					getcwd(curr_dir, sizeof(curr_dir));
				
					if(shdir_file_r != NULL){
						char line[MAX_LINE_LENGTH];
						fgets(line, MAX_LINE_LENGTH, shdir_file_r);
						while(!feof(shdir_file_r)){
							char *dir = strtok(line, " ");
							char *sh = strtok(NULL, "\n");

							if(strcmp(sh, short_name) == 0){
								if(strcmp(dir, curr_dir) == 0) exit(0);

								printf("There exists a shortdir: %s associated to directory: %s\nDelete the existing associaton or try another short name\n", sh, dir);
								exit(0);
							}

							if(strcmp(dir, curr_dir) == 0){
								shortdir_del(sh, file_name, MAX_LINE_LENGTH);
								break;
							}
							fgets(line, MAX_LINE_LENGTH, shdir_file_r);
						}
						fclose(shdir_file_r);
					}

					FILE *shdir_file_a= fopen(file_name, "a");
					fprintf(shdir_file_a, "%s %s\n", curr_dir, short_name);

					fclose(shdir_file_a);

					exit(0);

				} else if(strcmp(func_name, "jump") == 0){
					FILE *shdir_file = fopen(file_name, "r");

					if(shdir_file != NULL){
						char line[MAX_LINE_LENGTH];
						fgets(line, MAX_LINE_LENGTH, shdir_file);
						while(!feof(shdir_file)){
							char *dir = strtok(line, " ");
							char *sh = strtok(NULL, "\n");

							if(strcmp(short_name, sh) == 0){
								//piping to change directory in parent process
								fclose(shdir_file);
								close(pipe_buff[0]);
								write(pipe_buff[1], dir, strlen(dir) + 1);
								close(pipe_buff[0]);
								exit(0);
							}

							fgets(line, MAX_LINE_LENGTH, shdir_file);
						}

						fclose(shdir_file);
					}

					printf("shortdir not found.\n");

					//to indicate not to change directory
					close(pipe_buff[0]);
					write(pipe_buff[1], " ", 2);
					close(pipe_buff[0]);
					
					exit(0);

				} else if(strcmp(func_name, "del") == 0){
					if(shortdir_del(short_name, file_name, MAX_LINE_LENGTH) == SUCCESS)
						exit(0);
				}
			} else {
				printf("Invalid arguments\n");
				exit(0);
			} 
		}

		/**PART 6**/
		if(strcmp(command->name, "unique") == 0){
			if(command->arg_count != 4){
				printf("Invalid arguments\n");
				exit(0);
			}

			int MAX_LINE_LENGTH = 500;

			if(strcmp(command->args[1], "-l") == 0 || strcmp(command->args[1], "-f") == 0){
				FILE *file = fopen(command->args[2], "r");
				FILE *temp = fopen("temp.txt", "w");
				FILE *unique_words = fopen("unique.txt", "w");
				fclose(unique_words);

				char *token;
				char line[MAX_LINE_LENGTH];

				if(file == NULL || temp == NULL || unique_words == NULL){
					printf("Error opening the file: %s\n", command->args[2]);
					exit(0);
				}
				
				while(fgets(line, MAX_LINE_LENGTH, file) != NULL){
					token = strtok(line, " \n");
					while(token != NULL){
						if(!isDuplicate(token)){
							fprintf(temp, "%s ", token);
							unique_words = fopen("unique.txt", "a");
							fprintf(unique_words, "%s\n", token);
							fclose(unique_words);
						}
						
						token = strtok(NULL, " \n");
					}
					fprintf(temp, "\n");
					if(strcmp(command->args[1], "-l") == 0){
						unique_words = fopen("unique.txt", "w");
						fclose(unique_words);
					}
				}

				fclose(file);
				fclose(temp);

				temp = fopen("temp.txt", "r");
				file = fopen(command->args[2],"w");

				while(fgets(line, MAX_LINE_LENGTH, temp) != NULL){
					fprintf(file, "%s", line);
				}

				fclose(file);
				fclose(temp);

				remove("temp.txt");
				remove("unique.txt");

				exit(0);
				
			} else {
				printf("Invalid arguments\n");
				exit(0);
			}
		}

		//execvp(command->name, command->args); // exec+args+path
		//exit(0);
		/// TODO: do your own exec with path resolving using execv()

		/** PART 1 **/
		char *env_path = getenv("PATH");
		char *token = strtok(env_path, ":");
		char *path = calloc(200, sizeof(char));
		while(token != NULL){
			strcpy(path, token);
			strcat(path, "/");
			strcat(path, command->name);
			execv(path, command->args);
			free(path);
			path = calloc(200, sizeof(char));
			token = strtok(NULL, ":");
		}
	}
	else
	{
		if (!command->background)
			wait(0); // wait for child process to finish
		
		if(strcmp(command->name, "shortdir") == 0 && strcmp(command->args[0], "jump") == 0){
			close(pipe_buff[1]);
			char dir[300];
			read(pipe_buff[0], dir, 300);
			close(pipe_buff[0]);

			if(strcmp(dir, " ") != 0){
				// We get this part from cd command
				r = chdir(dir);
				if (r == -1)
					printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			}
		}
		
		return SUCCESS;
	}

	// TODO: your implementation here

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}

int shortdir_del(char *short_name, char *file_name, int MAX_LINE_LENGTH){
	FILE *shdir_file_r = fopen(file_name, "r");
	FILE *temp_w = fopen("temp.txt", "w");

	char line[MAX_LINE_LENGTH];
	if(shdir_file_r != NULL){	
		fgets(line, MAX_LINE_LENGTH, shdir_file_r);
		while(!feof(shdir_file_r)){
			char *dir = strtok(line, " ");
			char *sh = strtok(NULL, "\n");

			if(strcmp(short_name, sh) != 0){
				fprintf(temp_w, "%s %s\n", dir, sh);
			}

			fgets(line, MAX_LINE_LENGTH, shdir_file_r);
		}
				
		fclose(shdir_file_r);
	}

	fclose(temp_w);

	FILE *shdir_file_w = fopen(file_name, "w");
	FILE *temp_r = fopen("temp.txt", "r");

	fgets(line, MAX_LINE_LENGTH, temp_r);
	while(!feof(temp_r)){
		fprintf(shdir_file_w, "%s", line);
		fgets(line, MAX_LINE_LENGTH, temp_r);
	}

	fclose(temp_r);
	fclose(shdir_file_w);

	remove("temp.txt");

	return SUCCESS;
}

int kdiff(int mod, char *file1_name, char *file2_name){
	int MAX_LINE_LENGTH = 500;

	FILE *file1;
	FILE *file2;

	char *text_pattern = "*.txt";
	if(fnmatch(text_pattern, file1_name, 0) != 0 && fnmatch(text_pattern, file2_name, 0) != 0){
		printf("please enter .txt files\n");
		exit(0);
	}
	
	if(mod == 0){
		file1 = fopen(file1_name, "r");
		file2 = fopen(file2_name, "r");
	} else {
		file1 = fopen(file1_name, "rb");
		file2 = fopen(file2_name, "rb");
	}

	if(file1 == NULL && file2 == NULL){
		printf("Cannot open files: %s %s\n", file1_name, file2_name);
		return SUCCESS;
	} else if(file1 == NULL){
		printf("Cannot open file: %s\n", file1_name);
		return SUCCESS;
	} else if(file2 == NULL){
		printf("Cannot open file: %s\n", file2_name);
		return SUCCESS;
	}

	int counter = 1;
	int diffcounter = 0;

	if(mod == 0){ //mod -a
		char *line1 = malloc(MAX_LINE_LENGTH);
		char *line2 = malloc(MAX_LINE_LENGTH);

		fgets(line1, MAX_LINE_LENGTH, file1);
		fgets(line2, MAX_LINE_LENGTH, file2);
		while(!feof(file1) && !feof(file2)){
			if(strcmp(line1, line2) != 0){
				printf("%s:Line %d: %s", file1_name, counter, line1);
				printf("%s:Line %d: %s", file2_name, counter, line2);
				diffcounter++;
			}
			fgets(line1, MAX_LINE_LENGTH, file1);
			fgets(line2, MAX_LINE_LENGTH, file2);
			counter++;
		}
	
		while(!feof(file1))	{
			printf("%s:Line %d: %s", file1_name, counter, line1);
			printf("%s:Line %d: NULL\n", file2_name, counter);
			fgets(line1, MAX_LINE_LENGTH, file1);
			diffcounter++;
			counter++;
		}
	
		while(!feof(file2))	{
			printf("%s:Line %d: NULL\n", file1_name, counter);
			printf("%s:Line %d: %s", file2_name, counter, line2);
			fgets(line2, MAX_LINE_LENGTH, file2);
			diffcounter++;
			counter++;
		}

		free(line1);
		free(line2);

	} else { //mod -b
		int char1;
		int char2;
		char1 = getc(file1);
		char2 = getc(file2);
			
		while(char1 != EOF && char2 != EOF){
			if(char1 != char2){
				diffcounter++;
			}
			char1 = getc(file1);
			char2 = getc(file2);
		}

		while(char1 != EOF){
			diffcounter++;
			char1 = getc(file1);
		}

		while(char2 != EOF){
			diffcounter++;
			char2 = getc(file2);
		}

	}
	
	if(diffcounter == 0){
		printf("The two files are identical\n");
	} else if (mod == 0){
		printf("%d different lines found\n", diffcounter);
	} else {
		printf("Two files are different in %d bytes\n", diffcounter);
	}

	fclose(file1);
	fclose(file2);
	
	return SUCCESS;
}

char *toLower(char *tok) {
    char *token = strdup(tok);
    for (int i = 0; i < strlen(token); i++) {
        if (*(token+i) >= 65 && *(token+i) <= 90) 
			*(token+i)= *(token+i)+32;
	}
    return token;
}

int isDuplicate(char *word){
	FILE *filer = fopen("unique.txt", "r");
	char lline[500];
	while(fgets(lline, 500, filer) != NULL){
		lline[strcspn(lline,"\n")] = '\0';
		if(strcmp(lline, word) == 0){
			fclose(filer);
			return 1;
		}
	}

	fclose(filer);
	return 0;
}