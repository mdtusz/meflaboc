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

#define MEF_VERSION "0.0.1"

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT {NULL, 0}

typedef struct editorRow {
    int size;
    char *chars;
} editorRow;

struct editorConfig {
    int numRows;
    editorRow *row;
    int rowOffset;
    int colOffset;
    int cx;
    int cy;
    int screenRows;
    int screenCols;
    struct termios original_termios;
};

struct editorConfig E;

struct abuf {
    char *buf;
    int len;
};

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN
};

void abufAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->buf, ab->len + len);

    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->len], s, len);
    ab->buf = new;
    ab->len += len;
}

void abufFree(struct abuf *ab) {
    free(ab->buf);
}

void editorClearScreen(struct abuf *ab) {
    abufAppend(ab, "\x1b[2J", 4);
    abufAppend(ab, "\x1b[H", 3);
}

void die(const char *s) {
    struct abuf ab = ABUF_INIT;

    editorClearScreen(&ab);
    write(STDOUT_FILENO, ab.buf, ab.len);
    abufFree(&ab);

    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.original_termios) == -1) {
        die("tcgetattr");
    }

    // Turn off raw mode at exit so the terminal behaves normally again.
    atexit(disableRawMode);

    struct termios raw = E.original_termios;

    // Disable some terminal settings that are undesirable.
    //
    // ECHO -> character input is not echoed.
    // ICANON -> canonical mode is off (i.e. chars will be read immediately).
    // ISIG -> Signals (Ctrl-C and Ctrl-Z) disabled.
    // IEXTEN -> Ctrl-V disabled.
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);

    // IXON -> Software control flow (Ctrl-S and Ctrl-Q) disabled.
    // ICRNL -> Disable CRNL where \r converted to \n.
    // BRKINT -> Disables break conditions.
    // INPCK -> Disables parity checking.A
    // ISTRIP -> Disables bit stripping (?).
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK);

    // OPOST -> Disable output post-processing (i.e. \n -> \r\n).
    raw.c_oflag &= ~(OPOST);

    // Set character size to 8 bits per byte.
    raw.c_cflag |= (CS8);

    raw.c_cc[VMIN] = 0;

    // Set max time before `read` will timeout and return 0.
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

int editorReadKey() {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    // If an escape sequence is detected, check for special keys (3 bytes).
    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1 || read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                };

                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                }
            }
        }
    }


    return c;
}

int getCursorPosition(int *rows, int *cols) {
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    char buf[32];
    unsigned int i = 0;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) == -1 || buf[i] == 'R') {
            break;
        }

        i++;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[' || sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenRows; y++) {
        int fileRow = y + E.rowOffset;

        if (fileRow >= E.numRows) {
            if (E.numRows == 0 && y == E.screenRows - 1) {
                char welcome[E.screenCols];
                int welcomeLen = snprintf(welcome, sizeof(welcome), ":Mef -- V%s", MEF_VERSION);

                if (welcomeLen > E.screenCols) {
                    welcomeLen = E.screenCols;
                }

                abufAppend(ab, welcome, welcomeLen);
            } else {
                abufAppend(ab, "~", 1);
            }
        } else {
            int len = min(E.screenCols, max(0, E.row[fileRow].size - E.colOffset));
            abufAppend(ab, &E.row[fileRow].chars[E.colOffset], len);
        }

        abufAppend(ab, "\x1b[k", 3);

        if (y < E.screenRows - 1) {
            abufAppend(ab, "\r\n", 2);
        }
    }
}

void editorHideCursor(struct abuf *ab) {
    abufAppend(ab, "\x1b[?25l", 6);
}

void editorShowCursor(struct abuf *ab) {
    abufAppend(ab, "\x1b[?25h", 6);
}

void editorScroll() {
    if (E.cy < E.rowOffset) {
        E.rowOffset = E.cy;
    }

    if (E.cy >= E.rowOffset + E.screenRows) {
        E.rowOffset = E.cy - E.screenRows + 1;
    }

    if (E.cx < E.colOffset) {
        E.colOffset = E.cx;
    }

    if (E.cx >= E.colOffset + E.screenCols) {
        E.colOffset = E.cx - E.screenCols + 1;
    }
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    editorHideCursor(&ab);

    editorClearScreen(&ab);
    editorDrawRows(&ab);

    // Move cursor to position.
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowOffset + 1, E.cx - E.colOffset + 1);
    abufAppend(&ab, buf, strlen(buf));

    editorShowCursor(&ab);

    write(STDOUT_FILENO, ab.buf, ab.len);
    abufFree(&ab);
}

void editorMoveCursor(int key) {
    editorRow *row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
        case 'h':
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0 ){
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
        case 'l':
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
        case 'k':
            E.cy = max(0, E.cy - 1);
            break;
        case ARROW_DOWN:
        case 'j':
            E.cy = min(E.numRows, E.cy + 1);
            break;
        case PAGE_UP:
            E.cy = 0;
            break;
        case PAGE_DOWN:
            E.cy = E.screenRows - 1;
            break;
        case 'A':
            if (row) {
                E.cx = row->size;
            }
            break;
        case 'I':
            if (row) {
                E.cx = 0;
            }
            break;

    }

    row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];
    int rowLen = row ? row->size : 0;

    if (E.cx > rowLen) {
        E.cx = rowLen;
    }
}

void editorProcessKey() {
    int key = editorReadKey();
    struct abuf ab = ABUF_INIT;

    switch (key) {
        case CTRL_KEY('q'):
            editorClearScreen(&ab);
            write(STDOUT_FILENO, ab.buf, ab.len);
            abufFree(&ab);
            exit(0);
            break;
        case ':':
            exit(0);
            break;
        default:
            editorMoveCursor(key);
            break;
    }
}

void initEditor() {
    if (getWindowSize(&E.screenRows, &E.screenCols) == -1) {
        die("getWindowSize");
    }

    E.numRows = 0;
    E.row = NULL;
    E.rowOffset = 0;
    E.colOffset = 0;
    E.cx = 0;
    E.cy = 0;
}

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(editorRow) * (E.numRows + 1));

    int at = E.numRows;

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);

    memcpy(E.row[at].chars, s, len);

    E.row[at].chars[len] = '\0';
    E.numRows++;
}

void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");

    if (!fp) {
        die("fopen");
    }

    char *line = NULL;
    size_t lineCap = 0;

    ssize_t lineLen;

    while ((lineLen = getline(&line, &lineCap, fp)) != -1) {
        while (lineLen > 0 && (line[lineLen - 1] == '\n' || line[lineLen - 1] == '\r')) {
            lineLen--;
        }

        editorAppendRow(line, lineLen);
    }

    free(line);
    fclose(fp);
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();

    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKey();
    }

    return 0;
}
