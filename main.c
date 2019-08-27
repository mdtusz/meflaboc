#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)


struct editorConfig {
    struct termios original_termios;
    int screenRows;
    int screenCols;
};

struct editorConfig E;

void editorClearScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

void die(const char *s) {
    editorClearScreen();
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

void editorDrawRows() {
    int y;
    for (y = 0; y < E.screenRows; y++) {
        write(STDOUT_FILENO, "~", 1);

        if (y < E.screenRows - 1) {
            write(STDOUT_FILENO, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    editorClearScreen();
    editorDrawRows();
}

void editorProcessKey() {
    char c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            editorClearScreen();
            exit(0);
            break;
    }
}

void initEditor() {
    if (getWindowSize(&E.screenRows, &E.screenCols) == -1) {
        die("getWindowSize");
    }
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
