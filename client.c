#include <stdbool.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <stdint.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>

// sigaction constant
volatile sig_atomic_t shutdown_initiated = 0;

// message type constants
typedef enum MessageType 
{LOGIN=0, LOGOUT=1, MESSAGE_SEND=2, MESSAGE_RECV=10, DISCONNECT=12, SYSTEM=13} message_type_t;

// packed message struct
typedef struct __attribute__((packed)) Message 
	{
	 uint32_t type;
	 uint32_t timestamp;
	 char username[32];
	 char message[1024];
	} message_t;

// TUI message structure
typedef struct {
    char timestamp[80];
    char username[32];
    char message[1024];
    bool is_system;
    bool is_disconnect;
} tui_msg_t;

// client settings struct
typedef struct Settings {
    struct sockaddr_in server;
    bool quiet;
    int sockfd;
    bool running;
    char username[32];
    bool tui_mode;
} settings_t;

// color and settings declared
static char* COLOR_RED = "\033[31m";
static char* COLOR_GRAY = "\033[90m";
static char* COLOR_RESET = "\033[0m";
static settings_t settings = {0};

// TUI state - only 28 messages
static tui_msg_t messages[28];
static int msg_count = 0;
static char input_buffer[1024] = {0};
static int input_pos = 0;
static pthread_mutex_t tui_mutex = PTHREAD_MUTEX_INITIALIZER;
static int screen_drawn = 0;
static struct termios original_termios;

// simply handles printing the error
void print_error(const char* error_message)
{
        fprintf(stderr, "%s\n", error_message);
}  

// prints help statement
int print_help()
{
	printf(
"usage: ./client [-h] [--port PORT] [--ip IP] [--domain DOMAIN] [--quiet] [--tui]\n"
"\n"
"mycord client\n"
"\n"
"options:\n"
  "  --help                show this help message and exit\n"
  "  --port PORT           port to connect to (default: 8080)\n"
  "  --ip IP               IP to connect to (default: \"127.0.0.1\")\n"
  "  --domain DOMAIN       Domain name to connect to (if domain is specified, IP must not be)\n"
  "  --quiet               do not perform alerts or mention highlighting\n"
  "  --tui                 enable Text User Interface mode\n"
"\n"
"examples:\n"
  "  ./client --help (prints the above message)\n"
  "  ./client --port 1738 (connects to a mycord server at 127.0.0.1:1738)\n"
  "  ./client --domain example.com (connects to a mycord server at example.com:8080)\n");
}


int process_args(int argc, char *argv[]) 
{
    // loops through all parts of argc and changes settings to indicated choice
        for(int i=1; i < argc; i++)
        {
                // checks for --help/-h
                if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
                {
                        // calls help helper
                        print_help();
                        // then exits code
                        exit(0);
                }
		// checks for --port
               	else if(strcmp(argv[i], "--port") == 0 && argc > i+1)
                {
			// char pointer for conversion 
			char* end_part;
			// attempts to covert to a base 10 number
			unsigned long port = strtoul(argv[i+1], &end_part, 10);
			// checks to make sure valid port given
			if(*end_part == '\0' && port <= 65535)
			{
				// assigns port value in global struct
                        	settings.server.sin_port = htons((unsigned int)atoi(argv[i+1]));
                        	i++;
			}
			// something about port was invalid, error
			else
			{
				print_error("Error: Invalid Port");
				exit(7);
			}
                      	
                }
		// checks for --ip
		else if(strcmp(argv[i], "--ip") == 0 && argc > i+1)
                {
			// buffer to pton
			unsigned char ip_buffer[sizeof(struct in_addr)];
			// checks if IP value is even an IP
			if(inet_pton(AF_INET, argv[i+1], ip_buffer) != 1)
			{
				print_error("Error: Invalid IP Address Provided");
				exit(10);
			}
			// checks addr not already specified
			if(settings.server.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
			{
				print_error("Error: Double IP/Domain specified");
				exit(1);
			}
			// assigns IP val
			settings.server.sin_addr = *(struct in_addr*)ip_buffer;
                        i++;
                }
		// checks for --domain
		else if(strcmp(argv[i], "--domain") == 0 && argc > i+1)
                {
			// checks to make sure addr not specified
			// checks addr not already specified
                        if(settings.server.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
                        {
                                print_error("Error: Double IP/Domain specified");
                                exit(1);
                        }
                        // host look up
			struct hostent* host = gethostbyname(argv[i+1]); 
			if(host == NULL)
			{
				print_error("Error: DNS lookup failure");
				exit(2);
			}
			// copies first ip looked up to the struct
                        settings.server.sin_addr = *(struct in_addr*)host->h_addr_list[0];
                        i++;
			
                }
		// checks for --quiet
		else if(strcmp(argv[i], "--quiet") == 0)
                {
                        settings.quiet = true;
                } 
		// checks for --tui
		else if(strcmp(argv[i], "--tui") == 0)
                {
                        settings.tui_mode = true;
                } 
		// catch case
		else
		{
			print_error("Error: Invalid Arguement");
			exit(6);
		}
	}
	return 1;
}

int get_username() 
{
    // username buffer
    char username[32];
    // uses whoami command to get username
    FILE* fp = popen("whoami", "r");
    // check popen failure
    if(!fp)
    {
	    print_error("Error: Login Retrival Failure");
            exit(4);
    }
    // gets login value and checks failure
    if(fgets(username, 32, fp) != NULL)
    {
	    // checks nonempty
            if(strlen(username) == 0)
            {
                    print_error("Error: Invalid Username");
                    exit(5);
            }
	    // checks all characters printable
	    int i = 0;
	    while(username[i] != '\n')
	    {
		    if(!isprint(username[i]))
		    {
			    print_error("Error: Invalid Username");
			    exit(5);
		    }
		    i++;
	    }
	    // copies buffer username to global
            strncpy(settings.username, username, 31);
	    // replaces  new line with null terminator
	    settings.username[strcspn(settings.username, "\n")] = '\0';
	    // close file
	    pclose(fp);
	    return 1;
    }
    else
    {
            print_error("Error: Login Retrival Failure");
            exit(4);
    }
}

// handles signals by starting shutdown sequence
void handle_signal(int signal) 
{
    shutdown_initiated = 1;
}

void send_logout()
{
    // makes message struct for logout
    message_t logout;
    // 0s memory
    memset(&logout, 0, sizeof(logout));
    // provides type
    logout.type = htonl(LOGOUT);
    // writes login over socket
    ssize_t total_written = write(settings.sockfd, &logout, sizeof(logout));
    // error catch
    if(total_written == -1)
    {
            // Don't print error if socket is already closed
            if (errno != EBADF && errno != EPIPE) {
                print_error("Error: Write Error");
            }
            // close socket if it's still open
            if (settings.sockfd >= 0) {
                close(settings.sockfd);
            }
    }
}

// TUI functions - TERMINAL MODE
void enable_raw_mode() {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &original_termios);
    raw = original_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
}

void tui_clear() {
    printf("\033[2J\033[H");
}

void tui_add_message(const char* timestamp, const char* username, const char* message, 
                    bool is_system, bool is_disconnect) {
    pthread_mutex_lock(&tui_mutex);
    
    // Shift messages up if we have 28 already
    if (msg_count >= 28) {
        for (int i = 0; i < 27; i++) {
            messages[i] = messages[i + 1];
        }
        msg_count = 27;
    }
    
    // Add new message at the end
    strncpy(messages[msg_count].timestamp, timestamp, 79);
    strncpy(messages[msg_count].username, username, 31);
    strncpy(messages[msg_count].message, message, 1023);
    messages[msg_count].is_system = is_system;
    messages[msg_count].is_disconnect = is_disconnect;
    messages[msg_count].timestamp[79] = '\0';
    messages[msg_count].username[31] = '\0';
    messages[msg_count].message[1023] = '\0';
    
    if (msg_count < 28) {
        msg_count++;
    }
    
    pthread_mutex_unlock(&tui_mutex);
}

void tui_draw() {
    pthread_mutex_lock(&tui_mutex);
    
    if (!screen_drawn) {
        // First time draw - clear screen and draw everything
        tui_clear();
        screen_drawn = 1;
    } else {
        // Subsequent draws - just move cursor to top and clear from there
        printf("\033[H");  // Move cursor to top-left
        printf("\033[J");  // Clear from cursor to end of screen
    }
    
    // Draw header
    printf("=== mycord (last %d messages) ===\n", msg_count);
    printf("Connected as: %s\n\n", settings.username);
    
    // Draw messages - always show last 28 or however many we have
    for (int i = 0; i < msg_count; i++) {
        if (messages[i].is_disconnect) {
            printf("%s[DISCONNECT] %s%s\n", COLOR_RED, messages[i].message, COLOR_RESET);
        } else if (messages[i].is_system) {
            printf("%s[SYSTEM] %s%s\n", COLOR_GRAY, messages[i].message, COLOR_RESET);
        } else {
            if (settings.quiet) {
                printf("[%s] %s: %s\n", messages[i].timestamp, messages[i].username, messages[i].message);
            } else {
                // Check for mentions
                char search[33];
                sprintf(search, "@%s", settings.username);
                char* line = messages[i].message;
                char* match = NULL;
                
                printf("\a[%s] %s: ", messages[i].timestamp, messages[i].username);
                
                while ((match = strstr(line, search)) != NULL) {
                    fwrite(line, 1, match - line, stdout);
                    printf("%s%s%s", COLOR_RED, search, COLOR_RESET);
                    line = match + strlen(search);
                }
                printf("%s\n", line);
            }
        }
    }
    
    // Draw input line - ALWAYS show current input_buffer
    printf("\n----------------------------------------\n");
    printf("> %s", input_buffer);
    
    fflush(stdout);
    pthread_mutex_unlock(&tui_mutex);
}

void* receive_messages_thread(void* arg) {
    // while actively connected is (running) true
    while(settings.running)
    {
	// message struct
	message_t received;
	char time_buffer[80];
        // gets total we need to rad
	ssize_t bytes_to_read = sizeof(received);
	// counter for bytes read
	ssize_t total_bytes_read = 0;
	// loops until full read
	while(total_bytes_read < bytes_to_read)
	{
		// reads and puts into struct in order
		ssize_t bytes_read = read(settings.sockfd, ((char*)&received) + total_bytes_read, sizeof(received) - total_bytes_read);
		// Some read error if <0
		if(bytes_read < 0)
		{
			print_error("Error: Read Error");
			exit(15);
		}
		// a 0 means the peer disconnected
		if(bytes_read == 0)
		{
			// NO ERROR MESSAGE
			settings.running = false;
			return NULL;
		}
		// adds to the counter
		total_bytes_read += bytes_read;
	}
	// converted time
	time_t c_time = (time_t) ntohl(received.timestamp);
	// formats time
	struct tm* formatted_time = localtime(&c_time);
	strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", formatted_time);
	
        // check the message type
	// basic message
	if(received.type == ntohl(MESSAGE_RECV))
	{
		if (settings.tui_mode) {
            tui_add_message(time_buffer, received.username, received.message, false, false);
            tui_draw();  // Update display with new message
        } else {
            // when quiet just straight print
            if(settings.quiet)
            {
                printf("[%s] %s: %s\n", time_buffer, received.username, received.message);
            }
            // none quiet usage
            else
            {
                // line intialized
                char* line = received.message;
                // match initialized
                char* match = NULL;
                // audio que
                printf("\a[%s] %s: ", time_buffer, received.username);
                // username buffer with @
                char search[33];
                // creates search variable
                sprintf(search, "@%s", settings.username);
                // loops through all substrings on mention
                while((match = strstr(line, search)) != NULL)
                {
                    // writes until match
                    fwrite(line, 1, match-line, stdout);
                    fprintf(stdout, "%s%s%s", COLOR_RED, search, COLOR_RESET);
                    // advances the parsing
                    line = match + strlen(search);
                }
                // remnants
                printf("%s\n", line);
            }
        }
	}
	// for system types
	else if(received.type == ntohl(SYSTEM))
	{
		if (settings.tui_mode) {
            tui_add_message("", "", received.message, true, false);
            tui_draw();  // Update display with new message
        } else {
            // simple print
            printf("%s[SYSTEM] %s%s\n", COLOR_GRAY, received.message, COLOR_RESET);
        }
	}
	// disconnect types
	else if(received.type == ntohl(DISCONNECT))
	{
        if (settings.tui_mode) {
            tui_add_message("", "SERVER", received.message, false, true);
            tui_draw();  // Show the disconnect message
        } else {
            printf("%s[DISCONNECT] %s%s\n", COLOR_RED, received.message, COLOR_RESET);
        }
        // Set shutdown flag for graceful exit
        shutdown_initiated = 1;
        settings.running = false;
        return NULL;  // Exit thread, let main thread cleanup
	}
	// catch case
	else
	{
		print_error("Error: Message from Server Recieve Error");
		exit(16);
	}
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    // setup sigactions (ill-advised to use signal for this project, use sigaction with default (0) flags instead)
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    // parse arguments
    // sets all struct values to defaults before parsing arguements
    settings.server.sin_family = AF_INET;
    settings.server.sin_port = htons(8080);
    settings.server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    settings.quiet = false;
    settings.running = false;
    settings.tui_mode = false;
    process_args(argc, argv);
    // get username
    get_username();
    // create socket
    settings.sockfd = socket(AF_INET, SOCK_STREAM, 0);
    // error check for socket
    if(settings.sockfd == -1)
    {
	    print_error("Error: Socket Creation Failure");
	    exit(8);
    }
    // connect to server
    if(connect(settings.sockfd, (const struct sockaddr*)&settings.server, sizeof(settings.server)) < 0)
    {
	    print_error("Error: Connection Failure");
	    exit(9);
    }
    // client is connected and running
    settings.running = true;
    // makes message struct for login    
    message_t login;
    // 0s memory
    memset(&login, 0, sizeof(login)); 
    // copies username
    strncpy(login.username, settings.username, sizeof(login.username) - 1);
    login.username[sizeof(login.username) - 1] = '\0';
    // provides type
    login.type = htonl(LOGIN);
    // writes login over socket
    ssize_t total_written = write(settings.sockfd, &login, sizeof(login));
    // error catch
    if(total_written == -1)
    {
	    print_error("Error: Write Error");
	    close(settings.sockfd);
	    exit(11);
    }

    // Setup TUI if enabled
    if (settings.tui_mode) {
        // Enable raw mode for character-by-character input
        enable_raw_mode();
        
        // Add welcome message
        tui_add_message("", "SYSTEM", "Connected to mycord server", true, false);
        tui_draw();  // Draw initial screen
    }

    // threading for recieivng messages
    pthread_t recieve_thread;
    // creates thread with recieve messages func
    pthread_create(&recieve_thread, NULL, receive_messages_thread, NULL);
    
    if (settings.tui_mode) {
        // Character-by-character input loop
        while(settings.running) {
            struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
            int ret = poll(&pfd, 1, 100);  // 100ms timeout
            
            if (ret > 0 && (pfd.revents & POLLIN)) {
                char c;
                if (read(STDIN_FILENO, &c, 1) == 1) {
                    if (c == '\n' || c == '\r') {  // Enter key
                        if (input_pos > 0) {
                            input_buffer[input_pos] = '\0';
                            
                            // Validate message
                            size_t len = input_pos;
                            bool valid_message = true;
                            
                            if (len < 1 || len > 1023) {
                                valid_message = false;
                            } else {
                                for(size_t i = 0; i < len; i++) {
                                    if(!isprint((unsigned char)input_buffer[i]) || input_buffer[i] == '\n') {
                                        valid_message = false;
                                        break;
                                    }
                                }
                            }
                            
                            if (valid_message) {
                                // Send message
                                message_t send;
                                memset(&send, 0, sizeof(send));
                                strncpy(send.username, settings.username, sizeof(send.username) - 1);
                                send.type = htonl(MESSAGE_SEND);
                                strncpy(send.message, input_buffer, len);
                                time_t seconds = time(NULL);
                                send.timestamp = htonl((uint32_t)seconds);
                                
                                ssize_t written = write(settings.sockfd, &send, sizeof(send));
                                if (written == -1) {
                                    print_error("Error: Write Error");
                                    close(settings.sockfd);
                                    break;
                                }
                                
                                // Clear input buffer for next message
                                input_pos = 0;
                                input_buffer[0] = '\0';
                                tui_draw();
                            } else {
                                // Invalid message - clear input
                                input_pos = 0;
                                input_buffer[0] = '\0';
                                tui_draw();
                            }
                        }
                    } else if (c == 127 || c == 8) {  // Backspace
                        if (input_pos > 0) {
                            input_pos--;
                            input_buffer[input_pos] = '\0';
                            tui_draw();
                        }
                    } else if (c == 3) {  // Ctrl+C
                        shutdown_initiated = 1;
                        break;
                    } else if (c == 4) {  // Ctrl+D (EOF)
                        printf("\n");
                        shutdown_initiated = 1;
                        break;
                    } else if (isprint(c) && input_pos < sizeof(input_buffer) - 1) {
                        input_buffer[input_pos] = c;
                        input_pos++;
                        input_buffer[input_pos] = '\0';
                        tui_draw();
                    }
                }
            } else if (ret < 0) {
                // Error in poll
                if (errno == EINTR && shutdown_initiated) {
                    break;
                }
            }
            
            // Check if shutdown was initiated by DISCONNECT or other means
            if (shutdown_initiated && !settings.running) {
                break;
            }
        }
        
        // Cleanup - now handles DISCONNECT, SIGINT, and EOF the same way
        disable_raw_mode();
        tui_clear();
        
        // Send logout if we initiated the shutdown (not from DISCONNECT)
        if (shutdown_initiated && settings.running) {
            send_logout();
        }
        
        printf("Disconnected from mycord server.\n");
    } else {
        // Original STDIN/STDOUT mode - FIXED VERSION
        char* line = NULL;
        size_t len = 0;
        ssize_t read_chars;

        // Set stdin to non-blocking for better signal handling
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

       // while active connection (running)
       while(settings.running)
       {
            // Check for shutdown initiated by signal or disconnect
            if (shutdown_initiated) {
                // Clean exit path
                if (line) {
                    free(line);
                    line = NULL;
                }
                break;
            }
            
            // Use poll to check if stdin has data (with timeout)
            struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
            int ret = poll(&pfd, 1, 100); // 100ms timeout
            
            if (ret > 0 && (pfd.revents & POLLIN)) {
                // read a line from STDIN
                read_chars = getline(&line, &len, stdin);
                
                // Handle EOF (Ctrl+D)
                if (read_chars == -1 && feof(stdin)) {
                    printf("\n"); // Newline after EOF
                    shutdown_initiated = 1;
                    if (line) {
                        free(line);
                        line = NULL;
                    }
                    break;
                }
                
                // Handle other getline errors
                if (read_chars == -1) {
                    // EINTR means interrupted by signal
                    if (errno == EINTR) {
                        if (shutdown_initiated) {
                            if (line) {
                                free(line);
                                line = NULL;
                            }
                            break;
                        }
                        continue;
                    } else {
                        print_error("Error: Getline() error");
                        if (line) {
                            free(line);
                        }
                        exit(17);
                    }
                }
                
                // strips newlines
                if (read_chars > 0 && line[read_chars-1] == '\n') {
                    line[read_chars-1] = '\0';
                    read_chars -= 1;
                }
                
                // check messages length
                if (read_chars < 1 || read_chars > 1023) {
                    print_error("Error: Invalid Message");
                    continue;
                }
                
                // checks ascii and newline
                bool valid_message = true;
                for (int i = 0; i < read_chars; i++) {
                    if (!isprint((unsigned char)line[i]) || line[i] == '\n') {
                        print_error("Error: Invalid Message");
                        valid_message = false;
                        break;
                    }
                }

                // Skips send if message was invalid
                if (!valid_message) {
                    continue;
                }
                
                // makes message struct for message to send
                message_t send;
                // 0s memory
                memset(&send, 0, sizeof(send));
                // copies username
                strncpy(send.username, settings.username, sizeof(send.username) - 1);
                send.username[sizeof(login.username) - 1] = '\0';
                // provides type
                send.type = htonl(MESSAGE_SEND);
                //provides message
                strncpy(send.message, line, read_chars);
                // time stamp
                time_t seconds;
                seconds = time(NULL);
                send.timestamp = htonl((uint32_t)seconds);	
                // writes message over socket
                ssize_t written = write(settings.sockfd, &send, sizeof(send));
                // error catch
                if (written == -1) {
                    print_error("Error: Write Error");
                    close(settings.sockfd);
                    break;
                }
            } else if (ret < 0) {
                // Poll error - check if it's EINTR (signal)
                if (errno == EINTR && shutdown_initiated) {
                    if (line) {
                        free(line);
                        line = NULL;
                    }
                    break;
                }
            }
            
            // Check if we need to shutdown
            if (shutdown_initiated) {
                if (line) {
                    free(line);
                    line = NULL;
                }
                break;
            }
       }
       
        // Restore stdin flags
        fcntl(STDIN_FILENO, F_SETFL, flags);
        
        if (line) {
            free(line);
            line = NULL;
        }
    }
    
    // Send logout if still connected (for non-TUI mode)
    if (settings.running && !shutdown_initiated) {
        send_logout();
    }
    
    // Set running to false to ensure receive thread exits
    settings.running = false;
    
    // Shutdown socket to wake up receive thread if it's blocking on read
    if (settings.sockfd >= 0) {
        shutdown(settings.sockfd, SHUT_RDWR);
    }
    
    // wait for the thread / clean up
    void* ret;
    pthread_join(recieve_thread, &ret);
    
    // cleanup and return
    if (settings.sockfd >= 0) {
        close(settings.sockfd);
    }
    return 0;  
}
