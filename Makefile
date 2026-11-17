OPT = -g -Wall -Wextra -Werror -pedantic -std=c99

curate: curate.c
	$(CC) curate.c -o curate $(OPT)

clean:
	rm -rf curate curate.dSYM app app.dSYM

.PHONY: clean
