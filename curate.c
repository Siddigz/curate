#include <stdio.h>
#include <unistd.h>
#include <termios.h>

int main(){
    char letter;
    while(read(STDIN_FILENO, &letter, 1) == 1 && letter != 'q'){

    }


    
    
    return 0;
}