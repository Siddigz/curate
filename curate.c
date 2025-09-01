#include <unistd.h>
#include <stdio.h>
#include <termios.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/ioctl.h>
// #define CTRL(c) (c & 31)

struct termConfig{
    int rows;
    int cols;
    struct termios OGTerminal;
};

struct termConfig editor;

void exitP(const char* desc){
    //Clears screen, repositions cursor, exits with error
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(desc);
    exit(1);
}

void disableRaw(){
    //Reverts the terminal config changes
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &editor.OGTerminal) != 0){
        exitP("disableRaw: tcsetattr");
    }
}

void enableRaw(){
    //Copies current terminal configurations to OGTerminal
    if(tcgetattr(STDIN_FILENO, &editor.OGTerminal) != 0){
        exitP("enableRaw: tcgetattr");
    }
    //Reverts all changes made to terminal when exiting program
    atexit(disableRaw);

    struct termios termAttr = editor.OGTerminal;

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

int getPointerPos(int* rows, int* cols){
    char buffer[32];
    unsigned int i = 0;

    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4){
        return -1;
    }

    while(i < sizeof(buffer)-1){
        if(read(STDIN_FILENO, &buffer[i], 1) != 1){
            break;
        }

        if(buffer[i] == 'R'){
            break;
        }
    }
    
    buffer[i] = '\0';
    if(buffer[0] != '\x1b' || buffer[1] != '['){
        return -1;
    }
    if(sscanf(&buffer[2], "%d;%d", rows,cols) !=2){
        return -1;
    }

    return 0;
}

void letterProcessing(){
    char letter = keyReading();

    switch(letter){
        //Exits program on Ctrl+Q
        case CTRL('q'): 
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
    }
}   

void borderDrawing(){
    int row;

    for(row = 0; row < editor.rows; row++){
        write(STDOUT_FILENO, "~", 1);
    
        if(row < editor.rows -1){
            write(STDOUT_FILENO, "\r\n", 2);    
        }
    }
}

void refresh(){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    borderDrawing();
    write(STDOUT_FILENO, "\x1b[H", 3);
    
}

int getWindowSize(int* rows , int* cols){
    struct winsize window;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == -1 || window.ws_col == 0 || window.ws_row == 0){
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12){
            return -1;
        }else{
            getPointerPos(&editor.rows, &editor.cols);
        }
        return -1;
    }else{
        *rows = window.ws_row;
        *cols = window.ws_col;
        return 0;
    }
}

void initEditor(){
    if(getWindowSize(&editor.rows, &editor.cols) == -1){
        exitP("initEditor: getWindowSize");
    }
}

struct appendBuffer{
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct appendBuffer *ab, const char* string, int len){
    char* new = realloc(ab->b, ab->len + len);
    if(new == NULL){
        return;
    }
    memcpy(&new[ab->len], string, len);
    ab->b = new;
    ab->len != len;
}

void abFree(struct appendBuffer *ab){
    free(ab->b);
}


int main(){

    //Enables custom raw processing mode
    enableRaw();
    initEditor();
    refresh();

    while(1){
        letterProcessing();
    }

    return 0;
}