OPT = -g -Wall -Wextra -Werror -pedantic -std=c99

app: curate.c
	$(CC) curate.c -o app $(OPT)