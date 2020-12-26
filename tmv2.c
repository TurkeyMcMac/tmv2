#include <curses.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <term.h>
#include <unistd.h>

// Maximum minimum delay between frames, measured in milliseconds.
#define MAX_DELAY_MS 2100000UL

// Maximum input line length before characters stop being recorded.
#define MAX_LINE_LEN 20000

static void print_version(const char *progname)
{
	printf("%s version 0.1.0\n", progname);
}

static void print_help(const char *progname)
{
	printf("\
Usage: %s [options] [--] [input-file]\n\
Options:\n\
 -d delay  Delay a minimum of the given number of milliseconds between\n\
           frames. The program may wait longer if it takes too long to read\n\
	   or write a frame. The maximum delay is %lu.\n\
 -l        Loop the animation. This requires that the input file be seekable.\n\
           (This usually excludes stdin.)\n\
 -L        Do not loop. This is the default.\n\
 -h        Print this help.\n\
 -v        Print program version\n\
If the input file is not given or the string is '-', stdin is read from to\n\
show the movie. Otherwise, the movie is read from the named file. Only one\n\
input argument can be given, at most.\n\
", progname, MAX_DELAY_MS);
}

// Like getline(3) but without the NUL or newline. May have to check feof(3).
static ssize_t get_line(char **buf, size_t *cap, FILE *from)
{
#define INITIAL_LINE_CAP 20
	size_t len = 0;
	int ch;
	while ((ch = fgetc(from)) != '\n' && ch != EOF) {
		if (len >= *cap) {
			// Expand by a factor of about 1.5:
			*cap = INITIAL_LINE_CAP + len + len / 2;
			*buf = realloc(*buf, *cap);
			if (!*buf) goto error;
		}
		// Only record characters before MAX_LINE_MAX:
		if (len < MAX_LINE_LEN) (*buf)[len++] = ch;
	}
	if (ferror(from)) goto error;
	return (ssize_t)len;

error:
	free(*buf);
	// Keep the parameters in a valid state:
	*cap = 0;
	*buf = NULL;
	return -1;
}

// Sets tv to a duration in seconds and microseconds equal to the given duration
// measured in just microseconds.
static void set_usec(struct timeval *tv, unsigned long usec)
{
	tv->tv_sec = (time_t)(usec / 1000000);
	tv->tv_usec = (suseconds_t)(usec % 1000000);
}

// Erases the last n_lines lines including the current line and moves up to the
// highest erased line.
static void move_cursor_upward_and_clear(long n_lines)
{
	if (n_lines > 0) {
		// Make sure to clear entire lines:
		putchar('\r');
		// Clear n_lines lines including the current line:
		putp(clr_eol);
		while (--n_lines > 0) {
			putp(cursor_up);
			putp(clr_eol);
		}
	}
}

// Turns the cursor back to normal and exits with the status.
static void restore_and_exit(int status)
{
	if (cursor_normal) putp(cursor_normal);
	fflush(stdout); // (Just in case.)
	exit(status);
}

// signal_terminated is switched on when this handler is called.
static volatile sig_atomic_t signal_terminated = false;
static void termination_handler(int signum)
{
	signal_terminated = true;
	return (void)signum; // (This argument is unused.)
}

int main(int argc, char *argv[])
{
	// "tmv2" is the default name, since argv[0] may be NULL:
	const char *progname = argv[0] ? argv[0] : "tmv2";
	// Set up the terminfo library or die:
	if (setupterm(NULL, STDOUT_FILENO, NULL) == ERR) {
		fprintf(stderr, "%s: setupterm() failed\n", progname);
		exit(EXIT_FAILURE);
	}
	// Ensure required capabilities are present:
	if (!clr_eol || !cursor_up) {
		fprintf(stderr, "%s: Need el and cuu1 terminfo capabilities\n",
			progname);
		exit(EXIT_FAILURE);
	}
	// Parse command-line arguments:
	unsigned long delay_ms = 100;
	bool loop = false;
	const char *movie_filename = "-"; // Use stdin ("-") by default.
	int opt;
	while ((opt = getopt(argc, argv, "d:lLhv")) != -1) {
		switch (opt) {
			char *end;
		case 'd':
			delay_ms = strtoul(optarg, &end, 10);
			if (*end != '\0' || delay_ms > MAX_DELAY_MS) {
				fprintf(stderr,
					"%s: -d requires an integer argument "
					"between 0 and %lu (inclusive)\n",
					progname, MAX_DELAY_MS);
				exit(EXIT_FAILURE);
			}
			break;
		case 'l':
			loop = true;
			break;
		case 'L':
			loop = false;
			break;
		case 'h':
			print_help(progname);
			exit(EXIT_SUCCESS);
		case 'v':
			print_version(progname);
			exit(EXIT_SUCCESS);
		default:
			fprintf(stderr, "Run '%s -h' for more help.\n",
				progname);
			exit(EXIT_FAILURE);
		}
	}
	if (argv[optind]) {
		if (argv[optind + 1]) {
			fprintf(stderr, "%s: Excess argument(s) after %s\n",
				progname, argv[optind]);
			exit(EXIT_FAILURE);
		}
		// Set movie filename if given; "-" still means stdin:
		movie_filename = argv[optind];
	}
	// Open movie file or use stdin:
	FILE *movie = strcmp(movie_filename, "-") != 0 ?
		fopen(movie_filename, "r") : stdin;
	if (!movie) {
		fprintf(stderr, "%s: Cannot open file %s: %s\n",
			progname, movie_filename, strerror(errno));
		exit(EXIT_FAILURE);
	}
	// Looping requires seeking to the beginning after each iteration:
	if (loop && fseek(movie, 0, SEEK_SET) < 0) {
		fprintf(stderr, "%s: -l requires that the input be seekable\n",
			progname);
		exit(EXIT_FAILURE);
	}
	// Make sure stdout is fully buffered to help prevent partial frames:
	setvbuf(stdout, NULL, _IOFBF, 0);
	// Hide the cursor if the terminal supports doing so and changing back:
	if (cursor_invisible && cursor_normal)
		putp(cursor_invisible);
	// Catch common termination signals:
	struct sigaction sa = { 0 };
	sa.sa_handler = termination_handler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	// Block SIGALRM; it's caught using sigwait(3).
	sigset_t alrm_set;
	sigemptyset(&alrm_set);
	sigaddset(&alrm_set, SIGALRM);
	sigprocmask(SIG_BLOCK, &alrm_set, NULL);
	// Set up the interval timer:
	// Always have a bit of delay so as not to deactivate the itimer:
	unsigned long delay_us = delay_ms > 0 ? delay_ms * 1000 : 1;
	struct itimerval timerval;
	set_usec(&timerval.it_interval, delay_us);
	set_usec(&timerval.it_value, delay_us);
	setitimer(ITIMER_REAL, &timerval, NULL);
	// frame_height is the number of lines in the printing frame:
	long frame_height = 0;
	do {
		// frame_height may be > 0 after the first iteration:
		move_cursor_upward_and_clear(frame_height);
		// Reset frame_height to accumulate with the new frame:
		frame_height = 0;
		// Parse the separator as the first line:
		char *sep = NULL;
		size_t sep_cap = 0;
		ssize_t sep_len = get_line(&sep, &sep_cap, movie);
		// If the read was interrupted, don't register an error:
		if (signal_terminated) restore_and_exit(EXIT_SUCCESS);
		if (sep_len < 0) {
			fprintf(stderr, "%s: Failed to read separator: %s\n",
				progname, strerror(errno));
			restore_and_exit(EXIT_SUCCESS);
		}
		// The line memory is shared between frame line iterations:
		char *line = NULL;
		size_t line_cap = 0;
		for (;;) {
			// Read the line:
			ssize_t line_len = get_line(&line, &line_cap, movie);
			// If the read was interrupted, don't register an error:
			if (signal_terminated) restore_and_exit(EXIT_SUCCESS);
			if (line_len < 0) {
				fprintf(stderr, "%s: Failed to read line: %s\n",
					progname, strerror(errno));
				restore_and_exit(EXIT_SUCCESS);
			}
			if (line_len == 0 && feof(movie)) {
				// This means no line was read since the end of
				// the file is here.
				// Make sure the frame is printed:
				fflush(stdout);
				// Delay to complete the frame:
				int sig;
				sigwait(&alrm_set, &sig);
				// The movie's over, at least this loop:
				break;
			} else if (line_len == sep_len
			        && memcmp(line, sep, sep_len) == 0)
			{
				// This means a separator was found.
				// Make sure the finished frame is printed:
				fflush(stdout);
				// Delay to complete the frame:
				int sig;
				sigwait(&alrm_set, &sig);
				// Clear and prepare for the next frame:
				move_cursor_upward_and_clear(frame_height);
				frame_height = 0;
			} else {
				// Move down for following lines:
				if (frame_height > 0) fputc('\n', stdout);
				// This means there's another line to draw.
				// Make sure to start at the terminal's start:
				fputc('\r', stdout);
				// Print the line:
				fwrite(line, 1, line_len, stdout);
				// Accumulate the height, ignoring stuff over
				// INT_MAX, which is huge:
				if (frame_height < INT_MAX) ++frame_height;
			}
		}
		free(line);
		free(sep);

		// If rewinding is not possible, it doesn't matter since the
		// code will then not loop anyway:
		rewind(movie);
	} while (loop); // Read the file again if requested.
	restore_and_exit(EXIT_SUCCESS);
}
