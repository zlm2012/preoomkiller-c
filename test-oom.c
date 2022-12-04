#include<stdlib.h>
#include<unistd.h>
#include<stdio.h>
#include<signal.h>
#include<string.h>

int main() {
    char *temp = malloc(80000000);
    memset(temp, 'a', 80000000);
    while (1) {
       temp = malloc(5000000);
       memset(temp, 'b', 5000000);
       sleep(1);
    }
    return 0;
}
