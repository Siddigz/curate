#include <unistd.h>
#include <stdio.h>
#include <termios.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

#define CTRL(c) (c & 31)



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

char keyReading(){
    char letter;
    int readOutput;

    //Reads each letter and returns it
    while((readOutput = read(STDIN_FILENO, &letter, 1)) != -1){
        if(readOutput == -1 && errno != EAGAIN){
            exitP("keyReading: read");
        }else{
            break;
        }
    }

    return letter;
}

void letterProcessing(){
    char letter = keyReading();

    switch(letter){
        case CTRL('q'):
            exit(0);
            break;
    }
}   

void refresh(){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}


int main(){

    //Enables custom raw processing mode
    enableRaw();

    while(1){
        refresh();
        letterProcessing();
    }

    return 0;
}