#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<time.h>
#include<unistd.h>
#include<signal.h>
#include<errno.h>
#ifndef NO_X
#include<X11/Xlib.h>
#endif
#define LENGTH(X)               (sizeof(X) / sizeof (X[0]))
#define CMDLENGTH		50

typedef struct {
	char* icon;
	char* command;
	unsigned int interval;
	unsigned int signal;
} Block;
void buttonhandler(int sig, siginfo_t *si, void *ucontext);
void dummysighandler(int num);
void sighandler(int num);
void getcmds(int time);
void getsigcmds(unsigned int signal);
void setupsignals();
void sighandler(int signum);
int getstatus(char *str, char *last);
void remove_all(char *str, char to_remove);
void statusloop();
void termhandler();
void pstdout();
#ifndef NO_X
void setroot();
static void (*writestatus) () = setroot;
static int setupX();
static Display *dpy;
static int screen;
static Window root;
#else
static void (*writestatus) () = pstdout;
#endif


#include "blocks.h"

static char statusbar[LENGTH(blocks)][CMDLENGTH] = {0};
static char statusstr[2][256];
static int statusContinue = 1;

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

void buttonhandler(int sig, siginfo_t *si, void *ucontext)
{
	char button[2] = {('0' + si->si_value.sival_int) & 0xff, '\0'};
	pid_t process_id = getpid();
	sig = si->si_value.sival_int >> 8;
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
	struct sigaction sa;

    for (int i = SIGRTMIN; i <= SIGRTMAX; i++)
		signal(i, SIG_IGN);

	for (unsigned int i = 0; i < LENGTH(blocks); i++) {
		if (blocks[i].signal > 0) {
			signal(SIGRTMIN+blocks[i].signal, sighandler);
			sigaddset(&sa.sa_mask, SIGRTMIN+blocks[i].signal);
		}
	}
	sa.sa_sigaction = buttonhandler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &sa, NULL);
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
	unsigned int interval = -1;
	for (int i = 0; i < LENGTH(blocks); i++) {
		if (blocks[i].interval) {
			interval = gcd(blocks[i].interval, interval);
		}
	}

	unsigned int i = 0;
	int interrupted = 0;
	const struct timespec sleeptime = {interval, 0};
	struct timespec tosleep = sleeptime;
	getcmds(-1);
	while (statusContinue) {
		interrupted = nanosleep(&tosleep, &tosleep);
		if (interrupted == -1) {
			continue;
		}
		getcmds(i);
		writestatus();
		i += interval;
		tosleep = sleeptime;
	}
}

/* this signal handler should do nothing */
void dummysighandler(int signum)
{
    return;
}

void sighandler(int signum)
{
	getsigcmds(signum-SIGRTMIN);
	writestatus();
}

void termhandler()
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
#ifndef NO_X
	if (!setupX())
		return 1;
#endif
	signal(SIGTERM, termhandler);
	signal(SIGINT, termhandler);
	statusloop();
#ifndef NO_X
	XCloseDisplay(dpy);
#endif
	return 0;
}
