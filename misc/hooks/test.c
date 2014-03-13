#include <stdio.h>
#include "zsim_hooks.h"

int main() {
    printf("C test\n");
    zsim_roi_begin();
    zsim_heartbeat();
    zsim_roi_end();
    printf("C test done\n");
    return 0;
}
