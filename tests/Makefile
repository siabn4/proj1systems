all: die writetostderr exitwith

die: die.c
	gcc -o die die.c

writetostderr: writetostderr.c
	gcc -o writetostderr writetostderr.c

exitwith: exitwith.c
	gcc -o exitwith exitwith.c

clean:
	rm -f die writetostderr
