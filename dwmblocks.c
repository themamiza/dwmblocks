#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <sys/signalfd.h>
#include <poll.h>
#define LENGTH(X) (sizeof(X) / sizeof (X[0]))
#define CMDLENGTH 50

typedef struct {
	char* icon;
	char* command;
	unsigned int interval;
	unsigned int signal;
} Block;
void buttonhandler(int ssi_int);
void dummysighandler(int num);
void sighandler();
void getcmds(int time);
void getsigcmds(unsigned int signal);
void setupsignals();
int getstatus(char *str, char *last);
void remove_all(char *str, char to_remove);
void statusloop();
void termhandler(int signum);
void pstdout();
void setroot();
static void (*writestatus) () = setroot;
static int setupX();
static Display *dpy;
static int screen;
static Window root;

#include "blocks.h"

static char statusbar[LENGTH(blocks)][CMDLENGTH] = {0};
static char statusstr[2][256];
static int statusContinue = 1;
static int signalFD;
static int timerInterval = -1;

int gcd(int a, int b)
{
	int temp;
	while (b > 0) {
		temp = a % b;
		a = b;
		b = temp;
	}
	return a;
}

void buttonhandler(int ssi_int)
{
	char button[2] = {('0' + ssi_int) & 0xff, '\0'};
	pid_t process_id = getpid();
	int sig = ssi_int >> 8;
	if (fork() == 0) {
		const Block *current;
		for (int i = 0; i < LENGTH(blocks); i++) {
			current = blocks + i;
			if (current->signal == sig)
				break;
		}
		char shcmd[1024];
		sprintf(shcmd, "%s && kill -%d %d", current->command, current->signal+34, process_id);
		char *command[] = { "/bin/sh", "-c", shcmd, NULL };
		setenv("BLOCK_BUTTON", button, 1);
		setsid();
		execvp(command[0], command);
		exit(EXIT_SUCCESS);
	}
}

//opens process *cmd and stores output in *output
void getcmd(const Block *block, char *output)
{
	if (block->signal) {
		output[0] = block->signal;
		output++;
	}
	char *cmd = block->command;
	FILE *cmdf = popen(cmd, "r");
	if (!cmdf)
		return;

	char tmpstr[CMDLENGTH] = "";

	char * s;
	int e;

	do {
		errno = 0;
		s = fgets(tmpstr, CMDLENGTH - (strlen(delim) + 1), cmdf);
		e = errno;
	} while (!s && e == EINTR);
	pclose(cmdf);
	int i = strlen(block->icon);
	strcpy(output+i, tmpstr);
	remove_all(output, '\n');
	i = strlen(output);
	if ((i > 0 && block != &block[LENGTH(blocks) - 1])) {
		strcat(output, delim);
	}
	i+=strlen(delim);
	output[i++] = '\0';
}

void getcmds(int time)
{
	const Block* current;
	for (unsigned int i = 0; i < LENGTH(blocks); i++) {
		current = blocks + i;
		if ((current->interval != 0 && time % current->interval == 0) || time == -1)
			getcmd(current,statusbar[i]);
	}
}

void getsigcmds(unsigned int signal)
{
	const Block *current;
	for (unsigned int i = 0; i < LENGTH(blocks); i++) {
		current = blocks + i;
		if (current->signal == signal)
			getcmd(current,statusbar[i]);
	}
}

void setupsignals()
{
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGALRM); // Timer events
	sigaddset(&signals, SIGUSR1); // Button events
	// All signals assigned to blocks
	for (size_t i = 0; i < LENGTH(blocks); i++)
		if (blocks[i].signal > 0)
			sigaddset(&signals, SIGRTMIN + blocks[i].signal);
	// Create signal file descriptor for pooling
	signalFD = signalfd(-1, &signals, 0);
	// Block all real-time signals
	for (int i = SIGRTMIN; i <= SIGRTMAX; i++) sigaddset(&signals, i);
	sigprocmask(SIG_BLOCK, &signals, NULL);
	// Do not transform children into zombies
	struct sigaction sigchld_action = {
		.sa_handler = SIG_DFL,
		.sa_flags = SA_NOCLDWAIT
	};
	sigaction(SIGCHLD, &sigchld_action, NULL);
}

int getstatus(char *str, char *last)
{
	strcpy(last, str);
	str[0] = '\0';
	for (unsigned int i = 0; i < LENGTH(blocks); i++) {
		strcat(str, statusbar[i]);
		if (i == LENGTH(blocks) - 1)
			strcat(str, " ");
	}
	str[strlen(str)-1] = '\0';
	return strcmp(str, last);//0 if they are the same
}

void remove_all(char *str, char to_remove)
{
	char *read = str;
	char *write = str;
	while (*read) {
		if (*read != to_remove) {
			*write++ = *read;
		}
		++read;
	}
	*write = '\0';
}

#ifndef NO_X
void setroot()
{
	if (!getstatus(statusstr[0], statusstr[1]))//Only set root if text has changed.
		return;
	XStoreName(dpy, root, statusstr[0]);
	XFlush(dpy);
}

int setupX()
{
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "dwmblocks: Failed to open display\n");
		return 0;
	}
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	return 1;
}
#endif

void pstdout()
{
	if (!getstatus(statusstr[0], statusstr[1]))//Only write out if text has changed.
		return;
	printf("%s\n",statusstr[0]);
	fflush(stdout);
}


void statusloop()
{
	setupsignals();
	for (int i = 0; i < LENGTH(blocks); i++)
		if (blocks[i].interval)
			timerInterval = gcd(blocks[i].interval, timerInterval);

	getcmds(-1);		// First time run all commands
	raise(SIGALRM);		// Schedule first timer event
	int ret;
	struct pollfd pfd[] = {{.fd = signalFD, .events = POLLIN}};
	while (statusContinue) {
		// Wait for new signal
		ret = poll(pfd, sizeof(pfd) / sizeof(pfd[0]), -1);
		if (ret < 0 || !(pfd[0].revents & POLLIN)) break;
		sighandler(); // Handle signal
	}
}

/* this signal handler should do nothing */
void dummysighandler(int signum)
{
    return;
}

void sighandler()
{
	static int time = 0;
	struct signalfd_siginfo si;
	int ret = read(signalFD, &si, sizeof(si));
	if (ret < 0) return;
	int signal = si.ssi_signo;
	switch (signal) {
		case SIGALRM:
			// Execute blocks and schedule the next timer event
			getcmds(time);
			alarm(timerInterval);
			time += timerInterval;
			break;
		case SIGUSR1:
			// Handle buttons
			buttonhandler(si.ssi_int);
			return;
		default:
			// Execute the block that has the given signal
			getsigcmds(signal - SIGRTMIN);
			break;
	}
	writestatus();
}

void termhandler(int signum)
{
	statusContinue = 0;
}

int main(int argc, char** argv)
{
	for (int i = 0; i < argc; i++) {//Handle command line arguments
		if (!strcmp("-d",argv[i]))
			delim = argv[++i];
		else if (!strcmp("-p",argv[i]))
			writestatus = pstdout;
	}
	if (!setupX())
		return 1;
	signal(SIGTERM, termhandler);
	signal(SIGINT, termhandler);
	statusloop();
	close(signalFD);
	XCloseDisplay(dpy);
	return 0;
}
