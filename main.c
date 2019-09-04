#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define MEF_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT {NULL, 0}


struct editorConfig {
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

char editorReadKey() {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
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
    int y;
    for (y = 0; y < E.screenRows; y++) {
        if (y == E.screenRows - 1) {
            char welcome[E.screenCols];
            int welcomeLen = snprintf(welcome, sizeof(welcome), ":Mef -- V%s", MEF_VERSION);

            if (welcomeLen > E.screenCols) {
                welcomeLen = E.screenCols;
            }

            abufAppend(ab, welcome, welcomeLen);
        } else {
            abufAppend(ab, "~", 1);
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

void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    editorHideCursor(&ab);

    editorClearScreen(&ab);
    editorDrawRows(&ab);

    // Move cursor to position.
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abufAppend(&ab, buf, strlen(buf));

    editorShowCursor(&ab);

    write(STDOUT_FILENO, ab.buf, ab.len);
    abufFree(&ab);
}

void editorProcessKey() {
    char c = editorReadKey();
    struct abuf ab = ABUF_INIT;

    switch (c) {
        case CTRL_KEY('q'):
            editorClearScreen(&ab);
            write(STDOUT_FILENO, ab.buf, ab.len);
            abufFree(&ab);
            exit(0);
            break;
    }
}

void initEditor() {
    if (getWindowSize(&E.screenRows, &E.screenCols) == -1) {
        die("getWindowSize");
    }

    E.cx = 0;
    E.cy = 0;
}

int main() {
    enableRawMode();
    initEditor();

    while (1) {
        editorRefreshScreen();
        editorProcessKey();
    }

    return 0;
}
