encode_vobs: encode_vobs.c
	gcc -Wall -Wextra -g -std=c99 -pedantic -O2 -o encode_vobs encode_vobs.c

clean:
	rm -f encode_vobs
