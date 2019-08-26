#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios original_termios;

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
}

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &original_termios);

    // Turn off raw mode at exit so the terminal behaves normally again.
    atexit(disableRawMode);

    struct termios raw = original_termios;

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

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
    enableRawMode();

    char c;
    while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {
        if (iscntrl(c)) {
            printf("%d\r\n", c);
        } else {
            printf("%d ('%c')\r\n", c, c);
        }
    }

    return 0;
}
