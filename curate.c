#include <unistd.h>
#include <stdio.h>
#include <termios.h>
#include <ctype.h>
#include <stdlib.h>


struct termios OGTerminal;

void disableRaw(){
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &OGTerminal);
}

void enableRaw(){
    tcgetattr(STDIN_FILENO, &OGTerminal);
    atexit(disableRaw);

    struct termios termAttr = OGTerminal;
    termAttr.c_lflag = termAttr.c_lflag & ~(ECHO | ICANON);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &termAttr);
}



int main(){
    enableRaw();

    char letter;
    while(read(STDIN_FILENO, &letter, 1) == 1 && letter != 'q'){
        if(iscntrl(letter)){
            printf("%d\n", letter);
        }else{
            printf("%d ('%c')\n", letter, letter);
        }
    }
    
    return 0;
}