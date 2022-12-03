#include <stdio.h>
#include "softwaredisk.h"
#include "softwaredisk.c"
#include "filesystem.c"
#include "filesystem.h"


int main() {
    printf("Checking structure alignment...\n");
    if(!check_structure_alignment())
    {
        printf("Check failed. Do not use filesystem.\n");
    }
    else
    {
        printf("Check succeeded.\n");
        printf("Initializing software disk.\n");
        init_software_disk();
    }
    return 0;
}