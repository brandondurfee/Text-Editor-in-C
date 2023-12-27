/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/
// represents every row in the text editor - consists of a size and a string
typedef struct erow {
    int size;
    char *chars;
} erow;

// global editor state
struct editorConfig {
    int cx, cy; // for cursor location
    int rowoff;
    int screenrows; // number of screen rows in the editor
    int screencols; // number of screen cols in the editor
    int numrows; // number of erows in the editor (rows of actual text)
    erow *row; // a pointer to an array of erows, representing the text in the editor. Basically, can access some erow using array notation because this is a pointer to the beginning of the rows
    struct termios orig_termios; // struct that deals with the terminal - we will use this to change some console settings to allow us to emulate a text editor
};

// initialize the global editor state as E
struct editorConfig E;


/*** terminal ***/

// end program, clears terminal and prints error message
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4); // helps clear terminal
    write(STDOUT_FILENO, "\x1b[H", 3); // helps clear terminal
    perror(s); // print error message that is passed in
    exit(1);
}

// returns terminal to original state
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) { // sets attr of terminal back to original state
        die("tcsetattr");
    }
}

// turns terminal from canonical mode to raw mode
void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);
    
    // remove lots of flags to prevent CTRL and other escape sequences/special characters in the command line
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // bitwise ors to combine flags
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr"); // sets attributes we just changed                                                                                                    
}

// reads key from the terminal
int editorReadKey() {
    int nread;
    char c;
    
    // store input into char c
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // if c is an escape sequence
    if (c == '\x1b') {
        char seq[3];

        // only escape key is pressed, we can exit
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
   
        // deals with escape sequences like home key, end key, del key, page up, page down, arrow keys, etc
        if (seq[0] == '[') { // logic to tell if it's a special type of key
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch(seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return c; // if not an escape sequence, just return the char
    }

    return c;
}

// gets the cursor position, helps set windowSize for some operating systems, which is why it is called in getWindowSize()
int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    
    return 0;
}

// gets the size of the terminal window, sets the cols and rows
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols); // if we are unable to get the window size using the struct, call getCursorPosition()
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** append buffer ***/

// char buffer with append built in
struct abuf {
    char * b;
    int len; // need a len here to insert at the end of the string
};

//default constructor
#define ABUF_INIT {NULL, 0}

// append method for abuf - reallocs the buffer with extra space len and copies over data
void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len); // realloc with extra space of len

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len); // copy over the string into the new memory block at the end
    ab->b = new; // set the pointer of the abuf to the new mem location so we can access it
    ab->len += len; // increase the len accordingly
}

// destructor for abuf
void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

void editorScroll() {
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
}

// method that draws the editor in the terminal
void editorDrawRows(struct abuf *ab) {
    int y; // y coordinate of drawing

    // when y < the number of screenrows (making sure y is in bounds)
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;

        // if filerow >= number of text rows in the editor (after all the text)
        if (filerow >= E.numrows) {
            
            // if the editor is empty of text and y is at E.screenrows / 3, print the welcome message
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Kilo editor -- version %s", KILO_VERSION);

                // pad welcome message
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            } else { // else when y is after the number of text rows, just put ~ in the beginning of the row
                abAppend(ab, "~", 1);
            }
        } else { // y is in the editor where there is some text / erow
            int len = E.row[filerow].size; // len of the row at y
            if (len > E.screencols) len = E.screencols; // cut off len at screencols if len > screencols
            abAppend(ab, E.row[filerow].chars, len); // print the char buffer at E.row[y]
        }
        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1) { // create a new line for each row
            abAppend(ab, "\r\n", 2);
        }
    }
}

// refresh the screen of the terminal
void editorRefreshScreen() {
    editorScroll();

    // default constructor
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));
    
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

// move cursor based on input arrows
void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            if (E.cx != E.screencols - 1) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
    }
}

// method to process keypress
void editorProcessKeypress() {
    // c is an int because our ENUMs are ints
    int c = editorReadKey();
    
    switch(c) {

        // CTRL(q) quits
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        
        // home key goes to the beginning of the line
        case HOME_KEY:
            E.cx = 0;
            break;
        
        // end key goes to the end of the line
        case END_KEY:
            E.cx = E.screencols - 1;
            break;

        // page up and page down go to the top and bottom of the screen
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screenrows;
                while (times--) {
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

/*** row operations ***/

// appends a row to E.row (which is a pointer to an array of erows)
void editorAppendRow(char *s, size_t len) {
    // reallocate E.row pointer to a block of mem that is sizeof(erow) * (E.numrows + 1), as we are adding a new row that has some text
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    
    int at = E.numrows; // index of new row
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1); // create space for text
    memcpy(E.row[at].chars, s, len); // copy over text into that location in mem
    E.row[at].chars[len] = '\0'; // null terminate
    E.numrows++; // increment num rows to account for new row
}

/*** file i/o ***/

// opens a new file in the text editor
void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");
    
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) { // reads an entire file into E.row
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--; // remove the carriage return and new line from the line via decreasing linelen
        editorAppendRow(line, linelen); // call helper function to copy data from file into the editor
    }
    free(line);
    fclose(fp);


}

/*** init ***/

// initializes the editor
void initEditor() {
    E.cx = 0; // initializes the cursor to position 0,0
    E.cy =  0;
    E.rowoff = 0; // initialize the row offset to 0 so scrolled to the top of the file by default
    E.numrows = 0; // initializes the number of text rows to 0
    E.row = NULL; // no text rows, so pointer is null

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) { // if a file is passed as a cmd line arg, then read from the file
        editorOpen(argv[1]);
    }
    
    while (1) { // runs until CTRL(q) is pressed which quits the text editor
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}