// Feature-test macros so getline(), strdup() and sigaction() are declared under -std=c99 on Linux too
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CURATE_VERSION "0.3.0"
#define TAB_STOP 4
#define QUIT_TIMES 1
#define UNDO_LIMIT 200

#define CTRL_KEY(c) ((c) & 0x1f)

// Keys that arrive as escape sequences get values outside the char range
enum editorKey{
    BACKSPACE = 127,
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

// What each character on screen is, for colouring
enum editorHighlight{
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

// How to highlight one language
struct editorSyntax{
    const char *filetype;
    const char **filematch;             // file extensions
    const char **keywords;              // a trailing '|' marks a type (second colour)
    const char *singlelineCommentStart;
    const char *multilineCommentStart;
    const char *multilineCommentEnd;
    int flags;
};

// One line of the file: what's stored (chars) and what's drawn (render, with tabs expanded)
typedef struct textRow{
    int idx;                // line number
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;      // one highlight value per rendered character
    int hlOpenComment;      // the line ends inside a /* comment */
} textRow;

struct termConfig{
    int cx, cy;         // cursor position in the file (column in chars, row)
    int rx;             // cursor column in the rendered line
    int rowOff;         // first file row shown on screen
    int colOff;         // first rendered column shown on screen
    int rows;           // text rows on screen (terminal rows minus the two bars)
    int cols;
    int numRows;
    textRow *row;
    int dirty;          // unsaved changes
    char *filename;
    char statusMsg[80];
    time_t statusTime;
    struct editorSyntax *syntax;
    struct termios OGTerminal;
};

struct termConfig editor;

// Set by the SIGWINCH handler when the terminal is resized
volatile sig_atomic_t windowChanged = 0;

void setStatusMessage(const char *fmt, ...);
void refresh(void);
void handleResize(void);
char *prompt(const char *message, void (*callback)(char *, int));

/*** filetypes ***/

const char *C_HL_extensions[] = {".c", ".h", ".cpp", ".hpp", ".cc", NULL};
const char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else", "struct", "union",
    "typedef", "static", "enum", "class", "case", "default", "do", "goto", "sizeof", "const",
    "volatile", "extern", "new", "delete", "namespace", "using", "public", "private", "template",
    "#include", "#define", "#ifndef", "#ifdef", "#endif", "#if", "#else",

    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|", "void|", "short|",
    "size_t|", "bool|", "auto|", NULL
};

const char *PY_HL_extensions[] = {".py", NULL};
const char *PY_HL_keywords[] = {
    "def", "class", "return", "if", "elif", "else", "for", "while", "in", "not", "and", "or",
    "import", "from", "as", "try", "except", "finally", "with", "pass", "break", "continue",
    "lambda", "yield", "raise", "global", "is",

    "None|", "True|", "False|", "self|", "int|", "str|", "float|", "list|", "dict|", NULL
};

struct editorSyntax HLDB[] = {
    {"c", C_HL_extensions, C_HL_keywords, "//", "/*", "*/", HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
    {"python", PY_HL_extensions, PY_HL_keywords, "#", "", "", HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** terminal ***/

void exitP(const char* desc){
    //Clears screen, repositions cursor, exits with error
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(desc);
    exit(1);
}

void disableRaw(void){
    //Reverts the terminal config changes
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &editor.OGTerminal) != 0){
        exitP("disableRaw: tcsetattr");
    }
}

void enableRaw(void){
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

int keyReading(void){
    char letter;
    int readOutput;

    //Waits until a key arrives (read returns 0 when the 0.1s timeout passes with no input,
    //and -1 with EINTR when a resize signal interrupts it)
    while((readOutput = read(STDIN_FILENO, &letter, 1)) != 1){
        if(readOutput == -1 && errno != EAGAIN && errno != EINTR){
            exitP("keyReading: read");
        }
        if(windowChanged){
            handleResize();
        }
    }

    if(letter != '\x1b'){
        return letter;
    }

    //Arrow, Home/End, Page Up/Down and Delete keys arrive as escape sequences
    char seq[3];
    if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if(seq[0] == '['){
        if(seq[1] >= '0' && seq[1] <= '9'){
            if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
            if(seq[2] == '~'){
                switch(seq[1]){
                    case '1': return HOME_KEY;
                    case '3': return DEL_KEY;
                    case '4': return END_KEY;
                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;
                    case '7': return HOME_KEY;
                    case '8': return END_KEY;
                }
            }
        }else{
            switch(seq[1]){
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
    }else if(seq[0] == 'O'){
        switch(seq[1]){
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
        }
    }

    return '\x1b';
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
        i++;
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

int getWindowSize(int* rows , int* cols){
    struct winsize window;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == -1 || window.ws_col == 0 || window.ws_row == 0){
        //Fallback: push the cursor to the bottom-right corner and ask where it ended up
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12){
            return -1;
        }
        return getPointerPos(rows, cols);
    }else{
        *rows = window.ws_row;
        *cols = window.ws_col;
        return 0;
    }
}

void handleSigwinch(int sig){
    (void)sig;
    windowChanged = 1;
}

void handleResize(void){
    //Reads the new size and redraws; scroll() keeps the cursor on screen
    windowChanged = 0;
    if(getWindowSize(&editor.rows, &editor.cols) == -1){
        return;
    }
    editor.rows -= 2;
    if(editor.rows < 1) editor.rows = 1;
    refresh();
}

/*** syntax highlighting ***/

int isSeparator(int c){
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];{}:!&|^", c) != NULL;
}

void updateSyntax(textRow *row){
    row->hl = realloc(row->hl, row->rsize + 1);
    memset(row->hl, HL_NORMAL, row->rsize);

    if(editor.syntax == NULL) return;

    const char **keywords = editor.syntax->keywords;

    const char *scs = editor.syntax->singlelineCommentStart;
    const char *mcs = editor.syntax->multilineCommentStart;
    const char *mce = editor.syntax->multilineCommentEnd;
    int scsLen = scs ? strlen(scs) : 0;
    int mcsLen = mcs ? strlen(mcs) : 0;
    int mceLen = mce ? strlen(mce) : 0;

    int prevSep = 1;
    int inString = 0;
    //A /* comment */ that started on an earlier line carries on into this one
    int inComment = (row->idx > 0 && editor.row[row->idx - 1].hlOpenComment);

    int i = 0;
    while(i < row->rsize){
        char c = row->render[i];
        unsigned char prevHl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if(scsLen && !inString && !inComment){
            if(!strncmp(&row->render[i], scs, scsLen)){
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if(mcsLen && mceLen && !inString){
            if(inComment){
                row->hl[i] = HL_MLCOMMENT;
                if(!strncmp(&row->render[i], mce, mceLen)){
                    memset(&row->hl[i], HL_MLCOMMENT, mceLen);
                    i += mceLen;
                    inComment = 0;
                    prevSep = 1;
                    continue;
                }else{
                    i++;
                    continue;
                }
            }else if(!strncmp(&row->render[i], mcs, mcsLen)){
                memset(&row->hl[i], HL_MLCOMMENT, mcsLen);
                i += mcsLen;
                inComment = 1;
                continue;
            }
        }

        if(editor.syntax->flags & HL_HIGHLIGHT_STRINGS){
            if(inString){
                row->hl[i] = HL_STRING;
                //An escaped quote doesn't end the string
                if(c == '\\' && i + 1 < row->rsize){
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if(c == inString) inString = 0;
                i++;
                prevSep = 1;
                continue;
            }else if(c == '"' || c == '\''){
                inString = c;
                row->hl[i] = HL_STRING;
                i++;
                continue;
            }
        }

        if(editor.syntax->flags & HL_HIGHLIGHT_NUMBERS){
            if((isdigit((unsigned char)c) && (prevSep || prevHl == HL_NUMBER)) ||
               (c == '.' && prevHl == HL_NUMBER)){
                row->hl[i] = HL_NUMBER;
                i++;
                prevSep = 0;
                continue;
            }
        }

        //Keywords only count as whole words
        if(prevSep){
            int j;
            for(j = 0; keywords[j]; j++){
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if(kw2) klen--;

                if(!strncmp(&row->render[i], keywords[j], klen) &&
                   isSeparator((unsigned char)row->render[i + klen])){
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if(keywords[j] != NULL){
                prevSep = 0;
                continue;
            }
        }

        prevSep = isSeparator((unsigned char)c);
        i++;
    }

    //If this line opened or closed a comment, the next line's colours change too
    int changed = (row->hlOpenComment != inComment);
    row->hlOpenComment = inComment;
    if(changed && row->idx + 1 < editor.numRows){
        updateSyntax(&editor.row[row->idx + 1]);
    }
}

int syntaxToColor(int hl){
    switch(hl){
        case HL_COMMENT:
        case HL_MLCOMMENT: return 36;   // cyan
        case HL_KEYWORD1: return 33;    // yellow
        case HL_KEYWORD2: return 32;    // green
        case HL_STRING: return 35;      // magenta
        case HL_NUMBER: return 31;      // red
        case HL_MATCH: return 34;       // blue
        default: return 37;
    }
}

void selectSyntaxHighlight(void){
    //Picks the language from the file extension and re-colours every line
    editor.syntax = NULL;
    if(editor.filename == NULL) return;

    const char *ext = strrchr(editor.filename, '.');
    unsigned int j;
    for(j = 0; j < HLDB_ENTRIES; j++){
        struct editorSyntax *s = &HLDB[j];
        int i = 0;
        while(s->filematch[i]){
            if(ext && !strcmp(ext, s->filematch[i])){
                editor.syntax = s;
                int r;
                for(r = 0; r < editor.numRows; r++){
                    updateSyntax(&editor.row[r]);
                }
                return;
            }
            i++;
        }
    }
}

/*** rows ***/

int rowCxToRx(textRow *row, int cx){
    //Converts a position in the stored line to a position on screen, expanding tabs
    int rx = 0;
    int j;
    for(j = 0; j < cx; j++){
        if(row->chars[j] == '\t'){
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        }
        rx++;
    }
    return rx;
}

int rowRxToCx(textRow *row, int rx){
    int curRx = 0;
    int cx;
    for(cx = 0; cx < row->size; cx++){
        if(row->chars[cx] == '\t'){
            curRx += (TAB_STOP - 1) - (curRx % TAB_STOP);
        }
        curRx++;
        if(curRx > rx){
            return cx;
        }
    }
    return cx;
}

void updateRow(textRow *row){
    //Rebuilds the rendered copy of the line, turning tabs into spaces
    int tabs = 0;
    int j;
    for(j = 0; j < row->size; j++){
        if(row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

    int idx = 0;
    for(j = 0; j < row->size; j++){
        if(row->chars[j] == '\t'){
            row->render[idx++] = ' ';
            while(idx % TAB_STOP != 0) row->render[idx++] = ' ';
        }else{
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    updateSyntax(row);
}

void insertRow(int at, const char *s, size_t len){
    if(at < 0 || at > editor.numRows) return;

    editor.row = realloc(editor.row, sizeof(textRow) * (editor.numRows + 1));
    memmove(&editor.row[at + 1], &editor.row[at], sizeof(textRow) * (editor.numRows - at));
    int j;
    for(j = at + 1; j <= editor.numRows; j++) editor.row[j].idx++;

    editor.row[at].idx = at;
    editor.row[at].size = len;
    editor.row[at].chars = malloc(len + 1);
    memcpy(editor.row[at].chars, s, len);
    editor.row[at].chars[len] = '\0';

    editor.row[at].rsize = 0;
    editor.row[at].render = NULL;
    editor.row[at].hl = NULL;
    editor.row[at].hlOpenComment = 0;
    editor.numRows++;
    updateRow(&editor.row[at]);

    editor.dirty++;
}

void freeRow(textRow *row){
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void delRow(int at){
    if(at < 0 || at >= editor.numRows) return;
    freeRow(&editor.row[at]);
    memmove(&editor.row[at], &editor.row[at + 1], sizeof(textRow) * (editor.numRows - at - 1));
    int j;
    for(j = at; j < editor.numRows - 1; j++) editor.row[j].idx--;
    editor.numRows--;
    editor.dirty++;
}

void rowInsertChar(textRow *row, int at, int c){
    if(at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    updateRow(row);
    editor.dirty++;
}

void rowAppendString(textRow *row, const char *s, size_t len){
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    updateRow(row);
    editor.dirty++;
}

void rowDelChar(textRow *row, int at){
    if(at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    updateRow(row);
    editor.dirty++;
}

/*** editing ***/

void insertChar(int c){
    //Typing past the last line starts a new one
    if(editor.cy == editor.numRows){
        insertRow(editor.numRows, "", 0);
    }
    rowInsertChar(&editor.row[editor.cy], editor.cx, c);
    editor.cx++;
}

void insertNewline(void){
    if(editor.cx == 0){
        insertRow(editor.cy, "", 0);
    }else{
        //Splits the line at the cursor
        textRow *row = &editor.row[editor.cy];
        insertRow(editor.cy + 1, &row->chars[editor.cx], row->size - editor.cx);
        row = &editor.row[editor.cy]; //insertRow may have moved the rows in memory
        row->size = editor.cx;
        row->chars[row->size] = '\0';
        updateRow(row);
    }
    editor.cy++;
    editor.cx = 0;
}

void delChar(void){
    if(editor.cy == editor.numRows) return;
    if(editor.cx == 0 && editor.cy == 0) return;

    textRow *row = &editor.row[editor.cy];
    if(editor.cx > 0){
        rowDelChar(row, editor.cx - 1);
        editor.cx--;
    }else{
        //Backspace at the start of a line joins it onto the line above
        editor.cx = editor.row[editor.cy - 1].size;
        rowAppendString(&editor.row[editor.cy - 1], row->chars, row->size);
        delRow(editor.cy);
        editor.cy--;
    }
}

/*** undo and redo ***/

// A copy of the whole buffer and cursor, taken before each group of edits
typedef struct snapshot{
    int numRows;
    char **lines;
    int *sizes;
    int cx, cy;
} snapshot;

enum editKind{
    EDIT_NONE = 0,
    EDIT_INSERT,
    EDIT_DELETE,
    EDIT_NEWLINE
};

snapshot undoStack[UNDO_LIMIT];
int undoCount = 0;
snapshot redoStack[UNDO_LIMIT];
int redoCount = 0;

int lastEditKind = EDIT_NONE;
int lastEditCx = -1;
int lastEditCy = -1;

void takeSnapshot(snapshot *s){
    int n = editor.numRows > 0 ? editor.numRows : 1;
    s->numRows = editor.numRows;
    s->cx = editor.cx;
    s->cy = editor.cy;
    s->lines = malloc(sizeof(char *) * n);
    s->sizes = malloc(sizeof(int) * n);

    int j;
    for(j = 0; j < editor.numRows; j++){
        s->sizes[j] = editor.row[j].size;
        s->lines[j] = malloc(editor.row[j].size + 1);
        memcpy(s->lines[j], editor.row[j].chars, editor.row[j].size + 1);
    }
}

void freeSnapshot(snapshot *s){
    int j;
    for(j = 0; j < s->numRows; j++){
        free(s->lines[j]);
    }
    free(s->lines);
    free(s->sizes);
}

void restoreSnapshot(snapshot *s){
    while(editor.numRows > 0){
        delRow(editor.numRows - 1);
    }
    int j;
    for(j = 0; j < s->numRows; j++){
        insertRow(j, s->lines[j], s->sizes[j]);
    }

    editor.cy = s->cy;
    if(editor.cy > editor.numRows) editor.cy = editor.numRows;
    editor.cx = s->cx;
    int rowLen = (editor.cy < editor.numRows) ? editor.row[editor.cy].size : 0;
    if(editor.cx > rowLen) editor.cx = rowLen;
    editor.dirty++;
}

void pushSnapshot(snapshot *stack, int *count){
    //When the stack is full the oldest step is dropped
    if(*count == UNDO_LIMIT){
        freeSnapshot(&stack[0]);
        memmove(&stack[0], &stack[1], sizeof(snapshot) * (UNDO_LIMIT - 1));
        (*count)--;
    }
    takeSnapshot(&stack[*count]);
    (*count)++;
}

void clearRedo(void){
    while(redoCount > 0){
        redoCount--;
        freeSnapshot(&redoStack[redoCount]);
    }
}

void beginEdit(int kind, int c){
    //Typing a word is one undo step: a new step starts when the kind of edit changes, the cursor
    //has moved since the last edit, a line is split, or a space or tab is typed
    int newGroup = kind != lastEditKind ||
                   editor.cx != lastEditCx || editor.cy != lastEditCy ||
                   kind == EDIT_NEWLINE ||
                   (kind == EDIT_INSERT && (c == ' ' || c == '\t'));
    if(newGroup){
        pushSnapshot(undoStack, &undoCount);
    }
    clearRedo();
    lastEditKind = kind;
}

void endEdit(void){
    lastEditCx = editor.cx;
    lastEditCy = editor.cy;
}

void undo(void){
    if(undoCount == 0){
        setStatusMessage("Nothing to undo");
        return;
    }
    pushSnapshot(redoStack, &redoCount);
    undoCount--;
    restoreSnapshot(&undoStack[undoCount]);
    freeSnapshot(&undoStack[undoCount]);
    lastEditKind = EDIT_NONE;
}

void redo(void){
    if(redoCount == 0){
        setStatusMessage("Nothing to redo");
        return;
    }
    pushSnapshot(undoStack, &undoCount);
    redoCount--;
    restoreSnapshot(&redoStack[redoCount]);
    freeSnapshot(&redoStack[redoCount]);
    lastEditKind = EDIT_NONE;
}

/*** files ***/

char *rowsToString(int *bufLen){
    int totalLen = 0;
    int j;
    for(j = 0; j < editor.numRows; j++){
        totalLen += editor.row[j].size + 1;
    }
    *bufLen = totalLen;

    char *buf = malloc(totalLen + 1);
    char *p = buf;
    for(j = 0; j < editor.numRows; j++){
        memcpy(p, editor.row[j].chars, editor.row[j].size);
        p += editor.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void openFile(const char *filename){
    free(editor.filename);
    editor.filename = strdup(filename);

    selectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if(fp == NULL){
        //A file that doesn't exist yet is created on the first save
        if(errno == ENOENT){
            editor.dirty = 0;
            return;
        }
        exitP("openFile: fopen");
    }

    char *line = NULL;
    size_t lineCap = 0;
    ssize_t lineLen;
    while((lineLen = getline(&line, &lineCap, fp)) != -1){
        while(lineLen > 0 && (line[lineLen - 1] == '\n' || line[lineLen - 1] == '\r')){
            lineLen--;
        }
        insertRow(editor.numRows, line, lineLen);
    }
    free(line);
    fclose(fp);
    editor.dirty = 0;
}

void saveFile(void){
    if(editor.filename == NULL){
        editor.filename = prompt("Save as: %s (ESC to cancel)", NULL);
        if(editor.filename == NULL){
            setStatusMessage("Save cancelled");
            return;
        }
        selectSyntaxHighlight();
    }

    int len;
    char *buf = rowsToString(&len);

    int fd = open(editor.filename, O_RDWR | O_CREAT, 0644);
    if(fd != -1){
        if(ftruncate(fd, len) != -1){
            if(write(fd, buf, len) == len){
                close(fd);
                free(buf);
                editor.dirty = 0;
                setStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    setStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** search ***/

void findCallback(char *query, int key){
    static int lastMatch = -1;
    static int direction = 1;

    //Puts back the colours of the line that was highlighted as the previous match
    static int savedHlLine;
    static unsigned char *savedHl = NULL;
    if(savedHl){
        if(savedHlLine < editor.numRows){
            memcpy(editor.row[savedHlLine].hl, savedHl, editor.row[savedHlLine].rsize);
        }
        free(savedHl);
        savedHl = NULL;
    }

    if(key == '\r' || key == '\x1b'){
        lastMatch = -1;
        direction = 1;
        return;
    }else if(key == ARROW_RIGHT || key == ARROW_DOWN){
        direction = 1;
    }else if(key == ARROW_LEFT || key == ARROW_UP){
        direction = -1;
    }else{
        //The query changed, so start again from the top
        lastMatch = -1;
        direction = 1;
    }

    if(lastMatch == -1) direction = 1;
    int current = lastMatch;
    int i;
    for(i = 0; i < editor.numRows; i++){
        current += direction;
        if(current == -1) current = editor.numRows - 1;
        else if(current == editor.numRows) current = 0;

        textRow *row = &editor.row[current];
        char *match = strstr(row->render, query);
        if(match){
            lastMatch = current;
            editor.cy = current;
            editor.cx = rowRxToCx(row, match - row->render);
            editor.rowOff = editor.numRows; //scroll() then puts the match at the top of the screen

            savedHlLine = current;
            savedHl = malloc(row->rsize + 1);
            memcpy(savedHl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void find(void){
    int savedCx = editor.cx;
    int savedCy = editor.cy;
    int savedColOff = editor.colOff;
    int savedRowOff = editor.rowOff;

    char *query = prompt("Search: %s (ESC to cancel, arrows for previous/next)", findCallback);

    if(query){
        free(query);
    }else{
        //Cancelled: go back to where the search started
        editor.cx = savedCx;
        editor.cy = savedCy;
        editor.colOff = savedColOff;
        editor.rowOff = savedRowOff;
    }
}

/*** output ***/

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
    ab->len += len;
}

void abFree(struct appendBuffer *ab){
    free(ab->b);
}

void scroll(void){
    //Keeps the cursor on screen by moving the visible window
    editor.rx = 0;
    if(editor.cy < editor.numRows){
        editor.rx = rowCxToRx(&editor.row[editor.cy], editor.cx);
    }

    if(editor.cy < editor.rowOff){
        editor.rowOff = editor.cy;
    }
    if(editor.cy >= editor.rowOff + editor.rows){
        editor.rowOff = editor.cy - editor.rows + 1;
    }
    if(editor.rx < editor.colOff){
        editor.colOff = editor.rx;
    }
    if(editor.rx >= editor.colOff + editor.cols){
        editor.colOff = editor.rx - editor.cols + 1;
    }
}

void drawRows(struct appendBuffer *ab){
    int y;
    for(y = 0; y < editor.rows; y++){
        int fileRow = y + editor.rowOff;
        if(fileRow >= editor.numRows){
            if(editor.numRows == 0 && editor.filename == NULL && y == editor.rows / 3){
                char welcome[80];
                int welcomeLen = snprintf(welcome, sizeof(welcome), "curate -- version %s", CURATE_VERSION);
                if(welcomeLen > editor.cols) welcomeLen = editor.cols;
                int padding = (editor.cols - welcomeLen) / 2;
                if(padding){
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while(padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomeLen);
            }else{
                abAppend(ab, "~", 1);
            }
        }else{
            int len = editor.row[fileRow].rsize - editor.colOff;
            if(len > editor.cols) len = editor.cols;
            if(len > 0){
                char *c = &editor.row[fileRow].render[editor.colOff];
                unsigned char *hl = &editor.row[fileRow].hl[editor.colOff];
                int currentColor = -1;
                int j;
                for(j = 0; j < len; j++){
                    if(iscntrl((unsigned char)c[j])){
                        //Control characters show as an inverted letter, e.g. Ctrl-A as A
                        char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                        abAppend(ab, "\x1b[7m", 4);
                        abAppend(ab, &sym, 1);
                        abAppend(ab, "\x1b[m", 3);
                        if(currentColor != -1){
                            char buf[16];
                            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", currentColor);
                            abAppend(ab, buf, clen);
                        }
                    }else if(hl[j] == HL_NORMAL){
                        if(currentColor != -1){
                            abAppend(ab, "\x1b[39m", 5);
                            currentColor = -1;
                        }
                        abAppend(ab, &c[j], 1);
                    }else{
                        //Only switch colour when it changes, to keep the output short
                        int color = syntaxToColor(hl[j]);
                        if(color != currentColor){
                            currentColor = color;
                            char buf[16];
                            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                            abAppend(ab, buf, clen);
                        }
                        abAppend(ab, &c[j], 1);
                    }
                }
                abAppend(ab, "\x1b[39m", 5);
            }
        }

        abAppend(ab, "\x1b[K", 3); //clears the rest of the line
        abAppend(ab, "\r\n", 2);
    }
}

void drawStatusBar(struct appendBuffer *ab){
    abAppend(ab, "\x1b[7m", 4); //inverted colours

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        editor.filename ? editor.filename : "[No Name]", editor.numRows,
        editor.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
        editor.syntax ? editor.syntax->filetype : "plain text", editor.cy + 1, editor.numRows);

    if(len > editor.cols) len = editor.cols;
    abAppend(ab, status, len);
    while(len < editor.cols){
        if(editor.cols - len == rlen){
            abAppend(ab, rstatus, rlen);
            break;
        }
        abAppend(ab, " ", 1);
        len++;
    }

    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void drawMessageBar(struct appendBuffer *ab){
    abAppend(ab, "\x1b[K", 3);
    int msgLen = strlen(editor.statusMsg);
    if(msgLen > editor.cols) msgLen = editor.cols;
    //Messages disappear after 5 seconds
    if(msgLen && time(NULL) - editor.statusTime < 5){
        abAppend(ab, editor.statusMsg, msgLen);
    }
}

void refresh(void){
    scroll();

    //Builds the whole frame in one buffer and writes it at once, so the screen doesn't flicker
    struct appendBuffer ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); //hides the cursor while drawing
    abAppend(&ab, "\x1b[H", 3);

    drawRows(&ab);
    drawStatusBar(&ab);
    drawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (editor.cy - editor.rowOff) + 1, (editor.rx - editor.colOff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void setStatusMessage(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(editor.statusMsg, sizeof(editor.statusMsg), fmt, ap);
    va_end(ap);
    editor.statusTime = time(NULL);
}

/*** input ***/

char *prompt(const char *message, void (*callback)(char *, int)){
    //Reads a line of text in the message bar; message has a %s where the typed text goes
    size_t bufSize = 128;
    char *buf = malloc(bufSize);
    size_t bufLen = 0;
    buf[0] = '\0';

    while(1){
        setStatusMessage(message, buf);
        refresh();

        int c = keyReading();
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if(bufLen != 0) buf[--bufLen] = '\0';
        }else if(c == '\x1b'){
            setStatusMessage("");
            if(callback) callback(buf, c);
            free(buf);
            return NULL;
        }else if(c == '\r'){
            if(bufLen != 0){
                setStatusMessage("");
                if(callback) callback(buf, c);
                return buf;
            }
        }else if(c < 128 && !iscntrl(c)){
            if(bufLen == bufSize - 1){
                bufSize *= 2;
                buf = realloc(buf, bufSize);
            }
            buf[bufLen++] = c;
            buf[bufLen] = '\0';
        }

        if(callback) callback(buf, c);
    }
}

void moveCursor(int key){
    textRow *row = (editor.cy >= editor.numRows) ? NULL : &editor.row[editor.cy];

    switch(key){
        case ARROW_LEFT:
            if(editor.cx != 0){
                editor.cx--;
            }else if(editor.cy > 0){
                //Wraps to the end of the previous line
                editor.cy--;
                editor.cx = editor.row[editor.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if(row && editor.cx < row->size){
                editor.cx++;
            }else if(row && editor.cx == row->size){
                //Wraps to the start of the next line
                editor.cy++;
                editor.cx = 0;
            }
            break;
        case ARROW_UP:
            if(editor.cy != 0) editor.cy--;
            break;
        case ARROW_DOWN:
            if(editor.cy < editor.numRows) editor.cy++;
            break;
    }

    //Moving onto a shorter line snaps the cursor to its end
    row = (editor.cy >= editor.numRows) ? NULL : &editor.row[editor.cy];
    int rowLen = row ? row->size : 0;
    if(editor.cx > rowLen){
        editor.cx = rowLen;
    }
}

void letterProcessing(void){
    static int quitTimes = QUIT_TIMES;

    int c = keyReading();

    switch(c){
        case '\r':
            beginEdit(EDIT_NEWLINE, c);
            insertNewline();
            endEdit();
            break;

        //Exits program on Ctrl+Q (asks again first if there are unsaved changes)
        case CTRL_KEY('q'):
            if(editor.dirty && quitTimes > 0){
                setStatusMessage("Unsaved changes! Press Ctrl-Q again to quit without saving.");
                quitTimes--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            saveFile();
            break;

        case CTRL_KEY('f'):
            find();
            break;

        case CTRL_KEY('z'):
            undo();
            break;

        case CTRL_KEY('y'):
            redo();
            break;

        case HOME_KEY:
            editor.cx = 0;
            break;

        case END_KEY:
            if(editor.cy < editor.numRows){
                editor.cx = editor.row[editor.cy].size;
            }
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c == DEL_KEY) moveCursor(ARROW_RIGHT);
            //Nothing to delete before the start of the file or after its end
            if(editor.cy == editor.numRows || (editor.cx == 0 && editor.cy == 0)) break;
            beginEdit(EDIT_DELETE, c);
            delChar();
            endEdit();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if(c == PAGE_UP){
                    editor.cy = editor.rowOff;
                }else{
                    editor.cy = editor.rowOff + editor.rows - 1;
                    if(editor.cy > editor.numRows) editor.cy = editor.numRows;
                }
                int times = editor.rows;
                while(times--){
                    moveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            moveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            beginEdit(EDIT_INSERT, c);
            insertChar(c);
            endEdit();
            break;
    }

    quitTimes = QUIT_TIMES;
}

/*** init ***/

void initEditor(void){
    editor.cx = 0;
    editor.cy = 0;
    editor.rx = 0;
    editor.rowOff = 0;
    editor.colOff = 0;
    editor.numRows = 0;
    editor.row = NULL;
    editor.dirty = 0;
    editor.filename = NULL;
    editor.statusMsg[0] = '\0';
    editor.statusTime = 0;
    editor.syntax = NULL;

    if(getWindowSize(&editor.rows, &editor.cols) == -1){
        exitP("initEditor: getWindowSize");
    }
    editor.rows -= 2; //room for the status bar and the message bar

    //Redraw when the terminal is resized (no SA_RESTART, so a resize interrupts the key read)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleSigwinch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);
}

int main(int argc, char *argv[]){

    //Enables custom raw processing mode
    enableRaw();
    initEditor();
    if(argc >= 2){
        openFile(argv[1]);
    }

    setStatusMessage("HELP: ^S save | ^Q quit | ^F find | ^Z undo | ^Y redo");

    while(1){
        refresh();
        letterProcessing();
    }

    return 0;
}
