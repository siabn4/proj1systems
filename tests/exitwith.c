/* Sleep for av[1] seconds, then exit with av[2] */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int ac, char *av[])
{
    sleep(atoi(av[1]));
    exit(atoi(av[2]));
}
