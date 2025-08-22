#include <unistd.h>
#include <stdio.h>
#include <termios.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>


struct termios OGTerminal;

void exitP(const char* desc){
    perror(desc);
    exit(1);
}

void disableRaw(){
    //Reverts the terminal config changes
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &OGTerminal) != 0){
        exitP("disableRaw: tcsetattr");
    }
}

void enableRaw(){
    //Copies current terminal configurations to OGTerminal
    if(tcgetattr(STDIN_FILENO, &OGTerminal) != 0){
        exitP("enableRaw: tcgetattr");
    }
    //Reverts all changes made to terminal when exiting program
    atexit(disableRaw);

    struct termios termAttr = OGTerminal;

    //Flags made to accept key combos like ctrl+m, ctrl+c, ctrl+v, ctrl+q, etc...
    termAttr.c_lflag = termAttr.c_lflag & ~(IEXTEN|ECHO|ICANON|ISIG);
    termAttr.c_iflag = termAttr.c_iflag & ~(BRKINT|ICRNL|ISTRIP|INPCK|IXON);
    termAttr.c_oflag = termAttr.c_oflag & ~(OPOST);
    termAttr.c_cflag = termAttr.c_cflag | CS8;

    termAttr.c_cc[VTIME] = 1;
    termAttr.c_cc[VMIN] = 0;

    //Sets the attributes to the terminal
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &termAttr) != 0){
        exitP("enableRaw: tcsetattr");
    }
}



int main(){

    //Enables custom raw processing mode
    enableRaw();

    while(1){
        char letter = '\0';

        if(read(STDIN_FILENO, &letter, 1) == -1 && errno != EAGAIN){
            exitP("main: read");
        }
        
        if(iscntrl(letter)){
            printf("%d\r\n",letter);    
        }else{
            printf("%d: '%c'\r\n", letter, letter);
        }
        if(letter == 'q'){
            break;
        }
    }
    
    return 0;
}