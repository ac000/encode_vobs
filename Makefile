encode_vobs: encode_vobs.c
	gcc -Wall -g -std=c99 -O2 -o encode_vobs encode_vobs.c

clean:
	rm encode_vobs
