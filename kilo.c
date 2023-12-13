/*** includes ***/
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/

struct termios orig_termios; // store original terminal attributes in orig_termios 

/*** terminal ***/

void die(const char *s) { // for error handling
    perror(s); // print descriptive error message
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode); // register our disableRawMode() function to be called
    // when the program exists
    
    struct termios raw = orig_termios; // make a copy
    raw.c_lflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // disable ctrl-s, ctrl-q, ctrl-m
    raw.c_oflag &= ~(OPOST); // disable output processing
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); // disable ECHO, canonical terminal, ctrl-c, ctrl-z, ctrl-v, ctrl-o
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr"); // set the termios to raw
}

/*** init ***/

int main() {
    enableRawMode();
    while (1) {
        char c = '\0';
        if ((read(STDIN_FILENO, &c, 1) == -1) && errno != EAGAIN) die("read");
        if (iscntrl(c)) {
            printf("%d\r\n", c);
        } else {
            printf("%d ('%c')\r\n", c, c);
        }
        if (c == 'q') break;
    }
}